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
#include <set>

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
      // key's answers name only what is kept here (Describe answers for the
      // addresses the body named, and this is its record of them), so an address
      // left out is one the key is not answered about. Lower-cased, as the
      // sender is, so that an erasure's LIKE finds it on a backend that compares
      // case (AddressTraceEraser, EraseAddress).
      String joined;
      for (const String &address : recipients)
      {
         const int needed = (joined.IsEmpty() ? 0 : 1) + address.GetLength();
         if (joined.GetLength() + needed > RecipientsMaximum)
            break;

         if (!joined.IsEmpty())
            joined += _T(",");
         joined += Lower(address);
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

   int
   ApiSends::CountThenDelete_(const AnsiString &where, const AnsiString &parameter, const String &value)
   {
      // Counted first so the caller can say what went, then deleted in one
      // statement: the DAL answers success, not affected rows.
      SQLCommand countCommand("select count(*) as apscount from hm_apisends where " + where);
      countCommand.AddParameter(parameter, value);

      std::shared_ptr<DALRecordset> counted = Application::Instance()->GetDBManager()->OpenRecordset(countCommand);
      if (!counted)
         return -1;

      const int count = counted->IsEOF() ? 0 : (int) counted->GetLongValue("apscount");
      if (count == 0)
         return 0;

      SQLCommand deleteCommand("delete from hm_apisends where " + where);
      deleteCommand.AddParameter(parameter, value);

      if (!Application::Instance()->GetDBManager()->Execute(deleteCommand))
         return -1;

      return count;
   }

   int
   ApiSends::DeleteForKey(const String &keyId)
   {
      if (keyId.IsEmpty())
         return 0;

      return CountThenDelete_("apskeyid = @KEYID", "@KEYID", keyId);
   }

   int
   ApiSends::DeleteOrphans(const std::vector<String> &liveKeyIds)
   {
      std::set<String> live;
      for (const String &id : liveKeyIds)
         live.insert(Lower(id));

      SQLCommand command("select distinct apskeyid from hm_apisends");

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return -1;

      std::vector<String> orphans;
      while (!recordset->IsEOF())
      {
         const String id = recordset->GetStringValue("apskeyid");
         if (live.find(Lower(id)) == live.end())
            orphans.push_back(id);

         recordset->MoveNext();
      }

      int removed = 0;
      for (const String &id : orphans)
      {
         const int count = DeleteForKey(id);
         if (count < 0)
            return -1;
         removed += count;
      }

      return removed;
   }

   int
   ApiSends::EraseAddress(const String &address)
   {
      if (address.IsEmpty())
         return 0;

      const String lowered = Lower(address);

      // What the address sent. The sender is stored lower-cased (RestApiSend.cpp
      // lower-cases from before it is recorded).
      int changed = CountThenDelete_("apssender = @SENDER", "@SENDER", lowered);
      if (changed < 0)
         return -1;

      // What it was sent. The recipients are a comma-separated list, so LIKE only
      // finds the candidates and each is matched whole in memory - LIKE alone would
      // also match inside a longer address (ann@ in joann@). Each is rewritten
      // without the address, keeping the key's record of the others, and a row
      // left naming nobody goes.
      SQLCommand candidates("select apsid, apsrecipients from hm_apisends where apsrecipients like @PATTERN");
      candidates.AddParameter("@PATTERN", _T("%") + lowered + _T("%"));

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(candidates);
      if (!recordset)
         return -1;

      std::vector<std::pair<__int64, String> > rewrites;
      while (!recordset->IsEOF())
      {
         bool found = false;
         String kept;

         for (const String &entry : StringParser::SplitString(recordset->GetStringValue("apsrecipients"), _T(",")))
         {
            if (entry.IsEmpty())
               continue;

            if (entry.CompareNoCase(lowered) == 0)
            {
               found = true;
               continue;
            }

            if (!kept.IsEmpty())
               kept += _T(",");
            kept += entry;
         }

         if (found)
            rewrites.push_back(std::make_pair(recordset->GetInt64Value("apsid"), kept));

         recordset->MoveNext();
      }

      for (const std::pair<__int64, String> &rewrite : rewrites)
      {
         bool done = false;

         if (rewrite.second.IsEmpty())
         {
            SQLCommand remove("delete from hm_apisends where apsid = @ID");
            remove.AddParameter("@ID", rewrite.first);
            done = Application::Instance()->GetDBManager()->Execute(remove);
         }
         else
         {
            SQLCommand update("update hm_apisends set apsrecipients = @RECIPIENTS where apsid = @ID");
            update.AddParameter("@RECIPIENTS", rewrite.second);
            update.AddParameter("@ID", rewrite.first);
            done = Application::Instance()->GetDBManager()->Execute(update);
         }

         if (!done)
            return -1;

         changed++;
      }

      return changed;
   }

   namespace
   {
      // Which of two states a named address answers when several addresses it
      // resolved to hold states of their own: one still waiting first, then the
      // worst of what happened.
      int Precedence(const String &state)
      {
         if (state.Compare(StateDeferred) == 0) return 6;
         if (state.Compare(StateQueued) == 0) return 5;
         if (state.Compare(StateBounced) == 0) return 4;
         if (state.Compare(StateRefused) == 0) return 3;
         if (state.Compare(StateDelivered) == 0) return 2;
         return 1;
      }
   }

   String
   ApiSends::Describe(const ApiSendRecord &record, std::vector<ApiSendRecipientState> &recipients, bool &traceEnabled)
   {
      traceEnabled = MessageTrace::GetEnabled();
      recipients.clear();

      // The addresses the body named, once each and in its order, and nothing
      // else: see the top of ApiSends.h.
      std::map<String, size_t> named;
      for (const String &address : record.recipients)
      {
         const String key = Lower(address);
         if (key.IsEmpty() || named.find(key) != named.end())
            continue;

         ApiSendRecipientState entry;
         entry.address = address;
         recipients.push_back(entry);
         named[key] = recipients.size() - 1;
      }

      // The message's row while it is one of the queue's - delivering (1), held
      // for ETRN (3) or held for review (4); a message delivered to one local
      // account has become that account's copy (type 2) and is no longer the
      // queue's - and how often it has been tried.
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

      // Each address the queue or the trace names, lower-cased, mapped to the
      // named address it stands for: itself when the body named it, and - while
      // the queue holds it - the original address its queue row keeps, which is
      // what the body named when it resolved to others.
      std::map<String, String> origin;
      std::vector<String> waiting;

      if (inQueue)
      {
         SQLCommand command("select recipientaddress, recipientoriginaladdress from hm_messagerecipients where recipientmessageid = @MESSAGEID");
         command.AddParameter("@MESSAGEID", record.queue_id);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         while (recordset && !recordset->IsEOF())
         {
            const String address = Lower(recordset->GetStringValue("recipientaddress"));
            String original = Lower(recordset->GetStringValue("recipientoriginaladdress"));

            // An address the body named stands for itself, even where it was
            // reached first through another named address - a member of a list
            // the body also named - whose name its queue row then keeps.
            if (original.IsEmpty() || named.find(address) != named.end())
               original = address;

            if (!address.IsEmpty() && named.find(original) != named.end())
            {
               origin[address] = original;
               waiting.push_back(address);
            }

            recordset->MoveNext();
         }
      }

      // The trace's word on each address, oldest first so that the last word
      // stands - with one exception: a refusal by the suppression list is followed
      // by the failure the refusal reports to the sender, and the refusal is the
      // truer name for it. An event about an address nothing maps to a named one
      // is left where it is.
      std::map<String, ApiSendRecipientState> actual;

      for (const MessageTraceEvent &traceEvent : MessageTrace::GetLatestByQueueID(record.queue_id, MaxEvents))
      {
         const String address = Lower(traceEvent.recipient);
         if (address.IsEmpty())
            continue;

         if (origin.find(address) == origin.end())
         {
            if (named.find(address) == named.end())
               continue;

            origin[address] = address;
         }

         ApiSendRecipientState &state = actual[address];
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

      // Still in the queue and no verdict: waiting. Tried already - a deferral the
      // trace did not record, because it was off - is deferred; not yet, queued.
      for (const String &address : waiting)
      {
         ApiSendRecipientState &state = actual[address];
         if (!state.state.IsEmpty())
            continue;

         state.state = tries > 0 ? StateDeferred : StateQueued;
         if (tries > 0)
            state.reason.Format(_T("Tried %d time(s) and waiting for the next attempt."), tries);
      }

      // Folded into the named addresses. An address the body named answers its own
      // state whole; one that resolved to others answers the state they hold taken
      // together - the one still waiting, or else the worst - with no status, code
      // or reply of theirs, which could name them.
      for (const auto &entry : actual)
      {
         if (entry.second.state.IsEmpty())
            continue;

         const String &original = origin[entry.first];
         const size_t index = named[original];
         ApiSendRecipientState &target = recipients[index];
         const bool itself = entry.first.Compare(original) == 0;

         if (Precedence(entry.second.state) < Precedence(target.state))
            continue;

         const String address = target.address;
         target = entry.second;
         target.address = address;

         if (!itself)
         {
            target.status = _T("");
            target.code = 0;
            target.reason = _T("This address resolved to other addresses on this server; the state is theirs, taken together, and they are not named.");
         }
      }

      for (size_t index = 0; index < recipients.size(); index++)
      {
         ApiSendRecipientState &state = recipients[index];

         if (!state.state.IsEmpty())
            continue;

         // Whatever nothing named: unknown, and why, in the sentence that is true
         // of where the message stands.
         state.state = StateUnknown;

         if (inQueue && !traceEnabled)
            state.reason = _T("No longer waiting at this address. The message trace is off (MessageTraceEnabled), so nothing recorded what became of it here: it may have been delivered, bounced or refused, or it resolved to other addresses, which are not named.");
         else if (inQueue)
            state.reason = _T("No longer waiting at this address, and the trace names no event for it: it was refused or removed on this server without a trace event (a full mailbox, a rule, a virus scan), or it resolved to other addresses (an alias, a list), which are not named.");
         else if (!traceEnabled)
            state.reason = _T("The message has left the queue and the message trace is off (MessageTraceEnabled), so nothing recorded what became of it at this address.");
         else
            state.reason = _T("The message has left the queue and the trace names no event for this address: it was refused or removed on this server without a trace event (a full mailbox, a rule, a virus scan), it resolved to other addresses (an alias, a list, a forward), which are not named, or the trace's rows for it have been removed.");
      }

      // The send's own state: the least finished of its addresses' while any
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
