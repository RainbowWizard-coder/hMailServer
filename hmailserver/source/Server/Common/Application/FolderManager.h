// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{
   class Message;
   class Account;
   class IMAPFolder;

   class FolderManager
   {
   public:
      FolderManager(void);
      ~FolderManager(void);

      bool GetInboxMessages(int accountID, std::vector<std::shared_ptr<Message> > &result);
      bool DeleteInboxMessages(int accountID, std::set<int> uids, const std::function<void()> &callbackEvery1000Message);

      // What ChangeMessageFlags did.
      enum FlagChangeResult
      {
         // Changed, and written to hm_messages with the folder's next mod-sequence.
         FlagChangeStored = 0,
         // The change said no (it returned false): nothing changed, nothing written.
         FlagChangeDeclined = 1,
         // The folder, or the message in it, is not there any more - expunged
         // by another session after the caller resolved it.
         FlagChangeGone = 2,
         // The write failed: the message is left as it was, in memory too.
         FlagChangeNotStored = 3
      };

      // Changes one stored message's flags and keywords as a single step. The
      // folder's live message - the one object every session on the mailbox
      // shares - is looked up, and under its flag-change lock (Message::
      // GetFlagChangeMutex) change is applied to it and the result written,
      // flags and keywords in one statement with one mod-sequence.
      //
      // This replaces UpdateMessageFlags(..., flags, keywords), which took the
      // values the caller had computed from what it read earlier and wrote them
      // over the live message: two STOREs on one message at the same moment -
      // one adding \Seen, the other \Flagged - could each write back what it
      // had read, and one change was lost in memory and in the database. With
      // the read, the change and the write under one lock, the second change
      // starts from the first one's result.
      //
      // change runs with the lock held and must touch nothing but the message
      // it is given (no other lock, no I/O); it returns false to leave the
      // message as it was, which is then restored. callersMessage, when given
      // and not the live object (a collection reloaded meanwhile), is brought
      // in line with the result afterwards, so the caller can answer from it.
      FlagChangeResult ChangeMessageFlags(__int64 accountID, __int64 folderID, __int64 messageID,
         const std::function<bool(Message &message)> &change,
         std::shared_ptr<Message> callersMessage = std::shared_ptr<Message>());

      // The same step on a message the caller already holds the live object of.
      static FlagChangeResult ChangeFlagsOf(std::shared_ptr<Message> message, const std::function<bool(Message &message)> &change);

   private:


   };
}