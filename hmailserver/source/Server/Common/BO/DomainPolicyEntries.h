// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Collection.h"

#include "DomainPolicyEntry.h"
#include "../Persistence/PersistentDomainPolicyEntry.h"

namespace HM
{
   // The whole domain white and black list, hm_domainpolicylist. There is one
   // list for the server: it is consulted before anything knows which recipient
   // a session is for, which is why it is not per domain and not per account.
   class DomainPolicyEntries : public Collection<DomainPolicyEntry, PersistentDomainPolicyEntry>
   {
   public:
      DomainPolicyEntries();
      ~DomainPolicyEntries();

      // Every row, by pattern. False when the database could not be read, with
      // the collection left as it was: "an empty list" and "could not tell" are
      // different answers, and only the first may let a black domain through.
      bool Refresh();

   protected:

      virtual String GetCollectionName() const { return "DomainPolicyList"; }
   };
}
