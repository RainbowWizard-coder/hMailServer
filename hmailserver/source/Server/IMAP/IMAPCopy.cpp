// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "../Common/AntiSpam/SpamAssassin/SpamAssassinLearner.h"
#include "IMAPCopy.h"
#include "IMAPConnection.h"
#include "../Common/BO/Message.h"
#include "../Common/BO/Account.h"
#include "../Common/BO/IMAPFolder.h"
#include "../Common/Persistence/PersistentMessage.h"
#include "IMAPSimpleCommandParser.h"
#include "../Common/BO/ACLPermission.h"
#include "../Common/Tracking/ChangeNotification.h"
#include "../Common/Tracking/NotificationServer.h"
#include "../Common/BO/Messages.h"


#include "MessagesContainer.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   IMAPCopy::IMAPCopy()
   {
      
   }


   IMAPResult
   IMAPCopy::DoAction(std::shared_ptr<IMAPConnection> pConnection, int messageIndex, std::shared_ptr<Message> pOldMessage, const std::shared_ptr<IMAPCommandArgument> pArgument)
   {
      if (!pArgument || !pOldMessage)
         return IMAPResult(IMAPResult::ResultBad, "Invalid parameters");
      
      std::shared_ptr<IMAPSimpleCommandParser> pParser = std::shared_ptr<IMAPSimpleCommandParser>(new IMAPSimpleCommandParser());

      pParser->Parse(pArgument);
      
      if (pParser->WordCount() <= 0)
         return IMAPResult(IMAPResult::ResultNo, "The command requires parameters.");

      String sFolderName;
      if (pParser->Word(0)->Clammerized())
         sFolderName = pArgument->Literal(0);
      else
      {
         sFolderName = pParser->Word(0)->Value();
         IMAPFolder::UnescapeFolderString(sFolderName);
      }

      // RFC 3501 6.4.7: a destination that does not exist is refused NO with
      // [TRYCREATE], so the client can create it and try again. It was BAD,
      // which tells the client its command was malformed.
      std::shared_ptr<IMAPFolder> pFolder = pConnection->GetFolderByFullPath(sFolderName);
      if (!pFolder)
         return IMAPResult(IMAPResult::ResultNo, "[TRYCREATE] The folder could not be found.");

      // The quota that matters is the DESTINATION folder owner's, because that
      // is the mailbox the copy will occupy - for an ordinary folder that is the
      // caller, and for a delegated one it is the owner. Charging the caller let
      // a delegate fill somebody else's mailbox past its limit while being
      // refused at their own.
      std::shared_ptr<const Account> pDestinationOwner = pConnection->GetAccountOwningFolder(pFolder);

      if (!pFolder->IsPublicFolder())
      {
         if (!pDestinationOwner)
            return IMAPResult(IMAPResult::ResultNo, "The destination folder could not be resolved.");

         if (!pDestinationOwner->SpaceAvailable(pOldMessage->GetSize()))
            return IMAPResult(IMAPResult::ResultNo, "[OVERQUOTA] Your quota has been exceeded.");
      }

      // Check if the user has permission to copy to this destination folder
      if (!pConnection->CheckPermission(pFolder, ACLPermission::PermissionInsert))
         return IMAPResult(IMAPResult::ResultNo, "[NOPERM] ACL: Insert permission denied (Required for COPY command).");

      std::shared_ptr<Message> pNewMessage = PersistentMessage::CopyToIMAPFolder(pOldMessage, pFolder);

      if (!pNewMessage)
         return IMAPResult(IMAPResult::ResultNo, "Failed to copy message");

      // Only the flags the user may set in the destination (RFC 4314 section 4).
      KeepOnlySettableFlags(pConnection, pFolder, pNewMessage);

      if (!PersistentMessage::SaveObject(pNewMessage))
         return IMAPResult(IMAPResult::ResultNo, "Failed to save copy of message.");

      // Kept, so that a later message that cannot be copied takes this one out again.
      destination_folder_ = pFolder;
      copied_message_ids_.insert(pNewMessage->GetID());

      // A move into or out of the user's own Junk folder is a lesson for spamd
      // (SpamAssassinLearnOnMove): decided here, delivered off this thread.
      SpamAssassinLearner::LearnFromMove(pConnection->GetCurrentFolder(), pFolder, pNewMessage, pDestinationOwner);

      // RFC 4315 (UIDPLUS): remember the source/destination UIDs for the COPYUID
      // response - for a destination the user may read (RFC 4315 section 6).
      RecordCopyUidIfReadable(pConnection, pFolder, pOldMessage->GetUID(), pNewMessage->GetUID());

      MessagesContainer::Instance()->SetFolderNeedsRefresh(pFolder->GetID());

      // Set a delayed notification so that the any IMAP idle client is notified when this
      // command has been finished.
      std::shared_ptr<ChangeNotification> pNotification = 
         std::shared_ptr<ChangeNotification>(new ChangeNotification(pFolder->GetAccountID(), pFolder->GetID(), ChangeNotification::NotificationMessageAdded));

      pConnection->SetDelayedChangeNotification(pNotification);

      return IMAPResult();
   }

   void
   IMAPCopy::RollBack(std::shared_ptr<IMAPConnection> pConnection)
   {
      if (copied_message_ids_.empty() || !destination_folder_)
         return;

      // Nobody is told of copies that are not there any more.
      pConnection->SetDelayedChangeNotification(std::shared_ptr<ChangeNotification>());

      std::shared_ptr<Messages> messages = MessagesContainer::Instance()->GetMessages(destination_folder_->GetAccountID(), destination_folder_->GetID());

      std::vector<__int64> deleted = messages->DeleteMessagesById(copied_message_ids_);

      // A session that has the destination open may have read them already.
      if (!deleted.empty())
      {
         std::shared_ptr<ChangeNotification> pNotification =
            std::shared_ptr<ChangeNotification>(new ChangeNotification(destination_folder_->GetAccountID(), destination_folder_->GetID(), ChangeNotification::NotificationMessageDeleted, deleted));

         Application::Instance()->GetNotificationServer()->SendNotification(pConnection->GetNotificationClient(), pNotification);
      }

      copied_message_ids_.clear();
   }
}
