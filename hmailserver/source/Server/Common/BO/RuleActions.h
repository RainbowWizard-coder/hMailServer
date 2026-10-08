// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Collection.h"

#include "../Persistence/PersistentRuleAction.h"

#include "RuleAction.h"

namespace HM
{
   class RuleActions : public Collection<RuleAction, PersistentRuleAction>
   {
   public:
      RuleActions(__int64 RuleID);
      ~RuleActions(void);
      
      void Refresh(); 

      __int64 GetRuleID() const {return rule_id_; }

      // The rule the collection belongs to, once a rule made with no id was saved
      // and given one (PersistentRule::SaveObject).
      void SetRuleID(__int64 ruleId) {rule_id_ = ruleId; }

      void MoveUp(std::shared_ptr<RuleAction> pRuleAction);
      void MoveDown(std::shared_ptr<RuleAction> pRuleAction);

   protected:
      virtual String GetCollectionName() const {return "RuleActions"; } 
      virtual bool PreSaveObject(std::shared_ptr<RuleAction> pRuleAction, XNode *node);
   private:

      std::vector<std::shared_ptr<RuleAction> >::iterator GetRuleActionIterator_(std::shared_ptr<RuleAction> pRuleAction);
      void UpdateSortOrder_();

      __int64 rule_id_;
   };
}