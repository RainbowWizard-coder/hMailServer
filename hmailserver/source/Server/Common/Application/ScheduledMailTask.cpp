// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "ScheduledMailTask.h"
#include "Application.h"
#include "ErrorManager.h"
#include "FolderManager.h"
#include "../BO/Account.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"
#include "../BO/MessageRecipients.h"
#include "../BO/Messages.h"
#include "../BO/IMAPFolders.h"
#include "../BO/IMAPFolder.h"
#include "../Cache/CacheContainer.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/DALRecordset.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../Util/Time.h"
#include "../Util/FileUtilities.h"
#include "../Util/MessageUtilities.h"
#include "../Util/OutgoingFooters.h"
#include "../Util/RestApiServer.h"
#include "../Util/ConfidentialMessages.h"
#include "../Util/ProtectedMessages.h"
#include "../Util/MessageRecall.h"
#include "../Util/CompromiseDetector.h"
#include "../Util/ResourceBooking.h"
#include "../BO/MessageRecipient.h"
#include "../Util/ContactGroups.h"
#include "../Tracking/ChangeNotification.h"
#include "../Tracking/NotificationServer.h"
#include "../../IMAP/IMAPFolderContainer.h"
#include "../../IMAP/MessagesContainer.h"
#include "../../SMTP/RecipientParser.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      void RemoveRow(__int64 schedId)
      {
         SQLCommand command("delete from hm_scheduled where schedid = @ID");
         command.AddParameter("@ID", schedId);
         Application::Instance()->GetDBManager()->Execute(command);
      }

      // "a@x, Name <b@y>; c@z" -> the three entries as the header writes them,
      // and the address each gives the envelope. The send route splits its To
      // field the same way (RestApiServer.cpp), and it must, because the same
      // expansion runs over both.
      void SplitList(const String &list, std::vector<String> &addresses, std::vector<String> &entries)
      {
         std::vector<String> parts = StringParser::SplitString(list, _T(","));
         for (size_t i = 0; i < parts.size(); i++)
         {
            std::vector<String> more = StringParser::SplitString(parts[i], _T(";"));
            for (size_t j = 0; j < more.size(); j++)
            {
               String entry = more[j];
               entry.TrimLeft();
               entry.TrimRight();
               if (entry.IsEmpty())
                  continue;

               String address = entry;
               int open = entry.Find(_T("<"));
               int close = entry.ReverseFind(_T('>'));
               if (open >= 0 && close > open)
                  address = entry.Mid(open + 1, close - open - 1);
               address.TrimLeft();
               address.TrimRight();
               if (address.IsEmpty())
                  address = entry;

               addresses.push_back(address);
               entries.push_back(entry);
            }
         }
      }

      String JoinEntries(const std::vector<String> &entries)
      {
         String joined;
         for (size_t i = 0; i < entries.size(); i++)
         {
            if (i > 0)
               joined += _T(", ");
            joined += entries[i];
         }
         return joined;
      }

      // A draft addressed to a contact group by its name - "Family" typed into
      // To and not picked from the completion, which would have written the
      // people out - is expanded here, at the moment it goes, exactly as the
      // send route expands it (ContactGroups.h). Without this the name is not
      // an address, the recipient is dropped, and a message the reader was
      // told would be sent at a time is never sent at all. The header is
      // rewritten with the people, as on the immediate path, so the readers
      // see who it went to; Bcc goes out of the file either way, so only its
      // addresses are wanted.
      void ExpandGroups(std::shared_ptr<const Account> account, MessageData &data, std::vector<String> &addresses)
      {
         std::vector<String> toAddresses, toEntries, ccAddresses, ccEntries, bccAddresses, bccEntries;
         SplitList(data.GetTo(), toAddresses, toEntries);
         SplitList(data.GetCC(), ccAddresses, ccEntries);
         SplitList(data.GetBCC(), bccAddresses, bccEntries);

         if (ContactGroups::NeedsExpansion(toEntries) || ContactGroups::NeedsExpansion(ccEntries) || ContactGroups::NeedsExpansion(bccEntries))
         {
            std::vector<ContactGroup> groups;
            if (ContactGroups::List(account, groups) && !groups.empty())
            {
               const String wasTo = JoinEntries(toEntries);
               const String wasCc = JoinEntries(ccEntries);

               ContactGroups::ExpandEntries(groups, toAddresses, toEntries);
               ContactGroups::ExpandEntries(groups, ccAddresses, ccEntries);
               ContactGroups::ExpandEntries(groups, bccAddresses, bccEntries);

               if (JoinEntries(toEntries) != wasTo)
                  data.SetTo(JoinEntries(toEntries));
               if (JoinEntries(ccEntries) != wasCc)
                  data.SetCC(JoinEntries(ccEntries));
            }
         }

         addresses.insert(addresses.end(), toAddresses.begin(), toAddresses.end());
         addresses.insert(addresses.end(), ccAddresses.begin(), ccAddresses.end());
         addresses.insert(addresses.end(), bccAddresses.begin(), bccAddresses.end());
      }

      void Notify(std::shared_ptr<IMAPFolder> folder, ChangeNotification::NotificationType type, const std::vector<__int64> &ids)
      {
         std::shared_ptr<ChangeNotification> notification =
            std::shared_ptr<ChangeNotification>(new ChangeNotification(folder->GetAccountID(), folder->GetID(), type, ids));
         Application::Instance()->GetNotificationServer()->SendNotification(notification);
      }

      // The row and the file of a message in the account's own tree, gone,
      // and every session told - what EXPUNGE does.
      bool Remove(std::shared_ptr<IMAPFolder> folder, std::shared_ptr<Message> message)
      {
         std::shared_ptr<Messages> messages = MessagesContainer::Instance()->GetMessages(folder->GetAccountID(), folder->GetID());
         if (!messages)
            return false;
         std::set<__int64> ids;
         ids.insert(message->GetID());
         std::vector<__int64> deleted = messages->DeleteMessagesById(ids);
         if (deleted.empty())
            return false;
         Notify(folder, ChangeNotification::NotificationMessageDeleted, deleted);
         return true;
      }
   }

   ScheduledMailTask::ScheduledMailTask()
   {
   }

   ScheduledMailTask::~ScheduledMailTask()
   {
   }

   void
   ScheduledMailTask::DoWork()
   {
      RunDue();
      // A room's or a piece of equipment's deliveries that a stop, a
      // Reinitialize or a crash left undecided are decided now
      // (ResourceBooking.h): each such resource is woken.
      ResourceBooking::WakeQueued();
      // Files sent as links that have expired go on the same minute, and so do
      // the kept copies of messages that expire (ConfidentialMessages.h).
      RestApiServer::SweepExpiredFiles();
      ConfidentialMessages::Sweep(false);
      ProtectedMessages::Sweep(false);
      // And the recall rows older than RecallDays (MessageRecall.h).
      MessageRecall::Sweep();
   }

   int
   ScheduledMailTask::RunDue()
   {
      // The rows are few and the time is compared as the text both sides are
      // stored in, "YYYY-MM-DD HH:MM:SS", which orders as time does - so no
      // backend is asked to compare a bound value with a datetime column.
      const String now = Time::GetCurrentDateTime();

      struct Due
      {
         __int64 id;
         __int64 accountId;
         __int64 messageId;
         int action;
         __int64 folderId;
      };
      std::vector<Due> due;

      SQLCommand command("select schedid, schedaccountid, schedmessageid, schedaction, schedat, schedfolderid from hm_scheduled order by schedat");
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return 0;
      while (!recordset->IsEOF())
      {
         String at = recordset->GetStringValue("schedat");
         if (at.GetLength() >= 19 && at.Mid(0, 19) <= now)
         {
            Due one;
            one.id = recordset->GetInt64Value("schedid");
            one.accountId = recordset->GetInt64Value("schedaccountid");
            one.messageId = recordset->GetInt64Value("schedmessageid");
            one.action = (int) recordset->GetLongValue("schedaction");
            one.folderId = recordset->GetInt64Value("schedfolderid");
            due.push_back(one);
         }
         recordset->MoveNext();
      }

      int ran = 0;
      for (size_t i = 0; i < due.size(); i++)
      {
         std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(due[i].accountId);
         std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
         bool present = account && PersistentMessage::ReadObject(message, due[i].messageId) && message->GetID() != 0;
         if (present)
         {
            if (due[i].action == ActionSend)
            {
               // A send that came due and did not go is said out loud: the
               // row is about to be removed, so this is the only record that
               // a message the reader was told would be sent at a time was
               // not. No recipient the checks allow is the usual reason.
               if (!SendDraft(account, message))
               {
                  ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6832, "ScheduledMailTask::RunDue",
                     "The scheduled send of message " + StringParser::IntToString(due[i].messageId) + " for account " +
                     StringParser::IntToString(due[i].accountId) + " came due and did not go - no recipient of its To, Cc or Bcc "
                     "could be delivered to, or the draft could not be read. The scheduled row is removed; the draft, if it is still "
                     "there, is not sent.");
               }
            }
            else if (due[i].action == ActionReturn)
               ReturnSnoozed(account, message, due[i].folderId);
         }
         // The row goes whether the action succeeded or its message is gone:
         // a draft the reader deleted is not sent, and a row that cannot act
         // must not be retried every minute for ever.
         RemoveRow(due[i].id);
         ran++;
      }
      return ran;
   }

   // The draft becomes a message in the queue, as the send route would have
   // made it: the recipients from its To, Cc and Bcc, each put through the
   // checks a send makes, the Bcc dropped from the copy that goes, the
   // compromised-account detection's judgment, a copy in the folder
   // designated \Sent, the draft itself gone.
   bool
   ScheduledMailTask::SendDraft(std::shared_ptr<const Account> account, std::shared_ptr<Message> draft)
   {
      MessageData data;
      if (!data.LoadFromMessage(account, draft))
         return false;

      std::vector<String> addresses;
      ExpandGroups(account, data, addresses);
      if (addresses.empty())
         return false;

      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
      message->SetFromAddress(account->GetAddress());

      RecipientParser parser;
      for (size_t i = 0; i < addresses.size(); i++)
      {
         if (!StringParser::IsValidEmailAddress(addresses[i]))
            continue;
         String reason;
         bool treatSecurityAsLocal = false;
         if (parser.CheckDeliveryPossibility(true, account->GetAddress(), addresses[i], reason, treatSecurityAsLocal, 0, true) != RecipientParser::DP_Possible)
            continue;
         bool recipientOK = false;
         parser.CreateMessageRecipientList(addresses[i], message->GetRecipients(), recipientOK);
      }
      if (message->GetRecipients()->GetCount() == 0)
         return false;

      const String fileName = MessageStore::WorkingFile(message);
      data.DeleteField("Bcc");
      data.SetSentTime(Time::GetCurrentMimeDate());
      if (!data.Write(fileName))
         return false;

      // The domain's signature and disclaimer, as the send route puts them on.
      OutgoingFooters::AddToWebmailMessage(message, account);

      message->SetSize((int) MessageStore::FileSize(fileName));

      // The shape of a takeover (CompromiseDetector.h), judged as the send
      // route judges it: a scheduled draft is the account's submission when
      // its minute comes, counted into the baseline like a send and, in
      // enforce mode, held for review like one - or held on the account's
      // standing lock when the account was locked while the draft waited.
      // There is no client address on a task thread, so that signal never
      // scores here. A held message is saved as HeldForReview, which the
      // deliverer never selects, and gets its \Sent copy when it is released.
      std::vector<String> submitted;
      String recipientList;
      // owned by this call: the recipients of the message this task built
      for (std::shared_ptr<MessageRecipient> recipient : message->GetRecipients()->GetVector())
      {
         if (!recipient)
            continue;
         const String address = recipient->GetOriginalAddress().IsEmpty() ? recipient->GetAddress() : recipient->GetOriginalAddress();
         submitted.push_back(address);
         recipientList += (recipientList.IsEmpty() ? _T("") : _T(", ")) + address;
      }
      CompromiseDetector::Assessment shape;
      CompromiseDetector::Assess(account, AnsiString(), submitted, CompromiseDetector::SourceWebmail, shape);
      const bool heldForReview = shape.hold;

      // The account's own submission, as the send route marks it
      // (Message::FlagSubmittedBySignedInUser), and as the account it is
      // (Message::GetSubmitterAccountID).
      message->SetSubmittedBySignedInUser(true);
      message->SetSubmitterAccountID(account->GetID());

      message->SetState(heldForReview ? Message::HeldForReview : Message::Delivering);
      if (!PersistentMessage::SaveObject(message))
      {
         FileUtilities::DeleteFile(fileName);
         return false;
      }

      if (shape.assessed)
         CompromiseDetector::Apply(shape, message->GetID(), message->GetFromAddress(), recipientList, data.GetSubject(), (__int64) message->GetSize());

      // A copy for \Sent, marked read, as the send route keeps one - not for
      // a held message, whose copy is made when it is released.
      if (!heldForReview)
         MessageUtilities::CopyToDesignatedSent(account, message);

      std::shared_ptr<IMAPFolders> folders = IMAPFolderContainer::Instance()->GetFoldersForAccount(account->GetID());
      if (folders)
      {
         std::shared_ptr<IMAPFolder> draftsFolder = folders->GetItemByDBIDRecursive(draft->GetFolderID());
         if (draftsFolder)
            Remove(draftsFolder, draft);
      }

      if (!heldForReview)
         Application::Instance()->SubmitPendingEmail();
      return true;
   }

   // The snoozed message back where it came from - or the inbox when that
   // folder is gone - and unread, so it is new again.
   bool
   ScheduledMailTask::ReturnSnoozed(std::shared_ptr<const Account> account, std::shared_ptr<Message> message, __int64 folderId)
   {
      std::shared_ptr<IMAPFolders> folders = IMAPFolderContainer::Instance()->GetFoldersForAccount(account->GetID());
      if (!folders)
         return false;
      std::shared_ptr<IMAPFolder> target = folders->GetItemByDBIDRecursive(folderId);
      if (!target)
         target = folders->GetFolderByName(_T("INBOX"));
      if (!target)
         return false;

      std::shared_ptr<Message> returned = message;
      if (target->GetID() != message->GetFolderID())
      {
         std::shared_ptr<IMAPFolder> source = folders->GetItemByDBIDRecursive(message->GetFolderID());
         __int64 newMessageId = 0;
         if (!MessageUtilities::CopyToIMAPFolder(message, (int) target->GetID(), newMessageId))
            return false;
         if (source)
            Remove(source, message);
         returned = std::shared_ptr<Message>(new Message());
         if (!PersistentMessage::ReadObject(returned, newMessageId) || returned->GetID() == 0)
            return true;
      }

      // On the folder's live message and its row as one step, as STORE does it.
      if (Application::Instance()->GetFolderManager()->ChangeMessageFlags(account->GetID(), target->GetID(), returned->GetID(),
             [](Message &live) { live.SetFlagSeen(false); return true; }, returned) == FolderManager::FlagChangeStored)
      {
         std::vector<__int64> changed;
         changed.push_back(returned->GetID());
         Notify(target, ChangeNotification::NotificationMessageFlagsChanged, changed);
      }
      return true;
   }
}
