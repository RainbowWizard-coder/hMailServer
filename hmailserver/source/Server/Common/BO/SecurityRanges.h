// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include "Collection.h"

#include "../Persistence/PersistentSecurityRange.h"
#include "../BO/SecurityRange.h"

namespace HM
{
   class SecurityRanges : public Collection<SecurityRange, PersistentSecurityRange> 
   {
   public:
      SecurityRanges();
      virtual ~SecurityRanges();

      // The two ranges a new database is made with - CreateTables*.sql inserts
      // them and SetDefault writes them back - by the options and the priority
      // each carries: "My computer" (127.0.0.1) and "Internet" (every IPv4
      // address). A new installation's defaults (Application/NewInstallDefaults.h)
      // ask whether a server still has exactly these before changing anything.
      enum ShippedRange
      {
         ShippedLocalOptions = 71627,
         ShippedLocalPriority = 30,
         ShippedInternetOptions = 96203,
         ShippedInternetPriority = 10
      };

      // False when the read failed, and the collection is then left as it was
      // (Collection::DBLoad_).
      bool Refresh();

      // False if the defaults could not be written. The existing ranges are deleted
      // first, so a false return means the server may now be left with fewer ranges
      // than it started with - the caller is expected to say so rather than report
      // that the defaults were restored.
      bool SetDefault();

   protected:
      virtual String GetCollectionName() const {return "SecurityRanges"; } 
   private:

   };

}