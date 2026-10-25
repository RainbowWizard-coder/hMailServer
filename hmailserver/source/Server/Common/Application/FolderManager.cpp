// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "StdAfx.h"

#include "FolderManager.h"
#include "../BO/Account.h"
#include "../BO/IMAPFolders.h"
#include "../BO/IMAPFolder.h"

#include "../../IMAP/IMAPFolderContainer.h"
#include "../../IMAP/MessagesContainer.h"
#include "../../IMAP/IMAPConfiguration.h"
#include "../../IMAP/IMAPFolderUtilities.h"

#include "../Tracking/ChangeNotification.h"
#include "../Tracking/NotificationServer.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   FolderManager::FolderManager(void)
   {

   }

   FolderManager::~FolderManager(void)
   {

   }

   bool
   FolderManager::GetInboxMessages(int accountID, std::vector<std::shared_ptr<Message>> &result)
   {
      std::shared_ptr<IMAPFolders> folders = IMAPFolderContainer::Instance()->GetFoldersForAccount(accountID);
      if (!folders)
         return false;

      std::shared_ptr<IMAPFolder> inbox = folders->GetItemByName("Inbox");
      if (!inbox)
         return false;

      // get folder lock.
      std::set<__int64> recent_messages;
      auto messages = MessagesContainer::Instance()->GetMessages(inbox->GetAccountID(), inbox->GetID(), recent_messages, false);

      result = messages->GetCopy();

      return true;
   }

   bool
   FolderManager::DeleteInboxMessages(int accountID, std::set<int> uids, const std::function<void()> &callbackEvery1000Message)
   {
      std::shared_ptr<IMAPFolders> folders = IMAPFolderContainer::Instance()->GetFoldersForAccount(accountID);
      if (!folders)
         return false;

      std::shared_ptr<IMAPFolder> folder = folders->GetFolderByName("Inbox", false);
      if (!folder)
         return false;

      __int64 inboxID = folder->GetID();

      int count = 0;

      std::function<bool(std::shared_ptr<Message>)> filter = [&uids, &count, &callbackEvery1000Message](std::shared_ptr<Message> message)
         {
            count++;

            if ((count % 1000) == 0)
            {
               callbackEvery1000Message();
            }

            return uids.find(message->GetUID()) != uids.end();
         };

      auto messages = MessagesContainer::Instance()->GetMessages(folder->GetAccountID(), folder->GetID());

      // The ids of what was deleted are what the notification carries: a POP3 session
      // has no view, and every IMAP session that does numbers these for itself.
      std::vector<__int64> affectedMessages = messages->DeleteMessages(filter);

      if (affectedMessages.size() > 0)
      {
         // Notify the mailbox notifier that the mailbox contents have changed.
         std::shared_ptr<ChangeNotification> pNotification =
            std::shared_ptr<ChangeNotification>(new ChangeNotification(accountID, inboxID, ChangeNotification::NotificationMessageDeleted, affectedMessages));

         Application::Instance()->GetNotificationServer()->SendNotification(pNotification);
      }


      return true;
   }

   FolderManager::FlagChangeResult
   FolderManager::ChangeMessageFlags(__int64 accountID, __int64 folderID, __int64 messageID,
      const std::function<bool(Message &message)> &change, std::shared_ptr<Message> callersMessage)
   {
      std::shared_ptr<IMAPFolders> folders;
      if (accountID > 0)
         folders = IMAPFolderContainer::Instance()->GetFoldersForAccount(accountID);
      else
         folders = IMAPFolderContainer::Instance()->GetPublicFolders();

      if (!folders)
         return FlagChangeGone;

      std::shared_ptr<IMAPFolder> folder = folders->GetItemByDBIDRecursive(folderID);
      if (!folder)
         return FlagChangeGone;

      // Looked up before the message's lock is taken: the collection's own lock
      // is never held under it (see Message::GetFlagChangeMutex).
      std::shared_ptr<Message> message = folder->GetMessages()->GetItemByDBID(messageID);
      if (!message)
         return FlagChangeGone;

      const FlagChangeResult result = ChangeFlagsOf(message, change);

      // The caller resolved the message from a collection that has since been
      // replaced (the cache let it go and loaded the folder afresh): its object
      // is brought in line with what was stored, after the live one's lock is
      // released, so no two message locks are ever held at once.
      if (result == FlagChangeStored && callersMessage && callersMessage != message)
      {
         boost::lock_guard<boost::mutex> guard(callersMessage->GetFlagChangeMutex());
         const short recent = (short) Message::FlagRecent;
         callersMessage->SetFlagBits((short) ~recent, message->GetFlags());
         callersMessage->SetKeywords(message->GetKeywords());
         callersMessage->SetModSeq(message->GetModSeq());
      }

      return result;
   }

   FolderManager::FlagChangeResult
   FolderManager::ChangeFlagsOf(std::shared_ptr<Message> message, const std::function<bool(Message &message)> &change)
   {
      if (!message)
         return FlagChangeGone;

      boost::lock_guard<boost::mutex> guard(message->GetFlagChangeMutex());

      const short flagsBefore = message->GetFlags();
      const String keywordsBefore = message->GetKeywords();
      const __int64 modSeqBefore = message->GetModSeq();

      // Put back what this change altered, and only that. \Recent is not
      // taken under this lock - the collection clears it for every message at
      // a claim (Messages::ClaimRecentMessageIDs) - so it is never restored:
      // doing so could hand a claimed message its \Recent back.
      auto restore = [&]()
      {
         const short recent = (short) Message::FlagRecent;
         const short altered = (short) ((flagsBefore ^ message->GetFlags()) & ~recent);
         message->SetFlagBits(altered, flagsBefore);
         message->SetKeywords(keywordsBefore);
      };

      if (!change(*message))
      {
         restore();
         return FlagChangeDeclined;
      }

      if (!PersistentMessage::SaveFlags(message))
      {
         restore();
         message->SetModSeq(modSeqBefore);
         return FlagChangeNotStored;
      }

      return FlagChangeStored;
   }



}