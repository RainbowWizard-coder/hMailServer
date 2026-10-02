// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PersistenceMode.h"

namespace HM
{
   class NeverBanName;

   // hm_neverbannames (schema 6108). Every save and delete tells
   // NeverBanNameList that the rows changed, which re-reads them for the next
   // ban and forgets every verdict reached under the old rows: a pattern taken
   // off the list because it was a mistake spares nobody from that moment.
   //
   // A row is recorded in the audit trail as a "never-ban name" under its
   // pattern, on the delete as well as the save, since the delete's statement
   // carries only the row's id.
   class PersistentNeverBanName
   {
   public:
      PersistentNeverBanName();
      ~PersistentNeverBanName();

      static bool DeleteObject(std::shared_ptr<NeverBanName> pObject);
      static bool SaveObject(std::shared_ptr<NeverBanName> pObject, String &errorMessage, PersistenceMode mode);
      static bool SaveObject(std::shared_ptr<NeverBanName> pObject);
      static bool ReadObject(std::shared_ptr<NeverBanName> pObject, std::shared_ptr<DALRecordset> pRS);

      // The row with this pattern, or 0. The pattern is unique in the table, so
      // a second save of the same pattern is refused in words before the
      // database refuses it with a constraint error nobody can read.
      static __int64 Find(const String &pattern);
   };
}
