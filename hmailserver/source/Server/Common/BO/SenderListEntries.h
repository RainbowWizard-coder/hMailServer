// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Collection.h"

#include "SenderListEntry.h"
#include "../Persistence/PersistentSenderListEntry.h"

namespace HM
{
   class SenderListEntries : public Collection<SenderListEntry, PersistentSenderListEntry>
   {
   public:
      SenderListEntries();
      explicit SenderListEntries(int scope, __int64 ownerId);
      ~SenderListEntries();

      // One scope's entries, newest last. Returns false when the database could
      // not be read, and leaves the collection untouched - the caller has to be
      // able to tell "nobody has listed anything" from "nothing could be read",
      // because treating the second as the first turns a database blip into a
      // list that quietly stops allowing anyone.
      bool Refresh();

      // Every entry of every scope, for the cache, which decides precedence in
      // memory rather than asking the database once per recipient.
      bool RefreshAll();

      int GetScope() const { return scope_; }
      __int64 GetOwnerID() const { return owner_id_; }

      // Used by the COM collection and the REST create: a new entry already
      // carrying the scope this collection is for, so nothing has to remember
      // to set it.
      std::shared_ptr<SenderListEntry> CreateEntry() const;

   protected:

      virtual String GetCollectionName() const { return "SenderListEntries"; }

   private:

      int scope_;
      __int64 owner_id_;
   };
}
