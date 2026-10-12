// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "IMAPCommandRangeAction.h"

#include <set>

namespace HM
{
   class IMAPCopy  : public IMAPCommandRangeAction
   {
   public:
      IMAPCopy();

      virtual IMAPResult DoAction(std::shared_ptr<IMAPConnection> pConnection, int messageIndex, std::shared_ptr<Message> pOldMessage, const std::shared_ptr<IMAPCommandArgument> pArgument);

      // COPY is atomic (RFC 3501 6.4.7), so nothing is copied unless every message exists.
      virtual MissingMessagePolicy GetMissingMessagePolicy() const { return MissingMessagePolicy::FailBeforeActing; }

      // RFC 3501 6.4.7: "If the COPY command is unsuccessful for any reason, server
      // implementations MUST restore the destination mailbox to its state before the
      // COPY attempt." The copies this command made of the messages before the one
      // that could not be copied - over the quota, a file that would not copy - were
      // left in the destination with the command answered NO (upstream #650/#666).
      // COPY and UID COPY call this when DoForMails has failed.
      void RollBack(std::shared_ptr<IMAPConnection> pConnection);

      // A COPY into the folder the session has selected: the copies are taken into its
      // view and claimed as its \Recent, as an APPEND or a MOVE into it are, and the
      // untagged EXISTS and RECENT that say so are returned, to go before the tagged OK.
      // Empty for a copy into any other folder. Called before the other sessions are
      // told, so that none of them claims the copies' \Recent first. The copying
      // session was never told of them - NotificationServer tells every session but the
      // one that made the change - so its client saw neither the copies nor a count
      // that included them until it selected the folder again (upstream #666).
      String ReportCopiesInTheSelectedFolder(std::shared_ptr<IMAPConnection> pConnection);

   private:

      std::shared_ptr<IMAPFolder> destination_folder_;
      std::set<__int64> copied_message_ids_;


   };
}
