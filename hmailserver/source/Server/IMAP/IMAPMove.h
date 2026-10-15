// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "IMAPCommandRangeAction.h"

#include <set>

namespace HM
{
   // Implements the message-move action used by the MOVE and UID MOVE
   // commands (RFC 6851). Each message is copied to the target folder and
   // recorded so that it can be expunged from the source folder afterwards.
   class IMAPMove : public IMAPCommandRangeAction
   {
   public:
      IMAPMove();

      virtual IMAPResult DoAction(std::shared_ptr<IMAPConnection> pConnection, int messageIndex, std::shared_ptr<Message> pOldMessage, const std::shared_ptr<IMAPCommandArgument> pArgument);

      // MOVE is atomic (RFC 6851 3.3), so nothing moves unless every message exists.
      virtual MissingMessagePolicy GetMissingMessagePolicy() const { return MissingMessagePolicy::FailBeforeActing; }

      // RFC 6851 section 4.3: the COPYUID response code in an untagged OK, sent
      // before ExpungeMovedMessages reports the originals gone. Nothing when no
      // message moved.
      void SendUIDPlusResponse(std::shared_ptr<IMAPConnection> pConnection);

      // Removes the successfully copied messages from the source folder and
      // sends the untagged EXPUNGE responses required by RFC 6851.
      void ExpungeMovedMessages(std::shared_ptr<IMAPConnection> pConnection);

      // RFC 6851 3.3: "The server SHOULD NOT leave any message in both mailboxes
      // ... This is true even if the server returns a tagged NO response to the
      // command." The copies this command made of the messages before the one that
      // could not be moved - over the quota, a file that would not copy - were left
      // in the destination with every original still in the source. They are taken
      // out again here as IMAPCopy::RollBack takes out a failed COPY's. MOVE and
      // UID MOVE call it when DoForMails has failed, and expunge nothing.
      void RollBack(std::shared_ptr<IMAPConnection> pConnection);

   private:

      // The destination and the copies made in it, so that a later message that
      // cannot be moved takes them out again (RollBack).
      std::shared_ptr<IMAPFolder> destination_folder_;
      std::set<__int64> copied_message_ids_;

      std::vector<unsigned int> moved_message_uids_;

      // Whether any message of this command went to a folder outside the
      // account it came from - a public folder, or another account's folder
      // shared under #Users. Such a move takes the message out of the mailbox,
      // so a legal hold on that mailbox keeps a copy of the original
      // (docs/Discovery.md).
      bool left_the_account_ = false;

      // The copies a MOVE made in the mailbox it was issued in - "MOVE 1 INBOX"
      // with INBOX selected. They are new messages there, announced with the
      // EXPUNGEs, in the command's own response (ExpungeMovedMessages).
      std::vector<__int64> copies_in_selected_;
   };
}
