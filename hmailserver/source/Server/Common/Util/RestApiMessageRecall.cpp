// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Message recall within the server, over the self-service API
// (Common/Util/MessageRecall.h, docs/MessageRecall.md):
//
//   POST /api/v1/me/recall   {"message_id": "<the Message-ID>", "sent_id": n}
//
// The rows for (the signed-in account, that Message-ID) not older than
// RecallDays are read and judged PER RECIPIENT, since a recipient who filed
// or copied the message holds several rows: read when any live copy of
// theirs carries \Seen (nothing is done; the reader keeps every copy),
// else recalled when at least one unread live copy was deleted the way the
// page deletes a message - every unread copy goes, IMAP sessions see the
// expunge, the recover-deleted store gets its copy while the server-wide
// window is on - else gone (no copy of theirs is there any more), and
// failed only when the database did not answer or a delete could not be
// made, in which case the rows are left unmarked and a later call decides
// them again. 404 when the account has no rows for that id - which is also
// the answer for somebody else's message, since the rows are keyed by the
// sender and a Message-ID is not a secret. Idempotent: rows recalled stay,
// marked, and a second call answers the same outcomes. Nothing is sent to
// anybody. The Sent copy, when sent_id names it and it is the caller's,
// gets the $Recalled keyword when at least one copy was recalled, and its
// To and Cc are read for the addresses that are not on this server,
// answered as elsewhere.
//
// Refused to a request that came in on an app password: a program's
// credential reads and sends mail; withdrawing what was sent is the
// person's own act.
#include "StdAfx.h"

#include "RestApiServer.h"
#include "HttpServer.h"
#include "JsonDocument.h"
#include "MessageRecall.h"
#include "Unicode.h"
#include "Parsing/StringParser.h"
#include "Strings/Formatter.h"
#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../Application/FolderManager.h"
#include "../Application/Logger.h"
#include "../Application/ObjectCache.h"
#include "../BO/Account.h"
#include "../BO/ACLPermission.h"
#include "../BO/DomainAliases.h"
#include "../BO/IMAPFolder.h"
#include "../BO/IMAPFolders.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"
#include "../BO/Messages.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentMessage.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseUnavailableMarker.h"
#include "../SQL/SQLCommand.h"
#include "../../IMAP/IMAPFolderContainer.h"
#include "../../IMAP/MessagesContainer.h"

#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const char *RecalledKeyword = "$Recalled";

      // The addresses of a To or Cc header, one per entry: the part inside
      // the angle brackets where there are any, else the entry itself. A
      // comma inside a quoted display name does not split.
      void AddressesOf(const String &header, std::vector<String> &addresses)
      {
         String entry;
         bool quoted = false;
         const int length = header.GetLength();
         for (int i = 0; i <= length; i++)
         {
            const wchar_t c = i < length ? header[i] : ',';
            if (c == '"')
               quoted = !quoted;
            if ((c == ',' || c == ';') && !quoted)
            {
               String address = entry;
               const int open = address.Find(_T("<"));
               const int close = address.ReverseFind(_T(">"));
               if (open >= 0 && close > open)
                  address = address.Mid(open + 1, close - open - 1);
               address.TrimLeft();
               address.TrimRight();
               if (!address.IsEmpty() && address.Find(_T("@")) > 0)
                  addresses.push_back(address);
               entry = _T("");
               continue;
            }
            entry += c;
         }
      }

      // How many distinct addresses the Sent copy names in To and Cc whose
      // domain this server does not host - after the domain aliases, so an
      // address at an alias name counts as here. Bcc is not in the copy's
      // headers and cannot be counted; the doc says so.
      int CountElsewhere(std::shared_ptr<const Account> account, std::shared_ptr<Message> copy)
      {
         MessageData data;
         if (!data.LoadFromMessage(account, copy))
            return 0;

         std::vector<String> addresses;
         AddressesOf(data.GetTo(), addresses);
         AddressesOf(data.GetCC(), addresses);

         std::shared_ptr<DomainAliases> aliases = ObjectCache::Instance()->GetDomainAliases();

         std::set<String> counted;
         int elsewhere = 0;
         for (size_t i = 0; i < addresses.size(); i++)
         {
            String address = addresses[i];
            address.ToLower();
            if (counted.find(address) != counted.end())
               continue;
            counted.insert(address);

            const String domain = StringParser::ExtractDomain(aliases ? aliases->ApplyAliasesOnAddress(address) : address);
            if (domain.IsEmpty() || CacheContainer::Instance()->GetDomain(domain))
               continue;

            elsewhere++;
         }
         return elsewhere;
      }

      // What one row's copy is, looked at now.
      enum CopyState
      {
         CopyUnavailable,   // the database did not answer; nothing may be decided
         CopyAbsent,        // no such row, another account's, another message's, or no folder
         CopySeen,          // there, and read
         CopyUnread         // there, unread: live and folder are set
      };

      // Opened here rather than through PersistentMessage::ReadObject, whose
      // false means either "no row" or "no answer" - and only one of those is
      // gone. A folder or a collection that could not be had is the database
      // away when the marker says so, and absent otherwise.
      CopyState Probe(const MessageRecall::Row &row, std::shared_ptr<const Account> recipient,
                      std::shared_ptr<Message> &live, std::shared_ptr<IMAPFolder> &folder)
      {
         live.reset();
         folder.reset();

         SQLCommand command("select * from hm_messages where messageid = @MESSAGEID");
         command.AddParameter("@MESSAGEID", row.stored_id);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return CopyUnavailable;
         if (recordset->IsEOF())
            return CopyAbsent;

         std::shared_ptr<Message> stored = std::shared_ptr<Message>(new Message());
         if (!PersistentMessage::ReadObject(recordset, stored, false) || stored->GetID() == 0)
            return DatabaseUnavailableMarker::IsMarked() ? CopyUnavailable : CopyAbsent;

         // The row must still be the recipient's: an id is never reused by
         // the databases that keep their identity counters, and where one is
         // reissued the Message-ID below tells the two messages apart.
         if (stored->GetAccountID() != row.recipient_id)
            return CopyAbsent;

         const String messageId = MessageRecall::MessageIdOf(PersistentMessage::GetFileName(recipient, stored));
         if (messageId != row.message_id)
         {
            if (DatabaseUnavailableMarker::IsMarked())
               return CopyUnavailable;
            LOG_DEBUG(Formatter::Format("Message recall: message {0} of {1} carries Message-ID <{2}>, not the recorded <{3}>; it is not the recorded copy.",
               row.stored_id, recipient->GetAddress(), messageId, row.message_id));
            return CopyAbsent;
         }

         std::shared_ptr<IMAPFolders> folders = IMAPFolderContainer::Instance()->GetFoldersForAccount(row.recipient_id);
         folder = folders ? folders->GetItemByDBIDRecursive(stored->GetFolderID()) : std::shared_ptr<IMAPFolder>();
         if (!folder)
            return DatabaseUnavailableMarker::IsMarked() ? CopyUnavailable : CopyAbsent;

         std::shared_ptr<Messages> messages = MessagesContainer::Instance()->GetMessages(row.recipient_id, folder->GetID());
         if (!messages)
            return DatabaseUnavailableMarker::IsMarked() ? CopyUnavailable : CopyAbsent;

         live = messages->GetItemByDBID(row.stored_id);
         if (!live)
            return DatabaseUnavailableMarker::IsMarked() ? CopyUnavailable : CopyAbsent;

         return live->GetFlagSeen() ? CopySeen : CopyUnread;
      }

      struct Probed
      {
         Probed() : row(nullptr), state(CopyAbsent) {}
         const MessageRecall::Row *row;
         CopyState state;
         std::shared_ptr<Message> live;
         std::shared_ptr<IMAPFolder> folder;
      };
   }

   HttpResponse
   RestApiServer::HandleMeRecall_(const Caller &caller, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      HttpResponse refusal;
      if (RefuseAppPasswordCaller_(caller, "recalling a message", refusal))
         return refusal;

      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.size()), document, parseError) || !document.IsObject())
         return BuildResponse_(400, "{\"error\":\"a JSON object is expected: message_id, and sent_id if the Sent copy is to be marked\"}");

      const JsonValue *idValue = document.Get("message_id");
      if (!idValue || !idValue->IsString())
         return BuildResponse_(400, "{\"error\":\"message_id is required: the Message-ID of the message to recall, with or without its angle brackets\"}");

      String messageId;
      Unicode::MultiByteToWide(AnsiString(idValue->AsString().c_str()), messageId);
      messageId = MessageRecall::Canonical(messageId);
      if (messageId.IsEmpty())
      {
         AnsiString json;
         json.Format("{\"error\":\"message_id is a Message-ID of at most %d characters, with or without its angle brackets\"}", MessageRecall::MessageIdMaximum);
         return BuildResponse_(400, json);
      }

      __int64 sentId = 0;
      const JsonValue *sentValue = document.Get("sent_id");
      if (sentValue && !sentValue->IsNull())
      {
         if (!sentValue->IsNumber() || sentValue->AsNumber() < 0)
            return BuildResponse_(400, "{\"error\":\"sent_id is the id of the Sent copy, a whole number\"}");
         sentId = (__int64) sentValue->AsNumber();
      }

      // A stale "database away" from earlier work on this pool thread must
      // not decide anything here; the marker is read fresh below.
      DatabaseUnavailableMarker::Scope databaseScope;

      std::vector<MessageRecall::Row> rows;
      if (!MessageRecall::List(account->GetID(), messageId, rows))
         return BuildResponse_(500, "{\"error\":\"the recall rows could not be read\"}");

      if (rows.empty())
      {
         AnsiString json;
         json.Format("{\"error\":\"no message with that Message-ID was sent from this account to an account on this server within the last %d days, or it was not recorded\"}", MessageRecall::Days(account->GetID()));
         return BuildResponse_(404, json);
      }

      const __int64 now = (__int64) time(nullptr);

      // The rows by recipient, in the order the recipients were first seen.
      std::vector<__int64> recipients;
      std::map<__int64, std::vector<const MessageRecall::Row *> > byRecipient;
      for (size_t i = 0; i < rows.size(); i++)
      {
         if (byRecipient.find(rows[i].recipient_id) == byRecipient.end())
            recipients.push_back(rows[i].recipient_id);
         byRecipient[rows[i].recipient_id].push_back(&rows[i]);
      }

      int recalled = 0;
      int read = 0;
      int gone = 0;
      int failed = 0;
      AnsiString recipientsJson = "[";

      for (size_t r = 0; r < recipients.size(); r++)
      {
         const std::vector<const MessageRecall::Row *> &theirs = byRecipient[recipients[r]];
         std::shared_ptr<const Account> recipient = CacheContainer::Instance()->GetAccount(recipients[r]);

         bool anyRead = false;
         bool anyRecalled = false;
         bool anyFailed = false;

         // What the marked rows already say, and what the pending rows' copies
         // are now. A copy that could not be looked at leaves the recipient
         // undecided: nothing of theirs is marked or deleted this time.
         std::vector<Probed> pending;
         for (size_t i = 0; i < theirs.size(); i++)
         {
            const MessageRecall::Row *row = theirs[i];
            if (row->outcome == MessageRecall::Read)
               anyRead = true;
            else if (row->outcome == MessageRecall::Recalled)
               anyRecalled = true;
            else if (row->outcome == MessageRecall::Pending)
            {
               Probed probed;
               probed.row = row;
               if (row->stored_id == 0 || !recipient)
                  probed.state = CopyAbsent;
               else
                  probed.state = Probe(*row, recipient, probed.live, probed.folder);
               if (probed.state == CopyUnavailable)
                  anyFailed = true;
               if (probed.state == CopySeen)
                  anyRead = true;
               pending.push_back(probed);
            }
         }

         if (!anyFailed)
         {
            for (size_t i = 0; i < pending.size(); i++)
            {
               const Probed &probed = pending[i];

               // A copy not yet delivered when the recall runs is answered
               // gone and left unmarked: the delivery fills the row in a
               // moment, and the next call decides it.
               if (probed.row->stored_id == 0)
                  continue;

               if (anyRead)
               {
                  // The recipient has read it: every copy of theirs stays.
                  MessageRecall::Mark(probed.row->id, MessageRecall::Read, now);
                  continue;
               }

               if (probed.state == CopyAbsent)
               {
                  MessageRecall::Mark(probed.row->id, MessageRecall::Gone, now);
                  continue;
               }

               // Unread and there: deleted the way the page's own delete
               // route deletes a message, through the same call.
               if (DeleteOwnMessage_(recipient, probed.live, probed.folder))
               {
                  MessageRecall::Mark(probed.row->id, MessageRecall::Recalled, now);
                  anyRecalled = true;
                  continue;
               }

               // The delete answered nothing. Another recall of the same
               // message may have taken the copy a moment ago and marked the
               // row; that mark is the answer. Otherwise the row stays as it
               // is and a later call tries again.
               MessageRecall::Row fresh;
               if (MessageRecall::Get(probed.row->id, fresh) && fresh.outcome != MessageRecall::Pending)
               {
                  if (fresh.outcome == MessageRecall::Read) anyRead = true;
                  else if (fresh.outcome == MessageRecall::Recalled) anyRecalled = true;
                  continue;
               }
               anyFailed = true;
            }
         }

         const char *outcome = anyFailed ? "failed" : anyRead ? "read" : anyRecalled ? "recalled" : "gone";
         if (anyFailed) failed++;
         else if (anyRead) read++;
         else if (anyRecalled) recalled++;
         else gone++;

         AnsiString entry;
         entry.Format("%hs{\"address\":\"%hs\",\"outcome\":\"%hs\"}", r > 0 ? "," : "",
            JsonEscape_(Utf8_(recipient ? recipient->GetAddress() : String())).c_str(), outcome);
         recipientsJson += entry;
      }
      recipientsJson += "]";

      // The Sent copy: the caller's own message, read for the addresses the
      // table does not know, and marked when something was withdrawn. A
      // sent_id that names nothing of the caller's is not an error - the
      // recall has happened - and answers elsewhere 0.
      int elsewhere = 0;
      bool marked = false;
      if (sentId > 0)
      {
         std::shared_ptr<IMAPFolder> folder;
         std::shared_ptr<Message> copy = FindOwnMessage_(account, sentId, folder);
         if (copy && folder)
         {
            elsewhere = CountElsewhere(account, copy);

            if (copy->HasKeyword(String(RecalledKeyword)))
               marked = true;
            else if (recalled > 0 && RightOn_(account, folder, ACLPermission::PermissionWriteOthers))
            {
               if (copy->AddKeyword(String(RecalledKeyword)) &&
                   Application::Instance()->GetFolderManager()->UpdateMessageFlags((int) folder->GetAccountID(), (int) folder->GetID(), copy->GetID(), copy->GetFlags(), copy->GetKeywords()))
               {
                  std::vector<__int64> changed;
                  changed.push_back(copy->GetID());
                  NotifyFolder(folder, ChangeNotification::NotificationMessageFlagsChanged, changed);
                  marked = true;
               }
            }
         }
      }

      LOG_APPLICATION(Formatter::Format("REST API: {0} recalled <{1}>: recalled {2}, read {3}, gone {4}.",
         account->GetAddress(), messageId, recalled, read, gone) +
         (failed > 0 ? Formatter::Format(" {0} could not be decided and will be tried again.", failed) : String()) +
         Formatter::Format(" Not on this server: {0}. From {1}.", elsewhere, String(caller.peer.ToString())));

      AnsiString json;
      json.Format("{\"message_id\":\"%hs\",\"recalled\":%d,\"read\":%d,\"gone\":%d,\"failed\":%d,\"elsewhere\":%d,\"sent_marked\":%hs,\"recipients\":%hs}",
         JsonEscape_(Utf8_(messageId)).c_str(), recalled, read, gone, failed, elsewhere, marked ? "true" : "false", recipientsJson.c_str());
      return BuildResponse_(200, json);
   }

   AnsiString
   RestApiServer::OpenApiMessageRecallPaths_()
   {
      return
         ",\"/api/v1/me/recall\":{\"post\":{\"summary\":\"Recall a message from the accounts on this server that have not read it\",\"description\":\"Body: message_id (the Message-ID of the message, with or without its angle brackets) and sent_id (optional: the id of the copy in the Sent folder). The rows this server recorded when the message was accepted from this account - one per recipient that resolved to a local account, kept for recall_days, with a row added for every copy the recipient makes within their own mailbox (IMAP COPY and MOVE, the webmail's move, archive, junk and trash), so the recall follows the message wherever they filed it - are read and judged per recipient: read when any live copy of theirs carries \\\\Seen (nothing is done; whoever has read it keeps every copy), recalled when at least one unread copy was deleted the way the webmail deletes a message - every unread copy goes, IMAP sessions see the expunge, the recover-deleted store gets its copy while the server-wide recover_deleted_days is on - gone when no copy of theirs is there any more (deleted, never delivered, or a copy that is still in the queue at this moment: that one is answered gone and decided again on the next call), and failed when the database did not answer or a delete could not be made, in which case nothing of theirs is marked and a later call decides again. Read is \\\\Seen and nothing more: a client that fetched the message without marking it seen has it, and the recall cannot know. Nobody outside this server is reached and no notice is sent to anybody; elsewhere counts the addresses the Sent copy names in To and Cc whose domain this server does not host, when sent_id is given (Bcc is not in the copy and is not counted). The Sent copy gets the keyword $Recalled when at least one copy was recalled. Idempotent: rows decided stay, marked, and a second call answers the same outcomes. 404 when this account has no rows for that Message-ID within recall_days - which is also the answer for somebody else's message, since the rows are keyed by the sender. Only a message submitted over authenticated SMTP or from the webmail is recorded; one with a Message-ID longer than 255 characters is not recallable (a submission without one is given one at acceptance, and the webmail always writes one), and at most 200 recipient rows per sender per day are recorded. Counted against the credential's request rate as every request is. Refused (403) to a request authenticated with an app password.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"message_id\"],\"properties\":{\"message_id\":{\"type\":\"string\"},\"sent_id\":{\"type\":\"integer\"}}}}}},\"responses\":{\"200\":{\"description\":\"message_id, recalled, read, gone, failed, elsewhere, sent_marked, and recipients: [{address, outcome}] with outcome recalled, read, gone or failed; address is empty for a recipient account since deleted\"},\"400\":{\"description\":\"No message_id, one longer than 255 characters, or a sent_id that is not a whole number\"},\"403\":{\"description\":\"The request came in on an app password\"},\"404\":{\"description\":\"No recorded recipients on this server for that Message-ID from this account within recall_days\"}}}}";
   }
}
