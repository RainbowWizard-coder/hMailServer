// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"
#include "../Common/AntiSpam/SpamAssassin/SpamAssassinLearner.h"
#include "IMAPMove.h"
#include "IMAPConnection.h"
#include "../Common/BO/Message.h"
#include "../Common/BO/Account.h"
#include "../Common/BO/IMAPFolder.h"
#include "../Common/BO/Messages.h"
#include "../Common/Persistence/PersistentMessage.h"
#include "../Common/Util/RecoverableStore.h"
#include "IMAPSimpleCommandParser.h"
#include "../Common/BO/ACLPermission.h"
#include "../Common/Tracking/ChangeNotification.h"
#include "../Common/Tracking/NotificationServer.h"

#include "MessagesContainer.h"
#include "IMAPFolderView.h"
#include "IMAPNotificationClient.h"

#include <algorithm>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   IMAPMove::IMAPMove()
   {

   }

   IMAPResult
   IMAPMove::DoAction(std::shared_ptr<IMAPConnection> pConnection, int messageIndex, std::shared_ptr<Message> pOldMessage, const std::shared_ptr<IMAPCommandArgument> pArgument)
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

      // Check if the user has permission to insert into the destination folder.
      if (!pConnection->CheckPermission(pFolder, ACLPermission::PermissionInsert))
         return IMAPResult(IMAPResult::ResultNo, "[NOPERM] ACL: Insert permission denied (Required for MOVE command).");

      std::shared_ptr<Message> pNewMessage = PersistentMessage::CopyToIMAPFolder(pOldMessage, pFolder);

      if (!pNewMessage)
         return IMAPResult(IMAPResult::ResultNo, "Failed to move message");

      // Only the flags the user may set in the destination (RFC 4314 section 4).
      KeepOnlySettableFlags(pConnection, pFolder, pNewMessage);

      if (!PersistentMessage::SaveObject(pNewMessage))
         return IMAPResult(IMAPResult::ResultNo, "Failed to save moved message.");

      // Kept, so that a later message that cannot be moved takes this one out again.
      destination_folder_ = pFolder;
      copied_message_ids_.insert(pNewMessage->GetID());

      // A move into or out of the user's own Junk folder is a lesson for spamd
      // (SpamAssassinLearnOnMove): decided here, and given, off this thread, once the
      // command has succeeded (GiveLessons). A MOVE that fails takes this copy out
      // again (RollBack), and a lesson queued now would tell spamd of it all the same.
      bool spam = false;
      if (SpamAssassinLearner::IsLesson(pConnection->GetCurrentFolder(), pFolder, pDestinationOwner, spam))
         HoldLesson(pNewMessage, pDestinationOwner, spam);

      // RFC 4315/6851 (UIDPLUS): remember the source/destination UIDs for the COPYUID
      // response - for a destination the user may read (RFC 4315 section 6).
      RecordCopyUidIfReadable(pConnection, pFolder, pOldMessage->GetUID(), pNewMessage->GetUID());

      MessagesContainer::Instance()->SetFolderNeedsRefresh(pFolder->GetID());

      // Notify any IMAP idle client watching the target folder.
      std::shared_ptr<ChangeNotification> pNotification =
         std::shared_ptr<ChangeNotification>(new ChangeNotification(pFolder->GetAccountID(), pFolder->GetID(), ChangeNotification::NotificationMessageAdded));

      pConnection->SetDelayedChangeNotification(pNotification);

      // A move that leaves the account - into a public folder, or into another
      // account's folder - is a delete as far as the source mailbox is
      // concerned: the copy belongs to someone else now, who may expunge it.
      std::shared_ptr<IMAPFolder> pSourceFolder = pConnection->GetCurrentFolder();
      if (pFolder->IsPublicFolder() || !pSourceFolder || pFolder->GetAccountID() != pSourceFolder->GetAccountID())
         left_the_account_ = true;

      // Remember the message so that it can be expunged from the source folder.
      moved_message_uids_.push_back(pOldMessage->GetUID());

      if (pSourceFolder && pFolder->GetID() == pSourceFolder->GetID())
         copies_in_selected_.push_back(pNewMessage->GetID());

      return IMAPResult();
   }

   void
   IMAPMove::SendUIDPlusResponse(std::shared_ptr<IMAPConnection> pConnection)
   {
      // "[COPYUID <validity> <source> <destination>] ", or empty.
      String sUidPlus = GetUIDPlusResponseCode();

      if (sUidPlus.IsEmpty())
         return;

      pConnection->SendAsciiData(_T("* OK ") + sUidPlus + _T("Moved UIDs.\r\n"));
   }

   void
   IMAPMove::RollBack(std::shared_ptr<IMAPConnection> pConnection)
   {
      // Nothing moved: no original is expunged, no COPYUID names a copy that is taken
      // out again, and spamd is taught nothing of it.
      moved_message_uids_.clear();
      copies_in_selected_.clear();
      left_the_account_ = false;
      ForgetCopyUids();
      ForgetLessons();

      if (copied_message_ids_.empty() || !destination_folder_)
         return;

      // Nobody is told of copies that are not there any more.
      pConnection->SetDelayedChangeNotification(std::shared_ptr<ChangeNotification>());

      std::shared_ptr<Messages> messages = MessagesContainer::Instance()->GetMessages(destination_folder_->GetAccountID(), destination_folder_->GetID());

      // Deleted as a failed COPY's copies are, and as any delete is - outside a
      // RecoverableStore::MoveScope, since these are not a move's originals - so a
      // legal hold on the destination's account keeps a copy of each, and a copy the
      // hold cannot keep, or the database will not delete, stays where it is.
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

   void
   IMAPMove::ExpungeMovedMessages(std::shared_ptr<IMAPConnection> pConnection)
   {
      if (moved_message_uids_.empty())
         return;

      std::shared_ptr<IMAPFolder> pCurFolder = pConnection->GetCurrentFolder();
      if (!pCurFolder)
         return;

      std::shared_ptr<IMAPFolderView> view = pConnection->GetCurrentFolderView();
      if (!view)
         return;

      std::set<unsigned int> moved_uids(moved_message_uids_.begin(), moved_message_uids_.end());

      std::function<bool(std::shared_ptr<Message>)> filter = [&moved_uids](std::shared_ptr<Message> message)
      {
         return moved_uids.find(message->GetUID()) != moved_uids.end();
      };

      auto messages = MessagesContainer::Instance()->GetMessages(pCurFolder->GetAccountID(), pCurFolder->GetID());

      std::vector<__int64> deleted_message_ids;
      if (left_the_account_)
      {
         // At least one message left the account: the originals are deleted
         // as any delete is, so a legal hold on the account keeps a copy of
         // each (RecoverableStore::Keep), and one whose copy cannot be made
         // stays where it is.
         deleted_message_ids = messages->DeleteMessages(filter);
      }
      else
      {
         // The originals of a move within the account, copied already: the
         // message is still in the mailbox, so a legal hold keeps nothing of
         // them (RecoverableStore::MoveScope).
         RecoverableStore::MoveScope moving;
         deleted_message_ids = messages->DeleteMessages(filter);
      }

      // RFC 6851: the untagged EXPUNGE responses for the source folder - numbered by this
      // session's view, or as one VANISHED under QRESYNC (RFC 6851 3.3).
      std::vector<unsigned int> expunged_uids;
      std::vector<int> expunged_sequences = view->RemoveMessages(deleted_message_ids, &expunged_uids);

      pConnection->RemoveRecentMessages(deleted_message_ids);

      String sResponse = IMAPNotificationClient::FormatExpungeResponses(pConnection, expunged_sequences, expunged_uids);

      // A move into the selected mailbox itself leaves a new copy of each
      // message there. It is reported now, after the EXPUNGEs, as an APPEND to
      // the selected mailbox reports its message - not left for the next
      // command, which is where it used to come. imaptest's move test found it:
      // "MOVE 1 <the mailbox>" answered the EXPUNGE and no EXISTS. The copies
      // are this session's \Recent, as an appended message is.
      if (!copies_in_selected_.empty())
      {
         view->AppendNewMessages(messages);

         for (__int64 copyId : copies_in_selected_)
         {
            pConnection->AddRecentMessage(copyId);

            // Claimed here, so off the shared collection: another session with
            // the mailbox open must not report the same message as \Recent too.
            std::shared_ptr<Message> copy = messages->GetItemByDBID((unsigned __int64) copyId);
            if (copy)
               copy->SetFlagRecent(false);
         }

         sResponse += IMAPNotificationClient::GenerateExistsString(view->GetMessageCount());
         sResponse += IMAPNotificationClient::GenerateRecentString((int) pConnection->GetRecentMessageCount());
      }

      if (!sResponse.IsEmpty())
         pConnection->SendAsciiData(sResponse);

      if (!deleted_message_ids.empty())
      {
         // Notify the mailbox notifier that the source folder contents changed.
         std::shared_ptr<ChangeNotification> pNotification =
            std::shared_ptr<ChangeNotification>(new ChangeNotification(pCurFolder->GetAccountID(), pCurFolder->GetID(), ChangeNotification::NotificationMessageDeleted, deleted_message_ids));

         Application::Instance()->GetNotificationServer()->SendNotification(pConnection->GetNotificationClient(), pNotification);
      }
   }
}
