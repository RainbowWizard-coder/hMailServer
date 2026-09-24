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
      distribution-list memberships and aliases.
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

   private:

      // Counts, then deletes, the rows matching one or two address columns.
      // Returns -1 on failure. Two statements rather than one because the DAL
      // reports success, not affected rows, and an erasure whose report says
      // "done" without saying how much is indistinguishable from one that did
      // nothing.
      static int DeleteAddressRows_(const AnsiString &table, const AnsiString &firstColumn, const AnsiString &secondColumn, const String &address);

      static int EraseQuarantine_(const String &address, bool &failed);
      static int EraseArchive_(const String &address, bool &failed);
   };
}
