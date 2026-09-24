// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// eDiscovery's four tables (schema 6097, docs/Discovery.md): the cases, the
// searches run for them, the holds they placed and the exports they made.
//
// THE AUDIT TRAIL. The four are audited tables (AuditTrail.cpp), so a request's
// insert or update is recorded by the statement chokepoint under the request's
// actor, and the runner's own writes on the maintenance queue - a search's
// progress, an export's result - carry no actor and are not. A row of these
// tables is mostly ids and numbers, so the writes here supply the words: every
// row is named "case <id>: <name>" (the case's own create is named by its name
// column), and a search's row is recorded with its whole query, which the route
// keeps under the trail's 2,000 characters for that reason.
//
// TIMES are seconds since 1970, UTC, and 0 for "not yet".
#pragma once

#include <vector>

namespace HM
{
   class DALRecordset;

   class DiscoveryStore
   {
   public:
      enum CaseState
      {
         CaseOpen = 0,
         CaseClosed = 1
      };

      // A search's and an export's state alike.
      enum JobState
      {
         JobQueued = 0,
         JobRunning = 1,
         JobDone = 2,
         JobFailed = 3
      };

      struct Case
      {
         __int64 id = 0;
         String name;
         String description;
         int state = CaseOpen;
         String created_by;
         __int64 created = 0;
         String closed_by;
         __int64 closed = 0;
      };

      struct Search
      {
         __int64 id = 0;
         __int64 case_id = 0;
         AnsiString query;       // the JSON the route stored, UTF-8
         String ran_by;
         __int64 ran = 0;        // when it was asked for
         __int64 finished = 0;
         int state = JobQueued;
         int scanned = 0;        // messages read so far
         int matched = 0;        // hits written
         int unsearched = 0;     // messages listed as not searched
         bool truncated = false; // the hits stopped at the ceiling
         String error;
         String result_file;     // the file's name under the case's directory
      };

      struct Hold
      {
         __int64 id = 0;
         __int64 case_id = 0;
         int scope = 0;          // DiscoveryHolds::Scope
         __int64 owner_id = 0;
         String name;            // the address, the domain or the archived copy
         String placed_by;
         __int64 placed = 0;
         String released_by;
         __int64 released = 0;

         bool Live() const { return released == 0; }
      };

      struct Export
      {
         __int64 id = 0;
         __int64 case_id = 0;
         __int64 search_id = 0;
         int state = JobQueued;
         String file;            // the zip's name under the case's directory
         __int64 size = 0;
         String sha256;          // of the zip, lower-case hex
         int items = 0;          // messages written
         int withheld = 0;       // hits listed and not written
         String requested_by;
         __int64 requested = 0;
         __int64 finished = 0;
         String error;
      };

      // Cases.
      static bool ListCases(std::vector<Case> &cases);
      static bool GetCase(__int64 id, Case &found);
      static __int64 CreateCase(const String &name, const String &description, const String &createdBy);

      // Closes the case and releases every hold of it still live, each
      // recorded; the number released in released. False when the case's row
      // could not be written.
      static bool CloseCase(const Case &theCase, const String &closedBy, int &released);

      // Searches.
      static bool ListSearches(__int64 caseId, std::vector<Search> &searches);
      static bool GetSearch(__int64 id, Search &found);
      static __int64 CreateSearch(const Case &theCase, const AnsiString &queryJson, const String &ranBy);
      static bool MarkSearchRunning(__int64 id);
      static bool RecordSearchProgress(__int64 id, int scanned, int matched, int unsearched);
      static bool FinishSearch(const Search &search);

      // Holds.
      static bool ListHolds(__int64 caseId, std::vector<Hold> &holds);
      static bool GetHold(__int64 id, Hold &found);
      static __int64 PlaceHold(const Case &theCase, int scope, __int64 ownerId, const String &name, const String &placedBy);
      static bool ReleaseHold(const Case &theCase, const Hold &hold, const String &releasedBy);

      // Exports.
      static bool ListExports(__int64 caseId, std::vector<Export> &exports);
      static bool GetExport(__int64 id, Export &found);
      static __int64 CreateExport(const Case &theCase, const Search &search, const String &requestedBy);
      static bool MarkExportRunning(__int64 id);
      static bool FinishExport(const Export &result);

      // The runner's queue: the oldest search or export still queued, searches
      // and exports alike in the order they were asked for. False when there is
      // none, or when the tables could not be read.
      static bool OldestQueued(bool &isSearch, __int64 &id);

      // A search or an export nobody is running any more - the server stopped,
      // or the maintenance queue was taken away, while it ran - is failed with
      // the sentence rather than left "running" for ever. Called by the runner
      // each time it starts, which is only when no other runner is at work in
      // this process.
      static void FailInterrupted();

      // "case <id>: <name>", which every row but the case's own create is
      // recorded under on the audit trail.
      static String CaseLabel(const Case &theCase);

      // <data directory>\Discovery\case-<id>: the searches' results and the
      // exports.
      static String CaseDirectory(__int64 caseId);

   private:
      static void ReadCase_(std::shared_ptr<DALRecordset> recordset, Case &theCase);
      static void ReadSearch_(std::shared_ptr<DALRecordset> recordset, Search &search);
      static void ReadHold_(std::shared_ptr<DALRecordset> recordset, Hold &hold);
      static void ReadExport_(std::shared_ptr<DALRecordset> recordset, Export &result);
   };
}
