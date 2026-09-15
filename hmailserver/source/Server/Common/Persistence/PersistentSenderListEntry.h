// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PersistenceMode.h"

namespace HM
{
   class SenderListEntry;

   class PersistentSenderListEntry
   {
   public:
      PersistentSenderListEntry();
      ~PersistentSenderListEntry();

      static bool DeleteObject(std::shared_ptr<SenderListEntry> pObject);
      static bool SaveObject(std::shared_ptr<SenderListEntry> pObject, String &errorMessage, PersistenceMode mode);
      static bool SaveObject(std::shared_ptr<SenderListEntry> pObject);
      static bool ReadObject(std::shared_ptr<SenderListEntry> pObject, std::shared_ptr<DALRecordset> pRS);

      // Every entry of one owner, for the delete of an account or a domain.
      // Account ids and domain ids are reissued, so a list left behind would be
      // inherited by whoever gets the number next - somebody else's allow list
      // silently becoming yours is the shape of failure this prevents.
      static bool DeleteByOwner(int scope, __int64 ownerId);

      // The expiry sweep. An entry whose time has passed is already ignored by
      // everything that reads the list (SenderListEntry::HasExpired), so this is
      // housekeeping rather than enforcement, and a failure is survivable.
      static bool DeleteExpired();

      // Whether this owner already lists this value, ignoring the kind: a person
      // who lists the same address twice, once to allow and once to block, has
      // written a contradiction into their own list rather than a rule, so the
      // second write replaces the first rather than sitting beside it.
      // out_id is the existing row when there is one.
      static bool Find(int scope, __int64 ownerId, const String &value, __int64 &out_id);
   };
}
