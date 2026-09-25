// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "IMAPCommandRangeAction.h"

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

   private:

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
