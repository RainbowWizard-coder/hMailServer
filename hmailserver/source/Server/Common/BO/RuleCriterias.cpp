// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "RuleCriterias.h"
#include "RuleCriteria.h"
#include "../Persistence/PersistentRuleCriteria.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   RuleCriterias::RuleCriterias(__int64 iRuleID) :
      rule_id_(iRuleID)
   {
   }

   RuleCriterias::~RuleCriterias(void)
   {

   }

   void
   RuleCriterias::Refresh()
   {
      // A rule not yet saved has nothing stored. The rows of rule id 0 are no rule's -
      // left by a rule whose own insert failed, before PersistentRule::SaveObject
      // checked it - and reading them gave a new rule criteria nobody gave it, which
      // its Save then stored as its own. What the collection holds in memory stays.
      if (rule_id_ <= 0)
         return;

      String sSQL;
      sSQL.Format(_T("select * from hm_rule_criterias where criteriaruleid = %I64d order by criteriaid asc"), rule_id_);

      DBLoad_(sSQL);
   }


   bool
   RuleCriterias::PreSaveObject(std::shared_ptr<RuleCriteria> pRuleCriteria, XNode *node)
   {
      pRuleCriteria->SetRuleID(rule_id_);
      return true;
   }   
}