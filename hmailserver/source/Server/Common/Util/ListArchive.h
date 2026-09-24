// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A managed list's archive, and the digest built from it (ListManager.h).
//
// Every message a managed list distributes is copied here when its archive is
// on or its digest is: the list's own copy - the subject prefix, Reply-To and
// the footer applied, no member's headers - as a file under the data
// directory's Lists\<list id>\ folder, and a row in hm_listarchive holding what
// the pages list by (the Message-ID, the subject, the sender, when, the month,
// the size) and whether the archive kept it or only the digest did.
//
// The archive is read by the list's page (/lists/{address}/archive, public or
// for members) and by GET /api/v1/lists/{address}/archive. It is not reachable
// over IMAP or NNTP: a list's archive is the list's, not a mailbox anybody
// signs in to.
//
// The DIGEST reads the same rows. Once a day at the list's hour, every member
// whose delivery is the digest is sent one message: a table of contents and
// then each message since the last digest as a message/rfc822 part of a
// multipart/digest (RFC 2046 5.1.5, in the shape RFC 1153 set out for text).
// A list with the digest on and the archive off still keeps the copies - the
// digest has nothing else to be built from - marked as the digest's alone, and
// the sweep deletes each one as soon as a digest has carried it, or all of them
// once the digest is off; the pages never show them.
//
// RETENTION. A copy the archive kept is deleted once it is older than the
// list's archive days (0 keeps everything), but never one the digest has not
// sent yet - whether or not the archive is still on: switching the archive off
// takes its pages away and keeps what they held until its days run out, and
// switching it on again brings it back. Deleting the list deletes its folder
// and its rows.

#pragma once

#include <vector>

namespace HM
{
   class DistributionList;
   class MessageData;

   struct ListArchiveEntry
   {
      __int64 id = 0;
      __int64 list_id = 0;
      String message_id;
      String subject;
      String sender;
      __int64 sent = 0;
      int size = 0;
      String file;
   };

   class ListArchive
   {
   public:
      // <data directory>\Lists\<list id>
      static String Directory(__int64 listId);

      // Copies the list's copy of a posting into the archive. False, reported,
      // when it could not be: the posting is still distributed.
      static bool Store(std::shared_ptr<const DistributionList> list, const String &copyFile, std::shared_ptr<MessageData> data);

      // The months that hold messages the archive kept, "YYYY-MM" in UTC,
      // newest first; read from the month column, never from every row.
      static std::vector<String> Months(__int64 listId);

      // A month's messages the archive kept, oldest first, at most `limit` of
      // them; month empty for the newest `limit`. The database is asked for no
      // more rows than that.
      static std::vector<ListArchiveEntry> List(__int64 listId, const String &month, int limit);

      static bool Get(__int64 listId, __int64 id, ListArchiveEntry &entry);

      // The file of an entry, under the list's folder.
      static String PathOf(const ListArchiveEntry &entry);

      // The list's rows and its folder. Called by PersistentDistributionList::DeleteObject.
      static void DeleteAll(__int64 listId);

      // Retention for every managed list, and the requests nobody confirmed
      // within ListManager::RequestLifetimeDays. Returns the rows removed.
      static int Sweep();

      // One list's digest, to every member whose delivery is the digest.
      // force ignores the hour and the once-a-day rule, which is what an
      // administrator's "send it now" and a test need; it does not send an
      // empty digest. Returns the digests sent.
      static int SendDigest(std::shared_ptr<const DistributionList> list, bool force);

      // Every managed list whose digest is due in this hour. Returns the
      // digests sent.
      static int RunDigests();

      // "YYYY-MM" for a Unix time, in UTC.
      static String MonthOf(__int64 unixTime);

      // The Unix time a "YYYY-MM" month starts, in UTC, or -1.
      static __int64 MonthStart(const String &month);

   private:
      static bool DeleteEntries_(const String &where, __int64 listId, __int64 bound);
   };

   // Run from ClassTester::DoTests: the month arithmetic, both ways.
   class ListArchiveTester
   {
   public:
      void Test();
   };
}
