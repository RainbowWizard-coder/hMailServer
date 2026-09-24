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
// that are a person deleting mail say so, and nothing else is kept for recovery.
// A legal hold is the other reason to keep a copy, and asks no Scope (Keep).
//
// The copy is a file under <DataDirectory>/Recoverable/<account id>/, and a row
// in hm_recoverable (schema 6068) naming the folder it came from, its subject,
// sender, size, flags and when it was deleted. The row goes when the message is
// put back or deleted for good, and the retention task's sweep ends what is
// older than the window, and any directory left by an account that is gone.
// The store is not in the backup: what a backup carries is what is in the
// mailboxes, and a restore brings that back and leaves this directory where it
// is (BackupExecuter's IsKeptOutOfBackup_), since hm_recoverable is not in the
// backup either and its rows name these files.
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

      // Marks the thread's deletes as the originals of a move inside one
      // account - an IMAP MOVE whose every destination is a folder of the
      // account the messages came from, the webmail's move and its delete to
      // Trash, each of which copies the message first and none of which may
      // leave the account - for as long as it lives. A legal hold keeps
      // nothing of them, since the message is still in the mailbox; what
      // RecoverDeletedDays keeps is decided by Scope alone, as before. An IMAP
      // MOVE into a public folder or into another account's folder is not
      // opened in one (IMAPMove::ExpungeMovedMessages): the message has left
      // the mailbox, so its original is kept as any delete's is.
      class MoveScope
      {
      public:
         MoveScope();
         ~MoveScope();
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
         String file;           // the copy, relative to the store's directory
      };

      // The days a deleted message of this account is kept; 0 keeps none.
      // RecoverDeletedDays is a setting with levels (Application/EffectiveSetting.h,
      // recover_deleted_days): the account's own where an administrator gave it
      // one, its domain's, or the server's. An accountId of 0 answers the
      // server's.
      static int Days(__int64 accountId);

      // Called just before a message is deleted: keeps a copy when the delete is
      // inside a Scope, the store is on and the message is in an account's folder
      // - and, whatever RecoverDeletedDays says and whether or not the delete is
      // inside a Scope, when a legal hold covers the account and the delete is
      // not a move's (docs/Discovery.md). False only when a held message's copy
      // could not be made: the caller must then not delete it, because a hold
      // that let the message go when its copy failed would be no hold. True in
      // every other case, a copy that failed for an account under no hold
      // included, which is deleted without one as it always was.
      //
      // sealPlaintext says the file is a zero-access mailbox's copy that has
      // not been sealed yet - one a filter drops at delivery, before the seal
      // (LocalDelivery::KeepHeldCopy_). The kept copy is then sealed to the
      // mailbox's key as the mailbox would have held it (ZeroAccess::SealFile,
      // which leaves a message that is under a key already as it is), and when
      // it cannot be sealed its plaintext is scrubbed and nothing is kept: the
      // answer is then the one for a copy that could not be made. Nothing
      // changes for an account without zero access.
      static bool Keep(std::shared_ptr<Message> message, bool sealPlaintext = false);

      static bool List(__int64 accountId, std::vector<Entry> &entries);

      // Where an entry's copy is on the disk, and the same for one entry by its
      // id: false when the account has no such entry.
      static String KeptFile(const Entry &entry);
      static bool KeptFileOf(__int64 accountId, __int64 id, String &path);

      // Puts one back into the folder it came from - or, when that folder is gone,
      // the one of the same name, or the INBOX. folderId and messageId are the new
      // message's. False with problem when it cannot be.
      static bool Restore(std::shared_ptr<const Account> account, __int64 id, __int64 &folderId, __int64 &messageId, String &problem);

      // Deleted for good, before the window ends.
      static bool Purge(__int64 accountId, __int64 id);

      // Ends what is older than its account's window - everything of an account
      // whose window is 0 - and removes the directories of accounts that are
      // gone. A held account's copies are not ended, however old: they are what
      // the hold keeps. The number ended.
      static int Sweep();
   };
}
