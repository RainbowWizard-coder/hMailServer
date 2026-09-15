// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The three tables behind bounces read and the suppression list (schema 6051):
// hm_bouncesuppressions, the list itself; hm_bouncecounts, what it is fed from;
// and hm_bouncereportkeys, the register a delivery report is matched against.
//
// Written for four databases, which decides every statement's shape. Each is a
// single-table select, insert, update or delete by key, every value from outside
// is a parameter, there is no scalar subquery compared with = and no CASE WHEN
// EXISTS (both of which SQL Server Compact refuses or crashes on), a row-limited
// select is TOP on SQL Server and Compact and LIMIT on MySQL and PostgreSQL, and
// a delete that must say what it removed counts first and deletes second.
// Addresses are stored lower-cased, so PostgreSQL's case-sensitive comparison
// needs no lower(), and dates are YYYY-MM-DD HH:MM:SS strings, which compare
// correctly as strings everywhere.

#pragma once

namespace HM
{
   struct BounceSuppressionEntry
   {
      __int64 id = 0;
      __int64 domain_id = 0;
      String address;
      int source = 0;
      int category = 0;
      String status;
      String diagnostic;
      String remote_mta;
      String created;
      String expires;         // empty: no expiry of its own
   };

   struct BounceCountRow
   {
      __int64 id = 0;
      __int64 domain_id = 0;
      String address;
      int hard = 0;
      int soft = 0;
      int mailbox_full = 0;
      int policy = 0;
      int other = 0;
      int consecutive_hard = 0;
      String last_status;
      int last_category = 0;
      String last_bounce;
      String last_delivered;
   };

   class PersistentBounce
   {
   public:
      // hm_bouncesuppressions.bssource.
      enum Source
      {
         SourceBounce = 0,     // a refusal while this server delivered
         SourceReport = 1,     // an RFC 3464 report that matched the register
         SourceManual = 2,     // an administrator
         SourceComplaint = 3   // reserved for the complaints row
      };

      static const char *SourceName(int source);
      static int SourceFromName(const String &name);

      // Trimmed, lower-cased, at most 255 characters.
      static String Canonical(const String &address);

      // YYYY-MM-DD HH:MM:SS, local time, days from now (negative for the past).
      static String DateTimeFromNow(int days);

      // -------------------------------------------------------- the list

      // 1 found, 0 not listed, -1 the query failed.
      static int FindSuppression(__int64 domainID, const String &address, BounceSuppressionEntry &entry);

      // The unique index decides a race between two deliveries listing the same
      // address at once; the loser is not an error, so none is written.
      static bool InsertSuppression(const BounceSuppressionEntry &entry);

      // Rows removed, or -1.
      static int DeleteSuppression(__int64 domainID, const String &address);

      // Newest first. search is a substring of the address; limit 1 to 1000.
      static bool ListSuppressions(__int64 domainID, const String &search, int limit, int offset,
                                   std::vector<BounceSuppressionEntry> &rows, int &total);

      // Whether an entry has stopped applying: its own expiry has passed, or it
      // came from a bounce or a report and is older than expiryDays (0 = never).
      static bool IsExpired(const BounceSuppressionEntry &entry, int expiryDays, const String &now);

      // -------------------------------------------------------- the counts

      static int FindCount(__int64 domainID, const String &address, BounceCountRow &row);
      static bool InsertCount(const BounceCountRow &row);
      static bool UpdateCount(const BounceCountRow &row);

      // A delivery: the run of hard bounces is over. Touches no row for an address
      // that has never bounced.
      static bool RecordDelivered(__int64 domainID, const String &address);

      // An administrator removed the entry by hand: the run starts again.
      static bool ResetConsecutiveHard(__int64 domainID, const String &address);

      static bool ListCounts(__int64 domainID, const String &search, int limit, int offset,
                             std::vector<BounceCountRow> &rows, int &total);

      // -------------------------------------------------------- the register

      static bool InsertReportKey(__int64 domainID, const String &key);

      // Finds one row with this key and deletes it, so the same report cannot be
      // counted twice. Returns 1 and the row's domain when one was consumed, 0
      // when there was none, -1 when the database failed.
      static int ConsumeReportKey(const String &key, __int64 &domainID);

      // -------------------------------------------------------- maintenance

      // Each returns the rows removed, or -1.
      static int ExpireSuppressions(int expiryDays);
      static int PruneCounts(int retentionDays);
      static int PruneReportKeys(int windowDays);

      // The counts and entries of one address in every domain, for an erasure.
      static int EraseAddress(const String &address);
   };
}
