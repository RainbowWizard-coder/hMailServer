// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../AntiSpam/DKIM/DkimRotationPlan.h"

#include <vector>

namespace HM
{
   // hm_dkimrotations (schema 6048): one row per domain whose DKIM key rotation the
   // server manages - the current, next and retiring slots, when each began, the last
   // check of each slot's record, the last selector retired and the last error. The
   // row is created by the first pass that has something to remember and cascades
   // from hm_domains, so a deleted domain takes it along.
   //
   // Every statement here is a plain select, insert, update or delete with its
   // values as parameters: no subquery, no CASE, no LIMIT or TOP, so the same text
   // runs on SQL Server Compact, SQL Server, MySQL and PostgreSQL.
   class PersistentDkimRotation
   {
   public:
      // False when the read failed. exists says whether there was a row; when there
      // was none the state is left as constructed.
      static bool Read(__int64 domainId, DkimRotationPlan::State &state, bool &exists);

      // Writes the row, inserting it when there is none. A unique-index collision on
      // the insert - another thread made the row a moment earlier - is answered by
      // updating the row that now exists.
      static bool Save(__int64 domainId, const DkimRotationPlan::State &state);

      // The domains a pass has to look at: those with the schedule on, and those
      // with a next or retiring selector in the row. Two plain statements merged
      // here, because a UNION of differently-shaped tables is exactly the kind of
      // statement one of the four dialects answers differently.
      static bool ListInFlight(std::vector<__int64> &domainIds);

      // Every row's retiring key file, for the deletion guard.
      static bool ListRetiringKeyFiles(std::vector<std::pair<__int64, String> > &files);

      // Every row, for the stalled-rotation alert.
      static bool ListAll(std::vector<std::pair<__int64, DkimRotationPlan::State> > &rows);

   private:
      friend class PersistentDkimRotationTester;

      // Save once the lookup has found no row: the insert, and when the unique
      // index refuses it - another node made the row in between - the update of
      // the row that now exists.
      static bool InsertOrUpdate_(__int64 domainId, const DkimRotationPlan::State &state);
   };

   // Save's race with another node, on the server's own database and a domain of
   // the tester's own: the lookup has missed and the row is there by the insert,
   // which must then update it - and an insert with nothing there writes the row.
   // ClassTester runs it.
   class PersistentDkimRotationTester
   {
   public:
      void Test();
   };
}
