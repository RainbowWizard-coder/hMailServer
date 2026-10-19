// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The parts of messages held once: blobs, and how many names need each.
//
// A blob is the bytes of one large MIME part, as they were on the wire, at
// <data>\Blobs\<first two hex digits>\<sha256>, and a row of hm_storeblobs
// (schema 6098) holds its size and how many stored files reference it. A count
// is of NAMES: a container placed in a folder adds one for each part it
// references, and so does every other name for it - a copy made through the
// store (IMAP COPY, the recover-deleted store's kept copy, a mailbox restore),
// a hard link DeliveryHardLinks makes. Removing a name lets go of one, and at
// none the row and the blob go.
//
// The rule every change keeps: never count fewer than there are. A count too
// high leaves a blob nothing needs, which the consistency check reports and a
// recount corrects; a count too low deletes a part a message still needs. So a
// count is added before the name that needs it is written, and let go of after
// the name is gone; only a name the store counted is let go of (a queue file
// and a working file never placed count nothing, MessageStore.h); and the only
// count ever LOWERED by a recount is lowered by the command line's, with the
// service stopped.
//
// A restore deletes every domain, or empties a mailbox, before it puts the
// backup's messages back, which would let a count go to none while the part is
// about to be needed again: while a DeletionHold is alive no blob is deleted -
// a count may reach none, and the blob and its row are kept - and when the last
// hold ends, with the hold still in force, a full restore's counts are rebuilt
// from the stored files (raised, never lowered: a name counted before the walk
// read it, and one counted while it ran, keep their counts), and only then is
// every blob whose count reached none during the hold and is none still
// deleted.
//
// Serialised within the process by one lock. The command-line walk that
// rewrites a store or recounts it refuses to run beside the server - on
// Windows by asking the service controller, on Linux by a lock the server
// holds for as long as it runs; see MessageStoreMaintenance.

#pragma once

#include "MessageStoreFormat.h"

namespace HM
{
   class MessageStoreBlobs
   {
   public:

      // <data>\Blobs, and one blob's file in it.
      static String Root();
      static String PathOf(const AnsiString &sha256);

      // The parts of a new stored file, cut from its original bytes: each blob
      // written when it is not there yet, or checked against its hash when it
      // is - and written again from these bytes, which hash to its name, when
      // what is there is damaged - then counted. On a failure nothing is
      // counted and the reason is given.
      static bool Keep(const std::string &original, const std::vector<MessageStoreReference> &references, String &problem);

      // Another name for a stored file that references these: one more each.
      static bool AddReferences(const std::vector<MessageStoreReference> &references, String &problem);

      // A name gone: one less each, and the blob and its row gone at none -
      // unless a restore holds them.
      static void Release(const std::vector<MessageStoreReference> &references);

      // A blob's bytes, checked against the length and the hash its reference
      // names.
      static bool Read(const MessageStoreReference &reference, std::string &bytes, String &problem);

      static bool Exists(const AnsiString &sha256);

      // A blob brought in from another copy of a store (a backup being restored
      // one mailbox at a time), when this store does not have it. The hash is
      // checked; nothing is counted - the copy of the file that needs it counts.
      static bool Adopt(const String &file, const AnsiString &sha256, unsigned __int64 length, String &problem);

      // While one of these lives, a count that reaches none deletes nothing.
      // When the last one ends - the hold still in force until it has - the
      // counts are rebuilt from the stored files if any hold asked for that
      // (a full restore, whose files arrive uncounted), and then every blob
      // whose count reached none while it lived, and is none still, is
      // deleted as it would have been. A mailbox restore, whose copies are all
      // counted as they are made, asks for no recount.
      class DeletionHold
      {
      public:
         explicit DeletionHold(bool recountWhenDone);
         ~DeletionHold();

         DeletionHold(const DeletionHold &) = delete;
         DeletionHold &operator=(const DeletionHold &) = delete;
      };

      static bool IsHeld();

      // The counts set from what the stored files reference (MessageStoreMaintenance
      // walks them). exact - the command line's, with the service stopped - sets
      // each count to what the walk found, and removes a row nothing references
      // (its file is left for the consistency check to report). Otherwise - in
      // the service, where names are counted while the walk reads - a count is
      // only ever raised: to what the walk found plus every count added since
      // the first hold began, when that is more than the row holds; no row is
      // removed.
      static bool SetCounts(const std::map<AnsiString, std::pair<unsigned __int64, __int64> > &counts, bool exact, String &summary);

      // Whether hm_storeblobs has a row at all.
      static bool Any();

      struct Totals
      {
         Totals() : blobs(0), bytes(0), references(0) {}
         __int64 blobs;
         __int64 bytes;
         __int64 references;
      };

      static bool GetTotals(Totals &totals);

      // Every blob the table names, with its count.
      static bool List(std::map<AnsiString, __int64> &counts);

   private:

      static bool AddOne_(const AnsiString &sha256, unsigned __int64 length, String &problem);
      static void ReleaseOne_(const AnsiString &sha256);

      // A blob whose count is none, row and file gone - the file only once the
      // row is, so that a count raised in between keeps it.
      static void DeleteIfNone_(const AnsiString &sha256);
   };

   // A stop of the servers reaching the end of a restore's hold (WorkQueue::Stop):
   // every free connection of the server's own database pool held, as statements
   // in flight hold them, a thread of the test's own interrupted as WorkQueue::Stop
   // interrupts a worker, and a hold that asks for the recount ended on it, so that
   // the recount has to wait for a connection with the interruption's request
   // pending. The end of the hold is to hold the interruption off - waiting out
   // the pool, which is handed back after half a second, and rebuilding the counts
   // - and return with the request still pending, where until 20 October 2026 the
   // recount's catch (...) took it at that wait and reported it as HM7352. An end
   // that returned while the pool was still held, the request pending, never
   // waited - it was handed a connection another thread had out and gave back -
   // and the round is made again, for thirty seconds at most. The pool is handed
   // back on every path. Run inside the server on Windows by ClassTester. Throws
   // std::logic_error naming what did not hold.
   class DeletionHoldStopInterruptionTester
   {
   public:
      void Test();
   };
}
