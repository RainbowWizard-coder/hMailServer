// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "InterfaceRule.h"

#include "../Common/BO/Rule.h"
#include "../Common/BO/Rules.h"
#include "../Common/BO/RuleAction.h"
#include "../Common/BO/RuleActions.h"
#include "../Common/Rules/RuleGuard.h"
#include "../Common/Persistence/PersistentRule.h"

#include "InterfaceRuleCriterias.h"
#include "InterfaceRuleActions.h"

#include "COMError.h"
#include "../Common/Util/AuditTrail.h"

STDMETHODIMP InterfaceRule::get_ID(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetID();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRule::get_AccountID(LONG* pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetAccountID();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRule::put_AccountID(LONG newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Moving the rule is changing both owners: the one it has now and the
      // account it would belong to.
      if (!authentication_->MayChangeRuleOwnedBy(object_->GetAccountID(), object_->GetDomainID()) ||
          !authentication_->MayChangeRuleOwnedBy(newVal, newVal > 0 ? 0 : object_->GetDomainID()))
         return COMError::GenerateError(HM::COMAuthentication::NotTakenIn);

      object_->SetAccountID(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRule::get_Name(BSTR* pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetName().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRule::put_Name(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetName(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRule::get_Active(VARIANT_BOOL* pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetActive() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRule::put_Active(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetActive(newVal == VARIANT_TRUE);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRule::get_UseAND(VARIANT_BOOL* pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetUseAND() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRule::put_UseAND(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetUseAND(newVal == VARIANT_TRUE);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRule::get_Criterias(IInterfaceRuleCriterias **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      CComObject<InterfaceRuleCriterias>* pItem = new CComObject<InterfaceRuleCriterias >();
      pItem->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::RuleCriterias> pRuleCriterias = object_->GetCriterias();
   
      if (pRuleCriterias)
      {
         pItem->Attach(pRuleCriterias);
         pItem->AddRef();
         *pVal = pItem;
      }
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRule::get_Actions(IInterfaceRuleActions **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      CComObject<InterfaceRuleActions>* pItem = new CComObject<InterfaceRuleActions >();
      pItem->SetAuthentication(authentication_);

      std::shared_ptr<HM::RuleActions> pRuleActions = object_->GetActions();
   
      if (pRuleActions)
      {
         pItem->Attach(pRuleActions);
         pItem->AddRef();
         *pVal = pItem;
      }
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRule::MoveUp()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->MayChangeRuleOwnedBy(object_->GetAccountID(), object_->GetDomainID()))
         return COMError::GenerateError(HM::COMAuthentication::NotTakenIn);

      if (object_->GetID() <= 0)
         return COMError::GenerateError("Object not yet saved.");
   
      parent_collection_->MoveUp(object_->GetID());
      return S_OK;
   
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRule::MoveDown()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->MayChangeRuleOwnedBy(object_->GetAccountID(), object_->GetDomainID()))
         return COMError::GenerateError(HM::COMAuthentication::NotTakenIn);

      if (object_->GetID() <= 0)
         return COMError::GenerateError("Object not yet saved.");
         
      parent_collection_->MoveDown(object_->GetID());
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRule::Save()
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!object_)
         return GetAccessDenied();

      // The rule's owner must be one this credential may change - an
      // account's, a domain's or the server's (MayChangeRuleOwnedBy) - and,
      // for a rule already stored, so must the owner it was stored under.
      if (!authentication_->MayChangeRuleOwnedBy(object_->GetAccountID(), object_->GetDomainID()) ||
          (object_->GetID() > 0 && !authentication_->MayChangeRule(object_->GetID())))
         return COMError::GenerateError(HM::COMAuthentication::NotTakenIn);

      // Saving the rule saves its actions as they are in memory, so an action changed
      // after RuleAction.Save, or one added before the rule had an owner it was saved
      // under, is held here to the rule's owner as it is now: a Set header value
      // action to the headers a rule of this owner may set, and a Reply action to the
      // addresses it may send from (RuleGuard::WhyActionMayNotBeStored). A Run
      // function action is held here too, whoever owns the rule, to a name the server
      // runs (RuleGuard::WhyScriptFunctionMayNotBeStored).
      for (std::shared_ptr<HM::RuleAction> action : object_->GetActions()->GetSnapshot())
      {
         const HM::String refusal = HM::RuleGuard::WhyActionMayNotBeStored(object_->GetAccountID(), object_->GetDomainID(), *action);
         if (!refusal.IsEmpty())
            return COMError::GenerateError(refusal);
      }

      // Set the sort order of the rule.
      if (object_->GetID() == 0 && object_->GetSortOrder() == 0)
      {
         std::vector<std::shared_ptr<HM::Rule> > vecExistingRules = parent_collection_->GetSnapshot();
   
         // Determine the highest SortOrder.
         if (vecExistingRules.size() == 0)
            object_->SetSortOrder(1);
         else
         {
            std::shared_ptr<HM::Rule> pLastRule = vecExistingRules[vecExistingRules.size() -1];
            object_->SetSortOrder(pLastRule->GetSortOrder() +1);
         }
      }
   
      if (HM::PersistentRule::SaveObject(object_))
      {
         // Add to parent collection
         AddToParentCollection();
   
         return S_OK;
      }
   
      return COMError::GenerateError("Failed to save object. See hMailServer error log.");
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRule::Delete()
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!object_)
         return GetAccessDenied();

      if (!authentication_->MayChangeRuleOwnedBy(object_->GetAccountID(), object_->GetDomainID()) ||
          (object_->GetID() > 0 && !authentication_->MayChangeRule(object_->GetID())))
         return COMError::GenerateError(HM::COMAuthentication::NotTakenIn);

      if (!parent_collection_)
         return HM::PersistentRule::DeleteObject(object_) ? S_OK : S_FALSE;
   
      parent_collection_->DeleteItemByDBID(object_->GetID());
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

