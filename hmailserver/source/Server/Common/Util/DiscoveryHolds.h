// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A legal hold, as every path that deletes mail asks about it (docs/Discovery.md).
//
// A hold is a row of hm_discoveryholds (schema 6097) placed by a case: on an
// account, on a domain - every account in it - or on one archived copy. It is
// live while its holdreleased is 0. Nothing here writes a hold; DiscoveryStore
// places and releases them and calls Invalidate afterwards.
//
// WHAT ASKS. Every place this server deletes a mailbox's mail, or the mailbox:
//
//   RecoverableStore::Keep     a held account's message is copied into its
//                              deleted items before ANY delete but the original
//                              of a move that stays inside the account (an IMAP
//                              MOVE into a public folder or another account's
//                              folder is a delete here), whatever
//                              RecoverDeletedDays says - and when the copy
//                              cannot be made the delete is refused
//                              (Messages::DeleteMessages, Messages::Save)
//   RecoverableStore::Sweep    skips a held account's copies
//   DELETE /api/v1/me/recoverable/{id}   refused for a held account (409)
//   LocalDelivery              an account rule's Delete or Reject, a Sieve
//                              discard, reject or redirect without keep, a
//                              forward that keeps no copy and the account's own
//                              spam delete threshold: the copy they would have
//                              dropped goes into the deleted items instead -
//                              sealed first for a zero-access mailbox - and when
//                              it cannot be kept the message is delivered
//   MailboxRetentionTask       leaves a held account alone
//   MailboxRestore             replace is refused for a held account, at the
//                              request and again when the job runs
//   BackupExecuter             a full restore with the domains option is
//                              refused before its first deletion while any
//                              account or domain is held
//   MailboxMove                an account held, directly or through its domain,
//                              is not moved to another domain
//   MailboxKeys::Forfeit       a held account's mailbox key - the only way to
//                              its mail encrypted at rest - is not removed
//                              (DELETE .../mailbox-encryption?confirm=forfeit)
//   PersistentAccount          the account's deletion, and the deletion of all
//                              of its messages at once, are refused
//   PersistentDomain           the domain's deletion is refused while it or any
//                              account in it is held
//   PersistentIMAPFolder       a held account's folder is not deleted while a
//                              message the hold could not keep is still in it
//   PersistentArchiveIndex     a held archive copy - by its own hold, or by its
//                              account's or domain's - is not swept by the
//                              archive's retention nor erased with an address
//
// THE COPY IN MEMORY. The questions are asked on every delete of every account,
// so they are answered from the live holds read into memory, read again at the
// next question after a hold is placed or released in this process and at most
// five seconds after anything else changed them. A read that fails is answered as
// held for everything, and said once a minute (HM7332): a hold that could not be
// read is not a hold that was released, and a delete that has to keep a copy
// while the database is away fails as the delete itself would have.
#pragma once

#include <set>

namespace HM
{
   class DiscoveryHolds
   {
   public:
      enum Scope
      {
         ScopeAccount = 1,
         ScopeDomain = 2,
         ScopeArchive = 3
      };

      // True while a live hold covers the account: a hold on it, or on its
      // domain. False for an id of 0 or less - a message in a public folder.
      // True, as every question is, when the holds could not be read, and
      // when a hold on some domain stands and the account itself could not
      // be read to learn its domain.
      static bool IsAccountHeld(__int64 accountId);

      // True while a live hold covers the domain, or any account in it.
      static bool IsDomainOrAnyAccountHeld(__int64 domainId);

      // True while any account or any domain on the server is held, or when
      // the holds could not be read; held then names some of what is held -
      // up to five domains and addresses - and is empty when the holds could
      // not be read. A full restore asks it before its first deletion, since
      // it deletes every domain.
      static bool AnyAccountOrDomainHeld(String &held);

      // Whether the last read of the holds failed, in which case every
      // question above is being answered "held". The retention sweep says so
      // once rather than naming a hold on every account.
      static bool ReadFailed();

      // A deletion that undoes an account the same request made a moment
      // before - PersistentAccount::SaveObject when the inbox could not be
      // made, SCIM's create when an alias of it was refused - is not a
      // deletion a hold was placed to stop: nothing was ever in the account.
      // For as long as one of these lives, on its thread, IsAccountHeld is not
      // asked by PersistentAccount::DeleteObject.
      class UndoingCreation
      {
      public:
         UndoingCreation();
         ~UndoingCreation();

         static bool Active();
      };

      // True while a live hold covers an archived copy: a hold on the entry
      // itself, or on the account (domain and local part, as the archive files
      // it) or the domain it was archived for. archiveId 0 is a file the index
      // has no row for, judged by where it lies alone.
      static bool IsArchiveEntryHeld(__int64 archiveId, const String &domain, const String &mailbox);

      // The copy in memory is read again at the next question.
      static void Invalidate();

      // The sentences the refusals carry, one each, so that REST, COM and the
      // persistence layer say the same thing.
      static String DeleteAccountRefusal(const String &address);
      static String DeleteDomainRefusal(const String &domain);
      static String DeleteMessagesRefusal(const String &address);
      static String ReplaceRefusal(const String &address);
      static String PurgeRefusal();
      static String MoveRefusal(const String &address, const String &domain);
      static String RestoreRefusal(const String &held);

   private:

      // Reads the live holds when there is no copy, when it was invalidated or
      // when it is more than five seconds old. Called with the lock held.
      static void EnsureLoaded_();
   };
}
