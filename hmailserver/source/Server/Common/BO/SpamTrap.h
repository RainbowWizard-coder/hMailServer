// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../Util/VariantDateTime.h"

namespace HM
{
   // One spam trap: an address in a hosted domain that nobody was ever given,
   // declared by an administrator so that a message addressed to it can be taken
   // as what it is - a sender working from a harvested, bought or guessed list.
   // SpamTrapMonitor decides what such a message does.
   //
   // Held as the local part and the domain's id rather than as the whole address,
   // so that renaming the domain takes its traps with it, as it takes its accounts.
   // The address is put together from the two whenever it is needed.
   class SpamTrap : public BusinessObject<SpamTrap>
   {
   public:
      SpamTrap();
      ~SpamTrap();

      // What the collection's GetItemByName compares.
      String GetName() const { return local_part_; }

      __int64 GetDomainID() const { return domain_id_; }
      void SetDomainID(__int64 newVal) { domain_id_ = newVal; }

      String GetLocalPart() const { return local_part_; }

      // Trimmed and lower-cased on the way in, so that every reader compares the
      // same text the RCPT TO path compares.
      void SetLocalPart(const String &newVal);

      DateTime GetCreated() const { return created_; }
      void SetCreated(DateTime newVal) { created_ = newVal; }

      bool XMLStore(XNode *pNode, int iOptions);
      bool XMLLoad(XNode *pNode, int iOptions);
      bool XMLLoadSubItems(XNode *pNode, int iOptions) { return true; }

   private:

      __int64 domain_id_;
      String local_part_;
      DateTime created_;
   };
}
