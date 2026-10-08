// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Collection.h"

#include "../Persistence/PersistentRuleCriteria.h"
#include "RuleCriteria.h"

namespace HM
{
   class RuleCriterias : public Collection<RuleCriteria, PersistentRuleCriteria>
   {
   public:
      RuleCriterias(__int64 iRuleID);
      ~RuleCriterias(void);

      void Refresh();

      __int64 GetRuleID() const {return rule_id_; }

      // The rule the collection belongs to, once a rule made with no id was saved
      // and given one (PersistentRule::SaveObject).
      void SetRuleID(__int64 ruleId) {rule_id_ = ruleId; }

   protected:
      virtual String GetCollectionName() const {return "RuleCriterias"; }
      virtual bool PreSaveObject(std::shared_ptr<RuleCriteria> pRuleCriteria, XNode *node);
   private:
      __int64 rule_id_;
   };
}