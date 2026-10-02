// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Collection.h"

#include "NeverBanName.h"
#include "../Persistence/PersistentNeverBanName.h"

namespace HM
{
   // The whole never-ban list by reverse name, hm_neverbannames. There is one
   // list for the server, as there is one AutoBanNeverBan: an auto-ban is a
   // server-wide IP range, made before anything knows which domain a client is
   // for.
   class NeverBanNames : public Collection<NeverBanName, PersistentNeverBanName>
   {
   public:
      NeverBanNames();
      ~NeverBanNames();

      // Every row, by pattern. False when the database could not be read, with
      // the collection left as it was: "an empty list" and "could not tell" are
      // different answers.
      bool Refresh();

   protected:

      virtual String GetCollectionName() const { return "NeverBanNames"; }
   };
}
