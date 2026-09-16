// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The two tables behind complaints read, the feedback loops (schema 6050):
// hm_complaints, one row per abuse report this server believed; and
// hm_complaintvolume, the per-domain daily tally of messages delivered and
// complaints received that the complaint rate is worked out from.
//
// The third table the feature writes is not here: a complaint is counted in
// hm_bouncecounts.bccomplaints and lists an address in hm_bouncesuppressions,
// which are the suppression list's own tables, written through PersistentBounce
// under the one lock that owns an address's row. A complaint is not a second
// suppression list.
//
// Written for four databases, which decides every statement's shape, and the
// rules are PersistentBounce's: a single-table select, insert, update or delete by
// key, every value from outside a parameter, no scalar subquery compared with =
// and no CASE WHEN EXISTS (both of which SQL Server Compact refuses or crashes
// on), a row-limited select that is TOP on SQL Server and Compact and LIMIT on
// MySQL and PostgreSQL, a delete that must say what it removed counting first,
// and a counter added to in the database's own arithmetic so two nodes counting
// at once cannot overwrite each other. Addresses are stored lower-cased and dates
// as YYYY-MM-DD HH:MM:SS strings, which compare correctly as strings everywhere.

#pragma once

namespace HM
{
   struct ComplaintRow
   {
      __int64 id = 0;
      __int64 domain_id = 0;
      __int64 account_id = 0;    // 0: the sender is not an account of this server
      String key;                // what makes the same report twice one complaint
      String address;            // the reader who complained; empty when redacted
      String sender;             // Original-Mail-From, a sender of this installation
      String feedback_type;      // abuse, fraud, virus, not-spam, other
      String provider;           // the report's User-Agent
      String source_ip;
      String reported_domain;
      String message_id;
      String subject;
      String arrival;            // when the provider says the message arrived
      String received;           // when this server read the report
   };

   // One domain's totals over a window of days.
   struct ComplaintVolumeRow
   {
      __int64 domain_id = 0;
      __int64 sent = 0;
      __int64 complaints = 0;
   };

   class PersistentComplaint
   {
   public:
      // YYYY-MM-DD, local time, days from now (negative for the past).
      static String Day(int daysFromNow);

      // -------------------------------------------------------- the complaints

      // 1 recorded, 0 the same report has already been recorded (the unique index
      // on the key decided it, here or on another node), -1 the database failed.
      static int Insert(const ComplaintRow &row);

      // Newest first. search is a substring of the complained-of address, the
      // sender or the subject; limit 1 to 1000. accountID 0 means every account.
      static bool List(__int64 domainID, __int64 accountID, const String &search, int limit, int offset,
                       std::vector<ComplaintRow> &rows, int &total);

      // -------------------------------------------------------- the tally

      // Adds to one domain's row for one day, inserting it when there is none.
      // sent and complaints are numbers this server computed, never text from
      // outside, and are written into the statement as literals because SQL Server
      // Compact cannot type a parameter used in arithmetic.
      static bool AddToVolume(__int64 domainID, const String &day, __int64 sent, int complaints);

      // Every domain's totals over the days from `since` (a Day value, inclusive).
      static bool ReadVolume(const String &since, std::vector<ComplaintVolumeRow> &rows);

      // -------------------------------------------------------- maintenance

      // Complaints older than retentionDays, and tally rows older than
      // tallyRetentionDays. 0 for either keeps that kind for ever. Returns the rows
      // removed, or -1.
      //
      // Two numbers and not one because the tally is the denominator of the rate,
      // and the rate is measured over a fixed window: an operator who sets the
      // retention below that window would otherwise be deleting the days the rate
      // is worked out from, and the rate would quietly stop being the number the
      // providers are looking at. The caller passes the larger of the retention and
      // the window (ComplaintTracker::RunMaintenance), which is where the window's
      // length is defined.
      static int Prune(int retentionDays, int tallyRetentionDays);

      // Every complaint naming this address, in every domain - as the complained-of
      // reader or as the sender - for an erasure.
      static int EraseAddress(const String &address);
   };
}
