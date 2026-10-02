// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "IPrerequisite.h"

namespace HM
{
   // Schema 6107's prerequisite: the end of the greylisting key migration that
   // schema 6072 began (work item #50, the follow-up to GitHub discussion #281).
   //
   // 6072 added a key to every triplet - GreyListTriplet::ComputeKey of the
   // sender and of the recipient, in glsenderhash and glrecipienthash - and left
   // the rows written before it with NULL there, to be found by their address
   // columns and keyed one at a time as they were matched. 6107 makes both
   // columns NOT NULL and the key unique, and the lookup asks for the key and
   // nothing else; before its script can do that, every row has to have its key
   // and no triplet may have two rows. That is this, and it is done here, in the
   // server, because no backend can compute the key: it is the MD5 of the
   // address with its ends trimmed of ASCII whitespace and its domain lower-cased
   // by hand, as UTF-8, written as 32 lower-case hex characters, and an MD5 a
   // database computes over its own nvarchar would not match a single lookup.
   //
   // For every row with either key NULL:
   //   - an address of 200 characters or more is the old columns' cut (AddObject
   //     wrote every address cut to the 200 they held), so two different
   //     addresses may already be one row: the row is deleted, since a key over
   //     the stub would freeze that collision and match no address ever sent;
   //   - a row whose key cannot be computed (an OpenSSL that answers no MD5,
   //     which a stock one never does) is deleted too, because the step's NOT
   //     NULL would otherwise stop the upgrade;
   //   - every other row is given the key new rows are written with, computed
   //     from the address it holds.
   // A row that already has both keys is left alone whatever its addresses'
   // length: since 6072 the key is over the whole of the address however long,
   // and a 200-character address is an address like another.
   // Then every triplet that has more than one row - a race before the key was
   // unique, or a row keyed here whose triplet another row already keyed - keeps
   // one: the one that has passed most often, the oldest of those. A confirmed
   // triplet is never thrown away for a pending copy of itself.
   //
   // It runs twice for an upgrade. The walkers - SchemaUpgrader, and DBUpdater
   // through Database.EnsurePrerequisites - run it first outside the step's
   // transaction, where every statement commits as it goes: on a table of
   // millions of rows an upgrade stopped part-way keeps what it did, and the next
   // run finds only the rows still without their key (the work is chosen by the
   // NULLs, so there is no cursor to lose). Then again inside the transaction,
   // just before the script, where it finds what is left - nothing, unless a
   // server still writing to the database added a row meanwhile. On a database
   // older than 6072 there are no key columns yet: it does nothing, and the run
   // inside the step's transaction does the work.
   class PreReqGreyListKeys : public IPrerequisite
   {
   public:
      // The schema the key columns came with.
      static const int KeyColumnsSchema = 6072;

      // The width of the address columns before 6072, and the length every
      // address was cut to when it was written.
      static const int LegacyAddressWidth = 200;

      int GetDatabaseVersion() override { return 6107; }
      bool RunsBeforeTransaction() const override { return true; }
      bool Ensure(std::shared_ptr<DALConnection> connection, const PrerequisiteReport &report, String &sErrorMessage) override;

      struct Counts
      {
         Counts() : keyed(0), truncated(0), unkeyable(0), duplicates(0), skipped(false), complete(false) {}

         __int64 keyed;
         __int64 truncated;
         __int64 unkeyable;
         __int64 duplicates;

         // The database is older than KeyColumnsSchema: nothing was looked at.
         bool skipped;
         // The pass went to the end: every row had its key and every triplet one
         // row when it finished.
         bool complete;
      };

      // One pass, with its sizes given: the rows read at a time (a range of glid,
      // at most batchRows of them), the rows one UPDATE or DELETE names, and the
      // number of batches after which to stop - as an upgrade stopped part-way
      // stops - where maxBatches is not negative. Ensure runs it with no limit.
      static bool Run(std::shared_ptr<DALConnection> connection, int batchRows, int statementRows, int maxBatches, Counts &counts, String &error);

      // What becomes of a row that has no key, given its two address columns;
      // the two keys when it is to be keyed, empty otherwise.
      enum Fate
      {
         Key = 1,
         DeleteTruncated = 2,
         DeleteUnkeyable = 3
      };
      static Fate Classify(const String &sender, const String &recipient, String &senderKey, String &recipientKey);

      // The rows of one triplet: which to keep, as an index into copies - the one
      // with the highest passed count, and of those the lowest id.
      struct Copy
      {
         __int64 id = 0;
         int passed = 0;
      };
      static size_t ChooseSurvivor(const std::vector<Copy> &copies);

      // Whether a value is a key as ComputeKey writes one: 32 characters, each
      // 0-9 or a-f.
      static bool IsKey(const String &value);

      // The one UPDATE that writes the keys of several rows, each value a literal
      // in a CASE on glid: the keys are hex and the ids numbers, so nothing a
      // sender wrote reaches the statement. Empty when a key is not IsKey, which
      // then writes nothing.
      struct KeyedRow
      {
         __int64 id = 0;
         String senderKey;
         String recipientKey;
      };
      static String KeyStatement(const std::vector<KeyedRow> &rows);

      // The line the pass reports.
      static String Summary(const Counts &counts);
   };

   // Holds the cutover to examples in ClassTester::DoTests (and in the POSIX
   // build's greylist-keys-tests program): the key against MD5 vectors, the fate
   // of a row, the survivor, the statement and its refusal. On Windows also the
   // pass itself and the 6107 script, on a SQL Server Compact database of the
   // tester's own: see the definition.
   class PreReqGreyListKeysTester
   {
   public:
      void Test();
   };
}
