// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Collection.h"

#include "GreyListingBypassEntry.h"
#include "../Persistence/PersistentGreyListingBypassEntry.h"

namespace HM
{
   // The greylisting bypass list, in the white list's shape: a collection over
   // hm_greylisting_bypass that the COM object, the REST resource and the backup
   // read whole. The SMTP session reads GreyListingBypass's cached copy of it,
   // which the persister marks stale on every save and delete.
   class GreyListingBypassEntries : public Collection<GreyListingBypassEntry, PersistentGreyListingBypassEntry>
   {
   public:
      GreyListingBypassEntries();
      ~GreyListingBypassEntries(void);

      // Refreshes this collection from the database. The bool matters: the
      // cache in GreyListingBypass tells "the list is empty" from "the database
      // could not be read" by it.
      bool Refresh();

   protected:
      virtual String GetCollectionName() const {return "GreyListingBypassEntries"; }
   };
}
