// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "MessageRecall.h"
#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../Application/EffectiveSetting.h"
#include "../Application/ErrorManager.h"
#include "../Application/Logger.h"
#include "../BO/Account.h"
#include "../BO/Message.h"
#include "../BO/MessageRecipient.h"
#include "../BO/MessageRecipients.h"
#include "../Cache/CacheContainer.h"
#include "../Mime/Mime.h"
#include "../Persistence/PersistentMessage.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "Strings/Formatter.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <mutex>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const char *Columns = "recallid, recallsenderid, recallmsgid, recallrecipientid, recallqueueid, recallmessageid, recallcreated, recallrecalledat, recalloutcome";

      // The senders told, this rolling day, that their ceiling was reached:
      // sender id to the day the line was written. Said once per sender per
      // day, so a bulk sender does not fill the application log with the
      // same sentence once per message.
      std::mutex said_mutex;
      std::map<__int64, __int64> said;

      void SayCeilingOnce(std::shared_ptr<const Account> sender, __int64 now)
      {
         const __int64 day = now / MessageRecall::DaySeconds;
         {
            std::lock_guard<std::mutex> guard(said_mutex);
            std::map<__int64, __int64>::iterator it = said.find(sender->GetID());
            if (it != said.end() && it->second == day)
               return;
            if (said.size() > 4096)
               said.clear();
            said[sender->GetID()] = day;
         }

         LOG_APPLICATION(Formatter::Format("Message recall: {0} has sent to {1} local recipients in the last day, which is the most that is recorded; nothing more it sends today can be recalled.",
            sender->GetAddress(), (int) MessageRecall::RowsPerSenderPerDay));
      }

      // How many rows the sender has had recorded in the last rolling day;
      // -1 when the database did not answer.
      int CountToday(__int64 senderId, __int64 now)
      {
         SQLCommand command("select count(*) as recallcount from hm_recall where recallsenderid = @SENDERID and recallcreated >= @SINCE");
         command.AddParameter("@SENDERID", senderId);
         command.AddParameter("@SINCE", now - MessageRecall::DaySeconds);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return -1;
         return recordset->IsEOF() ? 0 : (int) recordset->GetInt64Value("recallcount");
      }

      bool Insert(__int64 senderId, const String &messageId, __int64 recipientId, __int64 queueId, __int64 storedId, __int64 created)
      {
         SQLStatement statement(SQLStatement::STInsert, "hm_recall");
         statement.AddColumnInt64("recallsenderid", senderId);
         statement.AddColumn("recallmsgid", messageId);
         statement.AddColumnInt64("recallrecipientid", recipientId);
         statement.AddColumnInt64("recallqueueid", queueId);
         statement.AddColumnInt64("recallmessageid", storedId);
         statement.AddColumnInt64("recallcreated", created);
         statement.AddColumnInt64("recallrecalledat", 0);
         statement.AddColumn("recalloutcome", (long) MessageRecall::Pending);
         statement.SetIdentityColumn("recallid");

         __int64 id = 0;
         if (!Application::Instance()->GetDBManager()->Execute(statement, &id) || id <= 0)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7090, "MessageRecall::Record",
               "The recall row for a message sent to a local account could not be written; that copy cannot be recalled.");
            return false;
         }
         return true;
      }

      MessageRecall::Row ReadRow(std::shared_ptr<DALRecordset> recordset)
      {
         MessageRecall::Row row;
         row.id = recordset->GetInt64Value("recallid");
         row.sender_id = recordset->GetInt64Value("recallsenderid");
         row.message_id = recordset->GetStringValue("recallmsgid");
         row.recipient_id = recordset->GetInt64Value("recallrecipientid");
         row.queue_id = recordset->GetInt64Value("recallqueueid");
         row.stored_id = recordset->GetInt64Value("recallmessageid");
         row.created = recordset->GetInt64Value("recallcreated");
         row.recalled_at = recordset->GetInt64Value("recallrecalledat");
         row.outcome = (int) recordset->GetLongValue("recalloutcome");
         return row;
      }

      // The rows a command selects, oldest first; false when the database
      // did not answer.
      bool Select(SQLCommand &command, std::vector<MessageRecall::Row> &rows)
      {
         rows.clear();
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return false;
         while (!recordset->IsEOF())
         {
            rows.push_back(ReadRow(recordset));
            recordset->MoveNext();
         }
         return true;
      }
   }

   int
   MessageRecall::Days(__int64 senderId)
   {
      std::shared_ptr<const Account> sender;
      if (senderId > 0)
         sender = CacheContainer::Instance()->GetAccount(senderId);

      return EffectiveSetting::ResolveInt(_T("recall_days"), std::shared_ptr<const Domain>(), sender);
   }

   String
   MessageRecall::Canonical(const String &value)
   {
      String id = value;
      id.TrimLeft();
      id.TrimRight();
      if (id.GetLength() >= 2 && id.StartsWith(_T("<")) && id.EndsWith(_T(">")))
         id = id.Mid(1, id.GetLength() - 2);
      id.TrimLeft();
      id.TrimRight();
      if (id.GetLength() > MessageIdMaximum)
         return String();
      return id;
   }

   String
   MessageRecall::MessageIdOf(const String &fileName)
   {
      AnsiString raw = PersistentMessage::LoadHeader(fileName, false);
      if (raw.IsEmpty())
         return String();

      MimeHeader header;
      header.Load(raw.c_str(), raw.GetLength(), true);

      const char *value = header.GetRawFieldValue(_T("Message-ID"));
      if (value == nullptr || *value == 0)
         return String();

      return Canonical(String(AnsiString(value)));
   }

   int
   MessageRecall::Record(std::shared_ptr<const Account> sender, std::shared_ptr<Message> message, const AnsiString &source)
   {
      if (!sender || !message || message->GetID() == 0)
         return 0;

      // The distinct local accounts among the recipients, as the recipient
      // parser resolved them at RCPT TO: a list's members and an alias's
      // target are already there, each once.
      std::vector<__int64> recipients;
      // owned by this call: the recipients of the message being submitted, held by its session or by the send route
      for (std::shared_ptr<MessageRecipient> recipient : message->GetRecipients()->GetVector())
      {
         if (!recipient || recipient->GetLocalAccountID() == 0)
            continue;
         if (std::find(recipients.begin(), recipients.end(), recipient->GetLocalAccountID()) == recipients.end())
            recipients.push_back(recipient->GetLocalAccountID());
      }

      if (recipients.empty())
         return 0;

      const String messageId = MessageIdOf(PersistentMessage::GetFileName(message));
      if (messageId.IsEmpty())
      {
         LOG_DEBUG(Formatter::Format("Message recall: message {0} from {1} over {2} has no Message-ID, or one longer than {3} characters, and is not recallable.",
            message->GetID(), sender->GetAddress(), String(source), (int) MessageIdMaximum));
         return 0;
      }

      const __int64 now = (__int64) time(nullptr);

      const int today = CountToday(sender->GetID(), now);
      if (today < 0)
         return 0;

      const int room = RowsPerSenderPerDay - today;
      if (room <= 0)
      {
         SayCeilingOnce(sender, now);
         return 0;
      }

      int written = 0;
      for (size_t i = 0; i < recipients.size() && written < room; i++)
      {
         if (Insert(sender->GetID(), messageId, recipients[i], message->GetID(), 0, now))
            written++;
      }

      if (written < (int) recipients.size() && written == room)
         SayCeilingOnce(sender, now);

      const String who = sender->GetAddress() + _T(" over ") + String(source);
      LOG_DEBUG(Formatter::Format("Message recall: recorded {0} of {1} local recipients of <{2}> sent by {3}, queue message {4}.",
         written, (int) recipients.size(), messageId, who, message->GetID()));

      return written;
   }

   void
   MessageRecall::Delivered(__int64 queueId, std::shared_ptr<const Account> recipient, __int64 storedId, const String &fileName)
   {
      if (queueId <= 0 || !recipient || storedId <= 0)
         return;

      SQLCommand command(AnsiString("select ") + Columns + " from hm_recall where recallqueueid = @QUEUEID order by recallid");
      command.AddParameter("@QUEUEID", queueId);

      std::vector<Row> rows;
      if (!Select(command, rows))
         return;

      // The common case - a message from outside, or one this server wrote -
      // costs the one select above and nothing more.
      if (rows.empty())
         return;

      // The rows' message and the delivered file have to carry one
      // Message-ID. A database that hands out an hm_messages id again after
      // a restart - MySQL 5 recomputes AUTO_INCREMENT from what is left - can
      // accept an outsider's message under a queue id whose rows are still
      // within RecallDays; the header is what tells the two apart, and it is
      // read only here, when rows exist.
      const Row &first = rows.front();
      const String delivered = MessageIdOf(fileName);
      if (delivered.IsEmpty() || delivered != first.message_id)
      {
         LOG_DEBUG(Formatter::Format("Message recall: queue message {0} delivered to {1} carries Message-ID <{2}>, not the recorded <{3}>; the copy is not tied to the rows.",
            queueId, recipient->GetAddress(), delivered, first.message_id));
         return;
      }

      bool found = false;
      for (const Row &row : rows)
      {
         if (row.recipient_id != recipient->GetID())
            continue;

         found = true;
         if (row.stored_id != 0)
            continue;

         SQLCommand update("update hm_recall set recallmessageid = @MESSAGEID where recallid = @ID");
         update.AddParameter("@MESSAGEID", storedId);
         update.AddParameter("@ID", row.id);
         Application::Instance()->GetDBManager()->Execute(update);
      }

      if (found)
         return;

      // A recipient the acceptance did not know - reached by an expansion
      // made after RCPT TO - whose message is provably the one the rows
      // record: the same queue row, the same Message-ID. The sender is the
      // rows' sender, and the day's ceiling holds here as it does at
      // acceptance.
      const __int64 now = (__int64) time(nullptr);
      const int today = CountToday(first.sender_id, now);
      if (today < 0)
         return;
      if (today >= RowsPerSenderPerDay)
      {
         std::shared_ptr<const Account> sender = CacheContainer::Instance()->GetAccount(first.sender_id);
         if (sender)
            SayCeilingOnce(sender, now);
         return;
      }

      if (Insert(first.sender_id, first.message_id, recipient->GetID(), queueId, storedId, now))
      {
         LOG_DEBUG(Formatter::Format("Message recall: {0} was reached by expansion of queue message {1}; its copy is recorded under the sender's <{2}>.",
            recipient->GetAddress(), queueId, first.message_id));
      }
   }

   void
   MessageRecall::Copied(__int64 sourceId, __int64 copyId, __int64 accountId)
   {
      if (sourceId <= 0 || copyId <= 0 || accountId <= 0)
         return;

      SQLCommand command(AnsiString("select ") + Columns + " from hm_recall where recallmessageid = @SOURCEID order by recallid");
      command.AddParameter("@SOURCEID", sourceId);

      std::vector<Row> rows;
      if (!Select(command, rows) || rows.empty())
         return;

      int written = 0;
      for (const Row &row : rows)
      {
         // The copy is the recipient's own, or it is nobody's business here.
         if (row.recipient_id != accountId)
            continue;

         // The sibling keeps the original's clock, so the copy leaves the
         // window when the message does and not later.
         if (Insert(row.sender_id, row.message_id, row.recipient_id, row.queue_id, copyId, row.created))
            written++;
      }

      if (written > 0)
      {
         LOG_DEBUG(Formatter::Format("Message recall: message {0} was copied to {1} within its mailbox; {2} recall row(s) follow it.",
            sourceId, copyId, written));
      }
   }

   bool
   MessageRecall::List(__int64 senderId, const String &messageId, std::vector<Row> &rows)
   {
      const __int64 now = (__int64) time(nullptr);

      SQLCommand command(AnsiString("select ") + Columns + " from hm_recall where recallsenderid = @SENDERID and recallmsgid = @MSGID and recallcreated >= @SINCE order by recallid");
      command.AddParameter("@SENDERID", senderId);
      command.AddParameter("@MSGID", messageId);
      command.AddParameter("@SINCE", now - (__int64) Days(senderId) * DaySeconds);
      return Select(command, rows);
   }

   bool
   MessageRecall::Get(__int64 rowId, Row &row)
   {
      SQLCommand command(AnsiString("select ") + Columns + " from hm_recall where recallid = @ID");
      command.AddParameter("@ID", rowId);

      std::vector<Row> rows;
      if (!Select(command, rows) || rows.empty())
         return false;
      row = rows.front();
      return true;
   }

   bool
   MessageRecall::Mark(__int64 rowId, Outcome outcome, __int64 when)
   {
      SQLCommand command("update hm_recall set recalloutcome = @OUTCOME, recallrecalledat = @WHEN where recallid = @ID");
      command.AddParameter("@OUTCOME", (int) outcome);
      command.AddParameter("@WHEN", when);
      command.AddParameter("@ID", rowId);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   void
   MessageRecall::Sweep()
   {
      // The server's window, which is the widest there is: recall_days is a
      // limit, and no domain or account holds a longer one.
      const __int64 cutoff = (__int64) time(nullptr) -
         (__int64) EffectiveSetting::ResolveInt(_T("recall_days"), std::shared_ptr<const Domain>(), std::shared_ptr<const Account>()) * DaySeconds;

      SQLCommand command("delete from hm_recall where recallcreated < @CUTOFF");
      command.AddParameter("@CUTOFF", cutoff);
      Application::Instance()->GetDBManager()->Execute(command);
   }

   const char *
   MessageRecall::OutcomeWord(Outcome outcome)
   {
      switch (outcome)
      {
      case Recalled: return "recalled";
      case Read: return "read";
      case Gone: return "gone";
      default: return "pending";
      }
   }
}
