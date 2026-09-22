// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Message recall within the server, over the self-service API
// (Common/Util/MessageRecall.h, docs/MessageRecall.md):
//
//   POST /api/v1/me/recall   {"message_id": "<the Message-ID>", "sent_id": n}
//
// The rows for (the signed-in account, that Message-ID) not older than
// RecallDays are read; for each recipient copy on this server the outcome is
// gone (the row is no longer there), read (it carries \Seen; nothing is
// done) or recalled (the unread copy is deleted the way the page deletes a
// message, so IMAP sessions see the expunge, the recover-deleted store gets
// its copy where that is on, and the notifications fire). 404 when the
// account has no rows for that id - which is also the answer for somebody
// else's message, since the rows are keyed by the sender and a Message-ID
// is not a secret. Idempotent: rows recalled stay, marked, and a second
// call answers the same outcomes. Nothing is sent to anybody. The Sent
// copy, when sent_id names it and it is the caller's, gets the $Recalled
// keyword when at least one copy was recalled, and its To and Cc are read
// for the addresses that are not on this server, answered as elsewhere.
//
// Refused to a request that came in on an app password: a program's
// credential reads and sends mail; withdrawing what was sent is the
// person's own act.
#include "StdAfx.h"

#include "RestApiServer.h"
#include "HttpServer.h"
#include "JsonDocument.h"
#include "MessageRecall.h"
#include "RecoverableStore.h"
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
#include "../Tracking/ChangeNotification.h"
#include "../Tracking/NotificationServer.h"
#include "../../IMAP/IMAPFolderContainer.h"
#include "../../IMAP/MessagesContainer.h"

#include <ctime>
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

      // Every IMAP session on the folder is told, the way EXPUNGE and STORE
      // tell them: by message id, which each turns into its own sequence
      // numbers.
      void Notify(std::shared_ptr<IMAPFolder> folder, ChangeNotification::NotificationType type, const std::vector<__int64> &messageIds)
      {
         std::shared_ptr<ChangeNotification> notification =
            std::shared_ptr<ChangeNotification>(new ChangeNotification(folder->GetAccountID(), folder->GetID(), type, messageIds));
         Application::Instance()->GetNotificationServer()->SendNotification(notification);
      }

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

      // What became of one recipient's copy, decided now: gone when there is
      // no row, no folder or no message any more; read when the copy carries
      // \Seen, in which case nothing is touched; otherwise the copy is
      // deleted through the same collection the IMAP sessions hold, inside
      // the recover-deleted scope, and every session on the folder is told.
      // Pending is answered only when the delete itself failed, so the row
      // is not marked and a later call tries again.
      MessageRecall::Outcome Decide(const MessageRecall::Row &row)
      {
         if (row.stored_id == 0)
            return MessageRecall::Gone;

         std::shared_ptr<Message> stored = std::shared_ptr<Message>(new Message());
         if (!PersistentMessage::ReadObject(stored, row.stored_id) || stored->GetID() == 0)
            return MessageRecall::Gone;

         // The row must still be the recipient's: an id is never reused, but
         // the copy may have been moved into a shared folder another account
         // owns, and that account's mailbox is not the sender's to touch.
         if (stored->GetAccountID() != row.recipient_id)
            return MessageRecall::Gone;

         std::shared_ptr<IMAPFolders> folders = IMAPFolderContainer::Instance()->GetFoldersForAccount(row.recipient_id);
         std::shared_ptr<IMAPFolder> folder = folders ? folders->GetItemByDBIDRecursive(stored->GetFolderID()) : std::shared_ptr<IMAPFolder>();
         if (!folder)
            return MessageRecall::Gone;

         std::shared_ptr<Messages> messages = MessagesContainer::Instance()->GetMessages(row.recipient_id, folder->GetID());
         if (!messages)
            return MessageRecall::Gone;

         std::shared_ptr<Message> live = messages->GetItemByDBID(row.stored_id);
         if (!live)
            return MessageRecall::Gone;

         if (live->GetFlagSeen())
            return MessageRecall::Read;

         std::set<__int64> ids;
         ids.insert(row.stored_id);

         std::vector<__int64> deleted;
         {
            RecoverableStore::Scope recoverable;   // the reader's window applies, as it does to their own delete
            deleted = messages->DeleteMessagesById(ids);
         }
         if (deleted.empty())
            return MessageRecall::Pending;

         Notify(folder, ChangeNotification::NotificationMessageDeleted, deleted);
         return MessageRecall::Recalled;
      }
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

      std::vector<MessageRecall::Row> rows;
      if (!MessageRecall::List(account->GetID(), messageId, rows))
         return BuildResponse_(500, "{\"error\":\"the recall rows could not be read\"}");

      if (rows.empty())
      {
         AnsiString json;
         json.Format("{\"error\":\"no message with that Message-ID was sent from this account to an account on this server within the last %d days, or it was not recorded\"}", MessageRecall::Days());
         return BuildResponse_(404, json);
      }

      const __int64 now = (__int64) time(nullptr);

      int recalled = 0;
      int read = 0;
      int gone = 0;
      int failed = 0;
      AnsiString recipientsJson = "[";

      for (size_t i = 0; i < rows.size(); i++)
      {
         const MessageRecall::Row &row = rows[i];

         MessageRecall::Outcome outcome = (MessageRecall::Outcome) row.outcome;
         if (outcome == MessageRecall::Pending)
         {
            outcome = Decide(row);
            if (outcome != MessageRecall::Pending)
               MessageRecall::Mark(row.id, outcome, now);
         }

         if (outcome == MessageRecall::Recalled) recalled++;
         else if (outcome == MessageRecall::Read) read++;
         else if (outcome == MessageRecall::Gone) gone++;
         else failed++;

         std::shared_ptr<const Account> recipient = CacheContainer::Instance()->GetAccount(row.recipient_id);
         const String address = recipient ? recipient->GetAddress() : String();

         AnsiString entry;
         entry.Format("%hs{\"address\":\"%hs\",\"outcome\":\"%hs\"}", i > 0 ? "," : "",
            JsonEscape_(Utf8_(address)).c_str(),
            outcome == MessageRecall::Pending ? "failed" : MessageRecall::OutcomeWord(outcome));
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

            if (recalled > 0 && !copy->HasKeyword(String(RecalledKeyword)) && RightOn_(account, folder, ACLPermission::PermissionWriteOthers))
            {
               if (copy->AddKeyword(String(RecalledKeyword)) &&
                   Application::Instance()->GetFolderManager()->UpdateMessageFlags((int) folder->GetAccountID(), (int) folder->GetID(), copy->GetID(), copy->GetFlags(), copy->GetKeywords()))
               {
                  std::vector<__int64> changed;
                  changed.push_back(copy->GetID());
                  Notify(folder, ChangeNotification::NotificationMessageFlagsChanged, changed);
                  marked = true;
               }
            }
            else if (copy->HasKeyword(String(RecalledKeyword)))
               marked = true;
         }
      }

      LOG_APPLICATION(Formatter::Format("REST API: {0} recalled <{1}>: recalled {2}, read {3}, gone {4}.",
         account->GetAddress(), messageId, recalled, read, gone) +
         (failed > 0 ? Formatter::Format(" {0} could not be deleted and will be tried again.", failed) : String()) +
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
         ",\"/api/v1/me/recall\":{\"post\":{\"summary\":\"Recall a message from the accounts on this server that have not read it\",\"description\":\"Body: message_id (the Message-ID of the message, with or without its angle brackets) and sent_id (optional: the id of the copy in the Sent folder). The rows this server recorded when the message was accepted from this account - one per recipient that resolved to a local account, kept for recall_days - are read, and for each copy: gone when the recipient's message is no longer there (deleted, or never delivered), read when it carries \\\\Seen (nothing is done; whoever has read it keeps it), recalled when the unread copy was deleted the way the webmail deletes a message - IMAP sessions see the expunge, the recover-deleted store gets its copy where that is on. Read is \\\\Seen and nothing more: a client that fetched the message without marking it seen has it, and the recall cannot know. Nobody outside this server is reached and no notice is sent to anybody; elsewhere counts the addresses the Sent copy names in To and Cc whose domain this server does not host, when sent_id is given (Bcc is not in the copy and is not counted). The Sent copy gets the keyword $Recalled when at least one copy was recalled. Idempotent: rows recalled stay, marked, and a second call answers the same outcomes. 404 when this account has no rows for that Message-ID within recall_days - which is also the answer for somebody else's message, since the rows are keyed by the sender. Only a message submitted over authenticated SMTP or from the webmail is recorded; one without a Message-ID, or with one longer than 255 characters, is not recallable, and at most 200 recipient rows per sender per day are recorded. Refused (403) to a request authenticated with an app password.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"message_id\"],\"properties\":{\"message_id\":{\"type\":\"string\"},\"sent_id\":{\"type\":\"integer\"}}}}}},\"responses\":{\"200\":{\"description\":\"message_id, recalled, read, gone, failed (a copy whose delete failed; the row is not marked and a later call tries again), elsewhere, sent_marked, and recipients: [{address, outcome}] with outcome recalled, read, gone or failed\"},\"400\":{\"description\":\"No message_id, one longer than 255 characters, or a sent_id that is not a whole number\"},\"403\":{\"description\":\"The request came in on an app password\"},\"404\":{\"description\":\"No recorded recipients on this server for that Message-ID from this account within recall_days\"}}}}";
   }
}
