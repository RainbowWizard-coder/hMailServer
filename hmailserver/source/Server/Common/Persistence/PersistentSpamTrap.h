// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PersistenceMode.h"
#include "../Util/VariantDateTime.h"

#include <vector>

namespace HM
{
   class SpamTrap;

   // One time a trap was addressed: a row of hm_spamtraphits. The address is kept
   // as it was then, so a hit still reads correctly after the trap is removed or
   // the domain renamed; the domain id is what a listing for one domain filters on.
   struct SpamTrapHit
   {
      SpamTrapHit() :
         id(0),
         domain_id(0),
         banned(false),
         blocked(false)
      {
      }

      __int64 id;
      __int64 domain_id;
      String address;
      String ip_address;
      String sender;
      DateTime time;
      bool banned;
      bool blocked;

      // Why the address was not banned or the sender not blocked, or what was
      // done, in a sentence an administrator can act on.
      String note;
   };

   // hm_spamtraps and hm_spamtraphits (schema 6049).
   class PersistentSpamTrap
   {
   public:
      PersistentSpamTrap();
      ~PersistentSpamTrap();

      static bool DeleteObject(std::shared_ptr<SpamTrap> pObject);
      static bool SaveObject(std::shared_ptr<SpamTrap> pObject, String &errorMessage, PersistenceMode mode);
      static bool SaveObject(std::shared_ptr<SpamTrap> pObject);
      static bool ReadObject(std::shared_ptr<SpamTrap> pObject, std::shared_ptr<DALRecordset> pRS);

      // Whether the domain has a trap with this local part (already lower-cased).
      // out_id is its row when it has.
      static bool Find(__int64 domainId, const String &localPart, __int64 &out_id);

      // A domain's traps and the record of their hits, for the delete of the domain.
      // Domain ids are reissued, so a trap left behind would be inherited by the
      // next domain to take the number.
      static bool DeleteByDomain(__int64 domainId);

      static bool RecordHit(const SpamTrapHit &hit);

      // The newest maxRows hits, newest first; domainId 0 is every domain.
      static bool ReadHits(__int64 domainId, int maxRows, std::vector<SpamTrapHit> &hits);

      // How many hits are recorded; domainId 0 is every domain. -1 when the table
      // could not be read.
      static int CountHits(__int64 domainId);

      // domainId 0 is every domain.
      static bool DeleteHits(__int64 domainId);

      static bool DeleteHitsOlderThan(int days);
   };
}
