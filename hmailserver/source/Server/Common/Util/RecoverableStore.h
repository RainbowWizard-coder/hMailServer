// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Recover deleted items (Outlook's "Recover items deleted from this folder"):
// a message a person deletes - IMAP EXPUNGE, UID EXPUNGE and CLOSE, a POP3 DELE
// committed at QUIT, the webmail's delete and its emptying of a folder, their own
// retention choices - is copied aside before it goes, and for RecoverDeletedDays
// (0, the default, keeps nothing) it can be listed and put back.
//
// Only a delete made inside a Scope is kept: the same Messages::DeleteMessages
// also runs when an IMAP MOVE removes the original it has just copied, when a
// scheduled draft is sent, when an account is deleted - none of which is a
// person deleting mail, and none of which may leave a copy behind. So the places
// that are a person deleting mail say so, and nothing else is kept.
//
// The copy is a file under <DataDirectory>/Recoverable/<account id>/, and a row
// in hm_recoverable (schema 6068) naming the folder it came from, its subject,
// sender, size, flags and when it was deleted. The row goes when the message is
// put back or deleted for good, and the retention task's sweep ends what is
// older than the window, and any directory left by an account that is gone.
// The store is not in the backup: what a backup carries is what is in the
// mailboxes, and a restore brings that back.
#pragma once

namespace HM
{
   class Account;
   class Message;

   class RecoverableStore
   {
   public:
      // Marks the thread's deletes as a person's for as long as it lives.
      class Scope
      {
      public:
         Scope();
         ~Scope();
      };

      struct Entry
      {
         __int64 id = 0;
         __int64 folder_id = 0;
         String folder;
         String subject;
         String from;
         int size = 0;
         int flags = 0;
         __int64 deleted = 0;   // seconds since 1970, UTC
      };

      // The days a deleted message of this account is kept; 0 keeps none.
      // RecoverDeletedDays is a setting with levels (Application/EffectiveSetting.h,
      // recover_deleted_days): the account's own where an administrator gave it
      // one, its domain's, or the server's. An accountId of 0 answers the
      // server's.
      static int Days(__int64 accountId);

      // Called just before a message is deleted: keeps a copy when the delete is
      // inside a Scope, the store is on and the message is in an account's folder.
      static void Keep(std::shared_ptr<Message> message);

      static bool List(__int64 accountId, std::vector<Entry> &entries);

      // Puts one back into the folder it came from - or, when that folder is gone,
      // the one of the same name, or the INBOX. folderId and messageId are the new
      // message's. False with problem when it cannot be.
      static bool Restore(std::shared_ptr<const Account> account, __int64 id, __int64 &folderId, __int64 &messageId, String &problem);

      // Deleted for good, before the window ends.
      static bool Purge(__int64 accountId, __int64 id);

      // Ends what is older than its account's window - everything of an account
      // whose window is 0 - and removes the directories of accounts that are
      // gone. The number ended.
      static int Sweep();
   };
}
