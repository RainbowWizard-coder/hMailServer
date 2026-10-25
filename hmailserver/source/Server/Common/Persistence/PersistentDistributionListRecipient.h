// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include "../BO/DistributionListRecipient.h"
#include "PersistenceMode.h"

namespace HM
{  

   class PersistentDistributionListRecipient
   {
   public:
      PersistentDistributionListRecipient(void);
      ~PersistentDistributionListRecipient(void);

      static bool DeleteByListID(__int64 iListID);
      static bool DeleteObject(std::shared_ptr<DistributionListRecipient> pRecipient);
      
      static bool SaveObject(std::shared_ptr<DistributionListRecipient> pRecipient);
      static bool SaveObject(std::shared_ptr<DistributionListRecipient> pRecipient, String &sErrorMessage, PersistenceMode mode);

      static bool ReadObject(std::shared_ptr<DistributionListRecipient> pRecipient, long ObjectID);

      // The list manager's two lookups (ListManager.h): the row a link's token
      // names, and a list's row for one address, compared without regard to
      // case - the manager stores the addresses it adds lower-cased, and an
      // administrator's rows are as they were typed.
      static bool ReadByToken(std::shared_ptr<DistributionListRecipient> pRecipient, const String &token);
      static bool ReadByAddress(std::shared_ptr<DistributionListRecipient> pRecipient, __int64 listId, const String &address);
      static bool ReadObject(std::shared_ptr<DistributionListRecipient> pRecipient, const SQLCommand &command);
      static bool ReadObject(std::shared_ptr<DistributionListRecipient> pRecipient, std::shared_ptr<DALRecordset> pRS);

      // The list manager's own writes to an existing row, each of the columns
      // it concerns and nothing else (SaveObject writes them for a new row
      // only). SaveTokenIfEmpty and AddBounce read the row back onto the
      // object afterwards, so the object holds what is stored.
      static bool SaveTokenIfEmpty(std::shared_ptr<DistributionListRecipient> pRecipient, const String &token);
      static bool SaveState(std::shared_ptr<DistributionListRecipient> pRecipient);
      static bool SaveDeliveryState(std::shared_ptr<DistributionListRecipient> pRecipient);
      static bool AddBounce(std::shared_ptr<DistributionListRecipient> pRecipient, bool hard, __int64 now, __int64 windowStart);

   private:

      static bool Reread_(std::shared_ptr<DistributionListRecipient> pRecipient);
   };
}
