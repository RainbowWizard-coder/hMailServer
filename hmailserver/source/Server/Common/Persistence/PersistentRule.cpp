// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "StdAfx.h"

#include "PersistentRule.h"
#include "PersistentRuleCriteria.h"
#include "PersistentRuleAction.h"

#include "../BO/Rule.h"

#include "../BO/RuleActions.h"
#include "../BO/RuleAction.h"
#include "../BO/RuleCriterias.h"
#include "../BO/RuleCriteria.h"

#include "../Application/ObjectCache.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   PersistentRule::PersistentRule(void)
   {
   }

   PersistentRule::~PersistentRule(void)
   {
   }

   bool
   PersistentRule::ReadObject(std::shared_ptr<Rule> pRule, const SQLCommand& sSQL)
   {
      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(sSQL);
      if (!pRS)
         return false;

      bool bRetVal = false;
      if (!pRS->IsEOF())
      {
         bRetVal = ReadObject(pRule, pRS);
      }

      return bRetVal;
   }

   bool
   PersistentRule::ReadOwner(__int64 ruleId, __int64 &accountId, __int64 &domainId)
   {
      accountId = 0;
      domainId = 0;

      if (ruleId <= 0)
         return false;

      SQLCommand command("select ruleaccountid, ruledomainid from hm_rules where ruleid = @RULEID");
      command.AddParameter("@RULEID", ruleId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      accountId = recordset->GetInt64Value("ruleaccountid");
      domainId = recordset->GetInt64Value("ruledomainid");
      return true;
   }

   bool
   PersistentRule::ReadObject(std::shared_ptr<Rule> pRule, std::shared_ptr<DALRecordset> pRS)
   {
      if (pRS->IsEOF())
         return false;

      pRule->SetID(pRS->GetLongValue("ruleid"));
      pRule->SetAccountID(pRS->GetLongValue("ruleaccountid"));
      pRule->SetDomainID(pRS->GetLongValue("ruledomainid"));
      pRule->SetName(pRS->GetStringValue("rulename"));
      pRule->SetActive(pRS->GetLongValue("ruleactive") ? true : false);
      pRule->SetUseAND(pRS->GetLongValue("ruleuseand") ? true : false);
      pRule->SetSortOrder(pRS->GetLongValue("rulesortorder"));

      // Read actions
      pRule->GetActions();

      // Read criterias
      pRule->GetCriterias();

      return true;
   }

   bool
   PersistentRule::SaveObject(std::shared_ptr<Rule> pRule, String &errorMessage, PersistenceMode mode)
   {
      // errorMessage not supported yet.
      return SaveObject(pRule);
   }


   bool
   PersistentRule::SaveObject(std::shared_ptr<Rule> pRule)
   {
      SQLStatement oStatement;
      oStatement.SetTable("hm_rules");
      
      bool bNewObject = pRule->GetID() == 0;

      if (bNewObject)
      {
         oStatement.SetStatementType(SQLStatement::STInsert);
         oStatement.SetIdentityColumn("ruleid");
      }
      else
      {
         oStatement.SetStatementType(SQLStatement::STUpdate);
         String sWhere;
         sWhere.Format(_T("ruleid = %I64d"), pRule->GetID());
         oStatement.SetWhereClause(sWhere);
      }

      oStatement.AddColumnInt64("ruleaccountid", pRule->GetAccountID());
      oStatement.AddColumnInt64("ruledomainid", pRule->GetDomainID());
      oStatement.AddColumn("rulename", pRule->GetName());
      oStatement.AddColumn("ruleactive", 0);
      oStatement.AddColumn("ruleuseand", pRule->GetUseAND());
      oStatement.AddColumn("rulesortorder", pRule->GetSortOrder());

      // Save and fetch ID
      __int64 iDBID = 0;
      bool bRetVal = Application::Instance()->GetDBManager()->Execute(oStatement, bNewObject ? &iDBID : 0);
      if (bRetVal && bNewObject)
         pRule->SetID((int) iDBID);

      // This was not checked before the criteria and actions were saved beneath it,
      // so a new rule whose own insert failed left GetID() at 0 and every criterion
      // and action below was written against ruleid 0 - rows belonging to no rule,
      // which nothing ever reads and nothing ever cleans up.
      if (!bRetVal)
         return false;

      // Save criterias.
      //
      // The whole function writes ruleactive = 0 above and sets it back at the end,
      // which is the right shape: a rule is not live while it is half written. What
      // it did not do was check any of the parts before reactivating, and the
      // consequence is specific rather than general. Criteria are combined with AND
      // or OR, and under AND every criterion is a *restriction* - so a rule that was
      // meant to read "delete if from X and the subject contains Y" and lost the
      // second criterion becomes "delete if from X", which is a strictly broader rule
      // than the administrator wrote, applied to live mail, with the tool still
      // displaying the criterion that is not there.
      //
      // A rule with no criteria at all is inert - ApplyRule_ leaves bDoActions false -
      // so the empty case was never the danger. The partial case was.
      bool allPartsSaved = true;

      // The rule's criteria and actions collections may have been made before the rule
      // had an id - COM's Rule.Criterias or Rule.Actions read on a new rule, as
      // InterfaceRule::Save itself reads the actions to hold them to the rule's owner -
      // and they are bound to the id the insert gave it. A collection left at rule id 0
      // stored nothing added to it afterwards through the item's own Save
      // (InterfaceRuleAction::Save and InterfaceRuleCriteria::Save store an item only
      // under a rule id), until the rule itself was saved again, and RuleAction.Save
      // held such an action to no owner's limits: an account's rule took a Set header
      // value of From, which RuleGuard::WhyActionMayNotBeStored refuses it, without a
      // word.
      __int64 iRuleID = pRule->GetID();
      std::shared_ptr<RuleCriterias> pRuleCriterias = pRule->GetCriterias();
      pRuleCriterias->SetRuleID(iRuleID);
      pRule->GetActions()->SetRuleID(iRuleID);
      for (int i = 0; i < pRuleCriterias->GetCount(); i++)
      {
         std::shared_ptr<RuleCriteria> pRuleCriteria = pRuleCriterias->GetItem(i);
         pRuleCriteria->SetRuleID(iRuleID);

         if (!PersistentRuleCriteria::SaveObject(pRuleCriteria))
            allPartsSaved = false;
      }

      // Save actions
      std::shared_ptr<RuleActions> pRuleActions = pRule->GetActions();
      for (int i = 0; i < pRuleActions->GetCount(); i++)
      {
         std::shared_ptr<RuleAction> pRuleAction = pRuleActions->GetItem(i);
         pRuleAction->SetRuleID(iRuleID);

         if (!PersistentRuleAction::SaveObject(pRuleAction))
            allPartsSaved = false;
      }

      // Leaving it inactive is the safe half of the choice, and it is not a silent
      // one: the rule is still there to be looked at and saved again once the
      // database is working. A rule that does not run is a nuisance; a rule that runs
      // with half its conditions is a rule doing something nobody asked for.
      if (!allPartsSaved)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 6101, "PersistentRule::SaveObject",
            Formatter::Format("Rule {0} was saved but some of its criteria or actions were not, so it has been left INACTIVE rather than run with conditions the administrator did not write. Correct the database problem and save the rule again.",
               pRule->GetName()));

         return false;
      }

      // Set the rule to active again.
      SQLCommand command("update hm_rules set ruleactive = @ACTIVE where ruleid = @RULEID");
      command.AddParameter("@ACTIVE", pRule->GetActive());
      command.AddParameter("@RULEID", iRuleID);

      bRetVal = Application::Instance()->GetDBManager()->Execute(command);

      NotifyReload_(pRule);

      return bRetVal;

   }

   void
   PersistentRule::DeleteByDomainID(__int64 iDomainID)
   {
      SQLCommand selectCommand("select * from hm_rules where ruledomainid = @DOMAINID");
      selectCommand.AddParameter("@DOMAINID", iDomainID);

      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(selectCommand);
      if (!pRS)
         return;

      while (!pRS->IsEOF())
      {
         std::shared_ptr<Rule> rule = std::shared_ptr<Rule>(new Rule);
         if (ReadObject(rule, pRS))
            DeleteObject(rule);
         pRS->MoveNext();
      }
   }

   void
   PersistentRule::DeleteByAccountID(__int64 iAccountID)
   {
      SQLCommand selectCommand("select * from hm_rules where ruleaccountid = @ACCOUNTID");
      selectCommand.AddParameter("@ACCOUNTID", iAccountID);

      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(selectCommand);
      if (!pRS)
         return ;

      bool bRetVal = false;
      while (!pRS->IsEOF())
      {
         // Create and read the fetch account.
         std::shared_ptr<Rule> oRule = std::shared_ptr<Rule>(new Rule);

         if (ReadObject(oRule, pRS))
         {
            // Delete this fetch account and all the 
            // UID's connected to it.
            DeleteObject(oRule);
         }

         pRS->MoveNext();
      }

      // All the fetch accounts have been deleted.

   }

   bool
   PersistentRule::DeleteObject(std::shared_ptr<Rule> pRule)
   {
      SQLCommand command("delete from hm_rules where ruleid = @RULEID");
      command.AddParameter("@RULEID", pRule->GetID());

      Application::Instance()->GetDBManager()->Execute(command);

      PersistentRuleAction::DeleteObjects(pRule->GetID());
      PersistentRuleCriteria::DeleteObjects(pRule->GetID());

      NotifyReload_(pRule);

      return true;
   }

   bool
   PersistentRule::SaveSortOrder(std::shared_ptr<Rule> pRule)
   {
      SQLCommand command("update hm_rules set rulesortorder = @SORTORDER where ruleid = @RULEID");
      command.AddParameter("@SORTORDER", pRule->GetSortOrder());
      command.AddParameter("@RULEID", pRule->GetID());

      if (!Application::Instance()->GetDBManager()->Execute(command))
         return false;

      // The rules of the owner the row names, which a move leaves as it was, are read
      // again in their new order - not those of an owner the rule in memory was given
      // and not saved under.
      __int64 accountId = 0;
      __int64 domainId = 0;
      if (ReadOwner(pRule->GetID(), accountId, domainId))
         NotifyReload_(accountId, domainId);

      return true;
   }

   void
   PersistentRule::NotifyReload_(std::shared_ptr<Rule> pRule)
   {
      NotifyReload_(pRule->GetAccountID(), pRule->GetDomainID());
   }

   void
   PersistentRule::NotifyReload_(__int64 accountId, __int64 domainId)
   {
      if (domainId > 0)
         ObjectCache::Instance()->SetDomainRulesNeedsReload(domainId);
      else if (accountId == 0)
         ObjectCache::Instance()->SetGlobalRulesNeedsReload();
      else
         ObjectCache::Instance()->SetAccountRulesNeedsReload(accountId);
   }
}