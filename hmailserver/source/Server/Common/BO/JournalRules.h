// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Collection.h"

#include "JournalRule.h"
#include "../Persistence/PersistentJournalRule.h"

namespace HM
{
   // Every journaling rule on the server, for Settings.JournalRules and the
   // REST resource; or one domain's, for the backup that carries them under the
   // domain. See JournalRule.h.
   class JournalRules : public Collection<JournalRule, PersistentJournalRule>
   {
   public:
      JournalRules();
      explicit JournalRules(__int64 domainId);
      virtual ~JournalRules();

      // Every rule, or this domain's when the collection was made for one. False
      // when the database could not be read, with the collection left as it was.
      bool Refresh();

      __int64 GetDomainID() const { return domain_id_; }

   protected:

      virtual String GetCollectionName() const { return "JournalRules"; }

      // A restore: the rule takes the id the same restore gave its domain, never
      // the one the archive recorded, and an account rule finds its account by
      // the address the archive carries - the accounts are restored before the
      // rules (Domain::XMLLoadSubItems), so the address names a row by then.
      virtual bool PreSaveObject(std::shared_ptr<JournalRule> rule, XNode *node);

   private:

      __int64 domain_id_;
   };
}
