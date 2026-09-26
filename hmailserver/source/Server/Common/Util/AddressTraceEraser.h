// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   /*
      Removes the traces of an email address that ordinary account deletion
      deliberately leaves behind.

      PersistentAccount::DeleteObject is thorough about what the ACCOUNT owns -
      messages, rules, app passwords, the mailbox directory, the Sieve tree -
      and blind, on purpose, to the places where the ADDRESS appears as data in
      somebody else's records: archive copies, quarantine rows, message-trace
      rows, the send API's record of sends (hm_apisends), greylisting triplets,
      distribution-list memberships and aliases, messages held for review
      (hm_heldmessages), messages that expire (hm_confidential) and
      password-protected ones (hm_protectedmessages), the recipients another
      account remembers writing to (hm_accountrecipients), the out-of-office
      memory (hm_autoreplies), and the correspondents' S/MIME certificates and
      OpenPGP keys other accounts keep (hm_smimekeys and hm_pgpring, kind 2).
      Deleting those as a side effect of removing an account would be wrong
      twice over - an archive can be under a legal hold that outranks the
      deletion, and a list membership is the list owner's record as much as the
      member's.

      So erasure is its own operation, invoked deliberately (COM:
      Utilities.EraseAddressTraces), for the case where an operator has been
      asked to remove a person rather than close a mailbox. It works on an
      ADDRESS, not an account, because the request usually arrives after the
      account is already gone.

      What it deliberately does NOT touch, and why:

      * Protocol logs and the AWStats journal, unless asked (includeLogs): by
        default addresses inside log lines are bounded by log retention rather
        than edited, because a log that has been selectively rewritten is a
        weaker record. Asked, every occurrence becomes [erased], so the line
        itself says it was changed.
      * The archive's Inbound folder. Messages from non-local senders land
        there under the message's own filename, attributed to nobody; finding
        one address's messages in it would mean parsing every message in the
        store. The per-user archive tree is the half that is keyed by address,
        and it is the half that is erased.
      * Backups. A backup archive is outside the running system's reach by
        design.
      * What a legal hold keeps (DiscoveryHolds.h). An erasure never deletes a
        message from a mailbox, so a hold meets it in the message stores it
        does delete from: an archived copy a hold covers keeps its file and its
        index row, and a message held for review, a message that expires or a
        password-protected one whose account is held is left as it is. Each
        is counted in heldCount and said in the application log, so that the
        operator can tell the person what the hold kept and why.
      * eDiscovery's own record (hm_discovery*, and the results and exports
        under Discovery\case-<id>): a case's queries, its custodians and its
        holds are the legal record the hold exists for, and a case is never
        deleted (docs/Discovery.md).
      * Configuration that names an address - an account's forward, a rule's
        action, a list's moderator, the sender lists, a journal rule, an alert
        recipient - and the audit trail, whose rows are hash-chained and age
        out by audit_retention_days. docs/PersonalData.md lists them.
   */
   class AddressTraceEraser
   {
   public:

      // Erases every reachable trace of the address. Returns false only when a
      // step FAILED - "nothing found" is success with removedCount 0. The count
      // is rows plus quarantine entries plus one per removed archive tree,
      // because the caller's honest report is "how many things were removed",
      // not which tables they were rows in.
      static bool Erase(const String &address, bool includeArchive, int &removedCount);

      // The same, and with includeLogs the server's own log files rewritten so
      // that no line names the address (every occurrence becomes [erased]);
      // the occurrences count towards removedCount. A log that has been
      // rewritten says so in its own lines, which is the honest form of a
      // record that had to change: the alternative, retaining a person's
      // address for as long as the retention window runs, is what article 17
      // does not allow once the request is made.
      static bool Erase(const String &address, bool includeArchive, bool includeLogs, int &removedCount);

      // The same, and heldCount the things a legal hold kept in place: the
      // archived copies (a file and its index row are one copy) and the
      // messages held for review, that expire or are password-protected whose
      // account is held. Not a failure: the erasure did what it may. Unless the
      // holds could not be read (DiscoveryHolds::ReadFailed, before the first
      // store a hold can keep or after the last): every question is then
      // answered "held" and what was left was left for want of an answer, so
      // the erasure fails naming the legal holds as a store to retry, and says
      // nothing of a legal hold.
      static bool Erase(const String &address, bool includeArchive, bool includeLogs, int &removedCount, int &heldCount);

   private:

      // Counts, then deletes, the rows matching one or two address columns,
      // compared lower-cased on both sides whatever case the row was written in,
      // and - when condition is given - that condition too (a literal with no
      // parameter, such as "smimekind = 2"). Returns -1 on failure. Two
      // statements rather than one because the DAL reports success, not
      // affected rows, and an erasure whose report says "done" without saying
      // how much is indistinguishable from one that did nothing.
      static int DeleteAddressRows_(const AnsiString &table, const AnsiString &firstColumn, const AnsiString &secondColumn, const String &address,
                                    const AnsiString &condition = AnsiString());

      static int EraseQuarantine_(const String &address, bool &failed);
      static int EraseArchive_(const String &address, bool &failed, int &held);
   };
}
