// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "ApiSends.h"

#include "MessageTrace.h"
#include "Time.h"
#include "../Application/Application.h"
#include "../Application/IniFileSettings.h"
#include "../BO/Message.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/DatabaseSettings.h"
#include "../SQL/DatabaseConnectionManager.h"

#include <ctime>
#include <map>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The six words a state is, spelled once.
      const wchar_t *StateQueued = L"queued";
      const wchar_t *StateDeferred = L"deferred";
      const wchar_t *StateDelivered = L"delivered";
      const wchar_t *StateBounced = L"bounced";
      const wchar_t *StateRefused = L"refused";
      const wchar_t *StateUnknown = L"unknown";

      String Lower(const String &value)
      {
         String copy = value;
         copy.MakeLower();
         return copy;
      }

      // The event's verdict on the address, with what the trace holds about it.
      void Decide(ApiSendRecipientState &state, const wchar_t *verdict, const MessageTraceEvent &traceEvent)
      {
         state.state = verdict;
         state.when = traceEvent.occurred;
         state.code = traceEvent.status_code;
         MessageTrace::SplitDetail(traceEvent.detail, state.status, state.reason);
      }
   }

   bool
   ApiSends::Record(const String &keyId, __int64 queueId, const String &messageId,
                    const String &sender, const std::vector<String> &recipients)
   {
      // Whole addresses, as many as the column holds. One that would not fit is
      // left out rather than cut: a cut address is somebody else's address. The
      // queue and the trace still name it, and Describe lists it from them.
      String joined;
      for (const String &address : recipients)
      {
         const int needed = (joined.IsEmpty() ? 0 : 1) + address.GetLength();
         if (joined.GetLength() + needed > RecipientsMaximum)
            break;

         if (!joined.IsEmpty())
            joined += _T(",");
         joined += address;
      }

      SQLStatement statement;
      statement.SetTable("hm_apisends");
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("apsid");
      statement.AddColumn("apskeyid", keyId.Mid(0, 32));
      statement.AddColumnInt64("apsqueueid", queueId);
      statement.AddColumn("apsmessageid", messageId.Mid(0, 255));
      statement.AddColumn("apssender", sender.Mid(0, 255));
      statement.AddColumn("apsrecipients", joined);
      statement.AddColumnDate("apscreated", Time::GetDateFromSystemDate(Time::GetCurrentDateTime()));

      __int64 insertedId = 0;
      return Application::Instance()->GetDBManager()->Execute(statement, &insertedId);
   }

   bool
   ApiSends::ReadRow_(std::shared_ptr<DALRecordset> recordset, ApiSendRecord &record)
   {
      if (!recordset || recordset->IsEOF())
         return false;

      record.id = recordset->GetInt64Value("apsid");
      record.key_id = recordset->GetStringValue("apskeyid");
      record.queue_id = recordset->GetInt64Value("apsqueueid");
      record.message_id = recordset->GetStringValue("apsmessageid");
      record.sender = recordset->GetStringValue("apssender");
      record.created = recordset->GetStringValue("apscreated");

      record.recipients.clear();
      for (const String &address : StringParser::SplitString(recordset->GetStringValue("apsrecipients"), _T(",")))
      {
         if (!address.IsEmpty())
            record.recipients.push_back(address);
      }

      return true;
   }

   bool
   ApiSends::Find(const String &keyId, __int64 queueId, ApiSendRecord &record)
   {
      if (keyId.IsEmpty() || queueId <= 0)
         return false;

      // The key is in the WHERE, not checked afterwards: a send another key made
      // is never read, so there is nothing about it to leak by accident.
      SQLCommand command("select * from hm_apisends where apskeyid = @KEYID and apsqueueid = @QUEUEID");
      command.AddParameter("@KEYID", keyId);
      command.AddParameter("@QUEUEID", queueId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

      return ReadRow_(recordset, record);
   }

   bool
   ApiSends::List(const String &keyId, int offset, int count, std::vector<ApiSendRecord> &records, int &total)
   {
      records.clear();
      total = 0;

      if (keyId.IsEmpty() || offset < 0 || count <= 0)
         return true;

      SQLCommand countCommand("select count(*) as apscount from hm_apisends where apskeyid = @KEYID");
      countCommand.AddParameter("@KEYID", keyId);

      std::shared_ptr<DALRecordset> counted = Application::Instance()->GetDBManager()->OpenRecordset(countCommand);
      if (!counted)
         return false;

      total = counted->IsEOF() ? 0 : (int) counted->GetLongValue("apscount");

      if (offset >= total)
         return true;

      // Newest first by the identity column, which is monotonic by construction,
      // and not by apscreated, whose one-second resolution ties every send of a
      // busy second. The first offset + count rows are read and the first offset
      // skipped: the caller bounds the two (RestApiSend.cpp), and TOP and LIMIT
      // are the forms every backend this server supports takes.
      const int rows = offset + count;
      String text;

      switch (IniFileSettings::Instance()->GetDatabaseType())
      {
      case DatabaseSettings::TypeMSSQLServer:
      case DatabaseSettings::TypeMSSQLCompactEdition:
         // TOP (n) rather than TOP n: SQL Server Compact requires the parentheses.
         text.Format(_T("select top (%d) * from hm_apisends where apskeyid = @KEYID order by apsid desc"), rows);
         break;
      default:
         text.Format(_T("select * from hm_apisends where apskeyid = @KEYID order by apsid desc limit %d"), rows);
         break;
      }

      SQLCommand command(text);
      command.AddParameter("@KEYID", keyId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      int index = 0;
      while (!recordset->IsEOF() && (int) records.size() < count)
      {
         if (index >= offset)
         {
            ApiSendRecord record;
            if (ReadRow_(recordset, record))
               records.push_back(record);
         }

         index++;
         recordset->MoveNext();
      }

      return true;
   }

   int
   ApiSends::DeleteExpired()
   {
      // The trace's own retention, because a send is only as answerable as the
      // trace's events for it: a row kept longer than those would say "unknown"
      // about everything it names.
      const int retentionDays = IniFileSettings::Instance()->GetMessageTraceRetentionDays();

      if (retentionDays <= 0)
         return 0;

      std::time_t cutoffTime = std::time(0) - (static_cast<std::time_t>(retentionDays) * 24 * 60 * 60);

      struct tm cutoffTm;

      if (localtime_s(&cutoffTm, &cutoffTime) != 0)
         return 0;

      char cutoffText[32];

      if (strftime(cutoffText, sizeof(cutoffText), "%Y-%m-%d %H:%M:%S", &cutoffTm) == 0)
         return 0;

      SQLCommand countCommand("select count(*) as apscount from hm_apisends where apscreated < @CUTOFF");
      countCommand.AddParameter("@CUTOFF", String(cutoffText));

      std::shared_ptr<DALRecordset> counted = Application::Instance()->GetDBManager()->OpenRecordset(countCommand);

      const int expired = (counted && !counted->IsEOF()) ? (int) counted->GetLongValue("apscount") : 0;

      if (expired == 0)
         return 0;

      SQLCommand deleteCommand("delete from hm_apisends where apscreated < @CUTOFF");
      deleteCommand.AddParameter("@CUTOFF", String(cutoffText));

      if (!Application::Instance()->GetDBManager()->Execute(deleteCommand))
         return 0;

      return expired;
   }

   String
   ApiSends::Describe(const ApiSendRecord &record, std::vector<ApiSendRecipientState> &recipients, bool &traceEnabled)
   {
      traceEnabled = MessageTrace::GetEnabled();
      recipients.clear();

      // One entry per address, by position in recipients and looked up by the
      // address in lower case: first every address the body named, in its
      // order, then any address the trace or the queue names that the body
      // did not - what an alias or a list resolved to.
      std::map<String, size_t> index;
      auto entryFor = [&recipients, &index](const String &address) -> size_t
      {
         const String key = Lower(address);
         auto found = index.find(key);
         if (found != index.end())
            return found->second;

         ApiSendRecipientState entry;
         entry.address = address;
         recipients.push_back(entry);
         index[key] = recipients.size() - 1;
         return recipients.size() - 1;
      };

      for (const String &address : record.recipients)
         entryFor(address);

      // The trace's events for the queue id, oldest first, so that the last
      // word on an address is the one that stands - with one exception: a
      // refusal by the suppression list is followed by the failure the refusal
      // reports to the sender, and the refusal is the truer name for it.
      for (const MessageTraceEvent &traceEvent : MessageTrace::GetByQueueID(record.queue_id))
      {
         if (traceEvent.recipient.IsEmpty())
            continue;

         ApiSendRecipientState &state = recipients[entryFor(traceEvent.recipient)];
         const String &name = traceEvent.event_name;

         if (name.CompareNoCase(MessageTrace::EventSuppressed) == 0)
         {
            // 250 is the suppression list observing, and the delivery follows.
            if (traceEvent.status_code >= 500)
               Decide(state, StateRefused, traceEvent);
         }
         else if (state.state.Compare(StateRefused) == 0)
         {
            continue;
         }
         else if (name.CompareNoCase(MessageTrace::EventDelivered) == 0)
            Decide(state, StateDelivered, traceEvent);
         else if (name.CompareNoCase(MessageTrace::EventFailed) == 0)
            Decide(state, StateBounced, traceEvent);
         else if (name.CompareNoCase(MessageTrace::EventDeferred) == 0)
         {
            // A deferral never overrides a verdict: an attempt that is retried
            // and then delivered reads delivered.
            if (state.state.IsEmpty() || state.state.Compare(StateDeferred) == 0)
               Decide(state, StateDeferred, traceEvent);
         }
      }

      // What is still waiting: the message's row while it is one of the queue's
      // - delivering (1), held for ETRN (3) or held for review (4) - and the
      // recipients it still has. A message delivered to one local account has
      // become that account's copy (type 2) and is no longer the queue's.
      bool inQueue = false;
      int tries = 0;
      {
         SQLCommand command("select messagetype, messagecurnooftries from hm_messages where messageid = @MESSAGEID");
         command.AddParameter("@MESSAGEID", record.queue_id);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (recordset && !recordset->IsEOF())
         {
            const int type = (int) recordset->GetLongValue("messagetype");
            inQueue = type == (int) Message::Delivering || type == 3 || type == (int) Message::HeldForReview;
            tries = (int) recordset->GetLongValue("messagecurnooftries");
         }
      }

      if (inQueue)
      {
         SQLCommand command("select recipientaddress from hm_messagerecipients where recipientmessageid = @MESSAGEID");
         command.AddParameter("@MESSAGEID", record.queue_id);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         while (recordset && !recordset->IsEOF())
         {
            ApiSendRecipientState &state = recipients[entryFor(recordset->GetStringValue("recipientaddress"))];

            // Still in the queue and no verdict: waiting. Tried already (a
            // deferral the trace did not record, because it was off) is
            // deferred; not tried yet is queued.
            if (state.state.IsEmpty())
            {
               state.state = tries > 0 ? StateDeferred : StateQueued;
               if (tries > 0)
                  state.reason.Format(_T("Tried %d time(s) and waiting for the next attempt."), tries);
            }

            recordset->MoveNext();
         }
      }

      // Whatever nothing named: unknown, and why, in the one sentence that is true.
      for (ApiSendRecipientState &state : recipients)
      {
         if (!state.state.IsEmpty())
            continue;

         state.state = StateUnknown;

         if (inQueue)
            state.reason = _T("This address resolved to others - an alias, a distribution list - which are listed under their own addresses.");
         else if (!traceEnabled)
            state.reason = _T("The message has left the queue and the message trace is off (MessageTraceEnabled), so nothing recorded what became of it at this address.");
         else
            state.reason = _T("The message has left the queue and the trace names no event for this address: it resolved to others (an alias, a list, a forward), listed under their own addresses, or the trace's rows for it have been removed.");
      }

      // The send's own state: the least finished of its recipients' while any
      // waits, and otherwise the worst of what happened.
      bool queued = false, deferred = false, delivered = false, bounced = false, refused = false;
      for (const ApiSendRecipientState &state : recipients)
      {
         queued = queued || state.state.Compare(StateQueued) == 0;
         deferred = deferred || state.state.Compare(StateDeferred) == 0;
         delivered = delivered || state.state.Compare(StateDelivered) == 0;
         bounced = bounced || state.state.Compare(StateBounced) == 0;
         refused = refused || state.state.Compare(StateRefused) == 0;
      }

      if (deferred)
         return StateDeferred;
      if (queued)
         return StateQueued;
      if (bounced)
         return StateBounced;
      if (refused)
         return StateRefused;
      if (delivered)
         return StateDelivered;
      return StateUnknown;
   }
}
