// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Collection.h"

#include "../Persistence/PersistentDomain.h"
#include "Domain.h"

namespace HM
{
   class Domains : public Collection<Domain, PersistentDomain>
   {
   public:
      Domains();
      virtual ~Domains();

      void Refresh();

      // The same read, with the answer DBLoad_ already had.
      //
      // Refresh() throws that bool away, which is right for a caller that only
      // wants the collection as good as it can be got; it is wrong for one that
      // acts on "there is no domain of that name", because Collection::DBLoad_
      // leaves the collection untouched when a read fails and a freshly built
      // one is then empty - indistinguishable from a server with no domains.
      // SampleDomain::Examine is that caller: it must not read a database it
      // could not open as "the sample has been deleted".
      bool RefreshChecked();

      void Refresh(__int64 iDomainID);
      String GetNames();

   protected:
      virtual String GetCollectionName() const {return "Domains"; }
   private:

   };
}
