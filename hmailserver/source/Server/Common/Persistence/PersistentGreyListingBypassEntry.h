// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PersistenceMode.h"

namespace HM
{
   class GreyListingBypassEntry;

   class PersistentGreyListingBypassEntry
   {
   public:
      PersistentGreyListingBypassEntry(void);
      ~PersistentGreyListingBypassEntry(void);

      static bool DeleteObject(std::shared_ptr<GreyListingBypassEntry> pObject);

      // Refuses, with the sentence in errorMessage, a row that could never match
      // or never be satisfied: an empty pattern, a pattern that is not a domain
      // name or a dotted suffix, a match word outside the three, and a row that
      // requires no result at all. The COM item and the REST route both come here,
      // so they refuse with one sentence.
      static bool SaveObject(std::shared_ptr<GreyListingBypassEntry> pObject, String &errorMessage, PersistenceMode mode);
      static bool SaveObject(std::shared_ptr<GreyListingBypassEntry> pObject);
      static bool ReadObject(std::shared_ptr<GreyListingBypassEntry> pObject, std::shared_ptr<DALRecordset> pRS);
   };
}
