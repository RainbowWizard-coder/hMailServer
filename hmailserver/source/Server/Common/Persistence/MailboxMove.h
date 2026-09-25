// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Renaming a mailbox, and moving one to another domain on this server, as one
// operation that keeps the account's identity.
//
// WHAT THE ACCOUNT IS, AND WHAT THEREFORE HAS TO MOVE
//
// An account is a row of hm_accounts with an identity column, a directory tree
// under the data directory named after its domain and its address, and a Sieve
// tree of its own outside that. Nearly everything else the account owns hangs
// off the identity: its rules, its folders and their subscriptions and access
// control entries, its application passwords, its second factor, its contacts,
// its calendars, its preferences, its known senders, its password history, its
// safe and blocked senders, its group memberships and its role assignment are
// all keyed on accountid and follow the row without being touched. That is why
// this is a rename and not a copy: a copy loses every one of them.
//
// Three kinds of thing do NOT follow the identity, and each is the reason a
// naive rename is a defect rather than a feature:
//
//   the store    PersistentMessage stores a PARTIAL file name - the GUID - and
//                builds the full path at read time from the ACCOUNT ADDRESS
//                (<data>/<domain>/<local part>/<2 chars>/<guid>.eml). Change
//                the address without moving the directory and every message in
//                the mailbox becomes unreadable; move the directory under the
//                old domain, as NameChanger::RenameAccount did, and a move to
//                another domain leaves the store where nothing will look for
//                it. An installation still holding FULL paths in
//                messagefilename cannot be renamed at all, which is the check
//                PersistentAccount::SaveObject has always made and this makes
//                too.
//
//   references   An alias whose VALUE is the old address, a distribution list
//                that has the old address as a member, and a recipient of a
//                message still in the delivery queue all name the mailbox by
//                its address as text. They are rewritten here.
//
//   the address  The quarantine names its recipients as text inside one
//                column, so an entry held for this mailbox is rewritten too.
//
// WHAT IS DELIBERATELY LEFT WHERE IT IS
//
// The archive (ArchiveDir and hm_archiveindex) and the message trace record
// what happened at an address at a time. Rewriting them would make this
// server's record of the past disagree with every other party's copy of the
// same messages, so they stay under the old address and the documents say so.
//
// EITHER IT COMPLETES OR THE ACCOUNT IS AS IT WAS
//
// There is no transaction across a filesystem and four database backends, so
// the order is chosen to make every step undoable by the step that follows it
// failing: the store is moved first (a copy and then a delete, so a failure
// leaves the original), then the account row (one UPDATE), then the references
// (one UPDATE each), then the alias. A failure at any point puts back what the
// earlier steps did, in reverse, and reports what it could not put back.
//
// The one thing that is NOT put back by anything is a message DELIVERED while
// the store is being copied. Delivery builds its path from the account address
// and the account row still carries the old one until the store step has
// finished, so a message that arrives while the copy is running is written
// under the old directory: if the copy has not reached that bucket yet it is
// carried across, and if it has, it is deleted with the rest of the old tree.
// Nothing in the server records a delivery in progress, so it cannot be waited
// for the way a POP3 lock can. What IS closed is the window after the delete:
// the caches are dropped the moment the account row carries the new address,
// and the old directory is then swept once for anything that landed in it
// after the copy passed (SweepLateArrivals_). The remaining window is the copy
// itself, and the documents say so rather than promise otherwise.
//
// What survives the process being killed is the journal, hm_mailboxmoves
// (schema 6050): a row opened before the first step and closed after the last,
// so a row left at a stage short of StageComplete names a move that was
// interrupted and which half of the store to look at. Each stage is written
// BEFORE the step it names where that step is not one statement - the store
// move is a copy and a delete that takes as long as the mailbox is big - so a
// row never says "nothing has been changed" about a tree that is half moved.
// docs/MailboxMove.md says what each stage leaves behind.
//
// ONE ENTRY ON THE AUDIT TRAIL
//
// The rows a move writes are all one change. Left to the statement chokepoint,
// the trail would carry an "account updated" for the address column, an "alias
// created" for the address left behind and a row for every list membership
// rewritten - several entries for one thing an administrator did once, none of
// which names both addresses. So the writing runs with no actor on the thread
// (an AuditScope carrying an absent actor, exactly as ScriptServer's boundary
// does) and the move is recorded once, afterwards, as "account moved" naming
// the address it left, the address it took, what it carried and whether the old
// address was kept. A move that failed and could not be put back is recorded
// too, saying what was left where; a move that failed and WAS put back is not,
// because nothing changed.
//
// IN USE
//
// A move is refused while something holds the mailbox, because a session that
// is already open holds an Account object carrying the OLD address and would
// keep building paths from it. Two things are askable of the running server
// and both are asked: the POP3 mailbox lock (POP3Sessions, held from
// authentication to QUIT) and an IMAP session with a mailbox of this account
// selected (NotificationServer's message-change subscribers, which is what a
// SELECT registers). An IMAP session that has authenticated and selected
// nothing is not counted, because nothing records it; neither is a webmail
// request, a ManageSieve session, or - the one that can cost a message rather
// than only an error - an SMTP delivery to this mailbox in flight. The
// documents say all four plainly.

#pragma once

#include <vector>

namespace HM
{
   class Account;
   class DALRecordset;

   // What one move carried. Every count is what was actually written, not what
   // was expected: a summary that says "12 messages" for a move that carried
   // none is worse than one that says nothing.
   struct MailboxMoveOutcome
   {
      __int64 journal_id = 0;
      __int64 account_id = 0;
      String old_address;
      String new_address;
      String old_domain;
      String new_domain;
      bool alias_kept = false;
      bool store_moved = false;
      // A rewrite that FAILED writes nothing and counts nothing, so a zero
      // beside it would read as "there was nothing to do". This says which it
      // was: true means at least one of the four rewrites below could not be
      // made, and HM6942 to HM6945 in the application log say which.
      bool references_incomplete = false;
      int messages = 0;
      int folders = 0;
      int aliases_repointed = 0;
      int list_memberships = 0;
      int queued_recipients = 0;
      int quarantine_entries = 0;
   };

   // One row of hm_mailboxmoves.
   struct MailboxMoveRecord
   {
      __int64 id = 0;
      __int64 account_id = 0;
      String old_address;
      String new_address;
      int stage = 0;
      bool keep_alias = false;
      int messages = 0;
      String started;
      String finished;
      String detail;
   };

   class MailboxMove
   {
   public:
      // hm_mailboxmoves.mmstage. The numbers are stored, so they never change
      // meaning; a stage added later takes the next number.
      enum Stage
      {
         StageStarted = 0,             // nothing has been changed yet
         StageStoreMoved = 1,          // the mailbox and Sieve trees are at the new address
         StageAccountWritten = 2,      // hm_accounts carries the new address and domain
         StageReferencesRewritten = 3, // aliases, list memberships, the queue and the quarantine
         StageComplete = 4,            // the alias (if any) exists and the caches are clear
         StageRolledBack = 5,          // a step failed and everything before it was put back
         StageRollbackFailed = 6,      // a step failed and something could not be put back
         // Written BEFORE the copy begins, and the reason the numbers are not
         // in order: StageStarted has to go on meaning "nothing has been
         // changed", and a process killed part way through a copy that takes
         // minutes would otherwise leave a row saying exactly that about a
         // mailbox whose tree is at the new address and whose old tree is gone.
         StageMovingStore = 7          // the trees are somewhere between the two addresses
      };

      static String StageName(int stage);

      // Why this account cannot be moved at this moment, in a sentence, or an
      // empty string when nothing holds it.
      static String InUseReason(std::shared_ptr<const Account> account);

      // Refuses, with a sentence, everything that cannot be done safely, and
      // otherwise moves the account. On false the account is as it was, unless
      // errorMessage says otherwise - which it does, naming what was left, in
      // the one case where putting something back also failed.
      //
      // newAddress carries the domain, so a rename and a move to another domain
      // are the same call; keepOldAddressAsAlias leaves the old address behind
      // as an alias pointing at the new one, which is the default every caller
      // offers because it is what keeps mail flowing.
      static bool Move(std::shared_ptr<Account> account, const String &newAddress, bool keepOldAddressAsAlias,
                       MailboxMoveOutcome &outcome, String &errorMessage);

      // The same move, for the door that has always existed: setting
      // Account.Address and saving. PersistentAccount::SaveObject calls this
      // where it used to call NameChanger::RenameAccount, so that there is one
      // implementation of "this account's address changed" and not two - the
      // one that carried the mailbox and nothing else is what left aliases
      // pointing at an address the account no longer had.
      //
      // Two things differ from Move, and both are compatibility rather than
      // design. It leaves no alias, because that setter never has and a script
      // that renames in a loop would otherwise fill the domain with them. And
      // it does not refuse while the mailbox is in use: the POP3 lock is
      // released when the session's socket closes, which is after the client
      // has read the reply to QUIT, so a script that reads a mailbox and then
      // renames it would fail on a race it cannot see. The explicit operation
      // refuses, says why, and can be retried; this one behaves as it did.
      static bool Rename(std::shared_ptr<Account> account, const String &newAddress, String &errorMessage);

      // What a completed move did, in a sentence - the journal's detail, the
      // audit trail's detail and the application log's line, so the three
      // cannot describe the same move differently.
      static String Describe(const MailboxMoveOutcome &outcome);

      // The journal, newest first. accountID 0 is every account's.
      static bool List(__int64 accountID, int limit, std::vector<MailboxMoveRecord> &rows);

      // The newest row for one account, or false when it has never been moved.
      static bool Newest(__int64 accountID, MailboxMoveRecord &row);

      // The outcome and one journal row as JSON, which is what COM answers and
      // what the REST routes send, so the two cannot describe a move
      // differently.
      static AnsiString OutcomeJson(const MailboxMoveOutcome &outcome);
      static AnsiString RecordJson(const MailboxMoveRecord &record);
      static AnsiString RecordsJson(const std::vector<MailboxMoveRecord> &records);

      // Writes one application-log line per move that was interrupted, at
      // start-up. A move that was rolled back is not one.
      static void ReportInterrupted();

   private:
      // Everything that writes. Separated from Move so that the whole of it can
      // run inside one AuditScope with no actor - see ONE ENTRY ON THE AUDIT
      // TRAIL above - and so that the decision about what to record is taken
      // once, where the actor is back. leftBehind says a failure could not be
      // undone, which is the one failure that IS an administrative change.
      static bool Move_(std::shared_ptr<Account> account, const String &newAddress, bool keepOldAddressAsAlias,
                        bool refuseWhileInUse, MailboxMoveOutcome &outcome, String &errorMessage);

      // oldAddress is the STORED address, read from the row rather than taken
      // from the object the caller holds. A caller that has set Account.Address
      // and not saved it holds an object whose address no account has, and a
      // move that took it at its word would find no directory at that name,
      // carry nothing, write the new address over the row, and leave every
      // alias, list membership, queued recipient and quarantine entry naming an
      // address that no longer exists - the whole mailbox orphaned by one
      // assignment.
      static bool Perform_(std::shared_ptr<Account> account, const String &oldAddress, const String &newAddress,
                           __int64 newDomainID, __int64 oldDomainID, bool keepOldAddressAsAlias,
                           MailboxMoveOutcome &outcome, String &errorMessage, bool &leftBehind);

      static __int64 OpenJournal_(__int64 accountID, const String &oldAddress, const String &newAddress, bool keepAlias);
      static void CloseJournal_(__int64 journalID, int stage, int messages, const String &detail);
      static void ReadRecord_(std::shared_ptr<DALRecordset> recordset, MailboxMoveRecord &record);

      // carried says whether anything was actually moved: a mailbox nothing has
      // ever been delivered to has no directory, and reporting "the store was
      // moved" for it is reporting that the step ran, not that anything moved.
      // On a failure it says instead that something could not be put back - an
      // object store that stopped answering part way through its objects -
      // which the caller journals as a rollback that failed.
      static bool MoveStore_(const String &oldAddress, const String &newAddress, bool &carried, String &errorMessage);
      static bool MoveDirectory_(const String &from, const String &to, bool &carried, String &errorMessage);

      // Anything delivered into the old mailbox directory after the store was
      // moved and before the account row carried the new address. Merged into
      // the new directory, which is what CopyDirectory does over a tree that
      // already exists, and the old one removed again.
      static void SweepLateArrivals_(const String &oldAddress, const String &newAddress);
      static bool WriteAccountRow_(__int64 accountID, const String &newAddress, __int64 newDomainID);
      // Each returns what it WROTE, and sets incomplete when the statement it
      // makes fails - which writes nothing, so the count alone cannot be told
      // from there having been nothing to write.
      static int RepointAliases_(const String &oldAddress, const String &newAddress, bool &incomplete);
      static int RewriteListMemberships_(const String &oldAddress, const String &newAddress, bool &incomplete);
      static int RewriteQueuedRecipients_(__int64 accountID, const String &oldAddress, const String &newAddress, bool &incomplete);
      static int RewriteQuarantine_(const String &oldAddress, const String &newAddress, bool &incomplete);
      static bool LeaveAlias_(__int64 domainID, const String &oldAddress, const String &newAddress, String &errorMessage);
      static void ForgetCaches_(std::shared_ptr<Account> account, const String &oldAddress,
                                __int64 oldDomainID, __int64 newDomainID);
      static int CountMessages_(__int64 accountID);
      static int CountFolders_(__int64 accountID);
   };
}
