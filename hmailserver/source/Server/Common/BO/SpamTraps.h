// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Collection.h"

#include "SpamTrap.h"
#include "../Persistence/PersistentSpamTrap.h"

namespace HM
{
   // One domain's spam traps, or every domain's for the cache.
   class SpamTraps : public Collection<SpamTrap, PersistentSpamTrap>
   {
   public:
      SpamTraps();
      explicit SpamTraps(__int64 domainId);
      ~SpamTraps();

      // This domain's traps, by local part. False when the database could not be
      // read, with the collection left as it was: "no traps" and "could not tell"
      // are different answers, and only the first may stop a ban.
      bool Refresh();

      // Every trap of every domain, for SpamTrapMonitor's cache.
      bool RefreshAll();

      __int64 GetDomainID() const { return domain_id_; }

   protected:

      virtual String GetCollectionName() const { return "SpamTraps"; }

      // A restore: the trap takes the id the same restore gave its domain, never
      // the one the archive recorded, which may belong to another domain here.
      virtual bool PreSaveObject(std::shared_ptr<SpamTrap> trap, XNode *node);

   private:

      __int64 domain_id_;
   };
}
