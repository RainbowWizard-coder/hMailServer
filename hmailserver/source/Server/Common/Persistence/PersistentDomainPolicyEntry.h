// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PersistenceMode.h"

namespace HM
{
   class DomainPolicyEntry;

   // hm_domainpolicylist (schema 6073). Every save and delete marks
   // DomainPolicyList's cache for reload, so a row added because mail is
   // arriving from somebody is in effect for the next session, and a row
   // removed because it was a mistake stops refusing at once.
   class PersistentDomainPolicyEntry
   {
   public:
      PersistentDomainPolicyEntry();
      ~PersistentDomainPolicyEntry();

      static bool DeleteObject(std::shared_ptr<DomainPolicyEntry> pObject);
      static bool SaveObject(std::shared_ptr<DomainPolicyEntry> pObject, String &errorMessage, PersistenceMode mode);
      static bool SaveObject(std::shared_ptr<DomainPolicyEntry> pObject);
      static bool ReadObject(std::shared_ptr<DomainPolicyEntry> pObject, std::shared_ptr<DALRecordset> pRS);

      // The row with this pattern and match key, or 0. The pair is unique in
      // the table, so a second save of the same pair is refused before the
      // database refuses it with a constraint error nobody can read.
      static __int64 Find(const String &pattern, int matchOn);
   };
}
