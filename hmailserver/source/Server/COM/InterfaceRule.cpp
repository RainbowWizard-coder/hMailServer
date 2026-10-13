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

namespace
{
   // Whether Rule.Save would write the action as its row holds it already: the same
   // type, and the same value in every field an action carries - its place in the
   // rule aside, which MoveUp and MoveDown store as they move it, and which none of
   // RuleGuard's checks reads.
   bool IsStoredAsItIs(HM::RuleAction &stored, HM::RuleAction &action)
   {
      return stored.GetType() == action.GetType() &&
             stored.GetSubject() == action.GetSubject() &&
             stored.GetBody() == action.GetBody() &&
             stored.GetFromName() == action.GetFromName() &&
             stored.GetFromAddress() == action.GetFromAddress() &&
             stored.GetIMAPFolder() == action.GetIMAPFolder() &&
             stored.GetTo() == action.GetTo() &&
             stored.GetFilename() == action.GetFilename() &&
             stored.GetScriptFunction() == action.GetScriptFunction() &&
             stored.GetAbortSpamFlagged() == action.GetAbortSpamFlagged() &&
             stored.GetHeaderName() == action.GetHeaderName() &&
             stored.GetValue() == action.GetValue() &&
             stored.GetRouteID() == action.GetRouteID();
   }

   // The rule's actions as its rows hold them - or none, so that every action is held,
   // for a rule not stored yet, and for one whose owner in memory is not the owner it
   // is stored under: a rule moved to another account (Rule.AccountID) has each of its
   // actions held to the owner it is moved to.
   std::shared_ptr<HM::RuleActions> ReadStoredActionsUnderTheSameOwner(HM::Rule &rule)
   {
      __int64 storedAccountId = 0;
      __int64 storedDomainId = 0;
      if (rule.GetID() <= 0 ||
          !HM::PersistentRule::ReadOwner(rule.GetID(), storedAccountId, storedDomainId) ||
          storedAccountId != rule.GetAccountID() || storedDomainId != rule.GetDomainID())
         return std::shared_ptr<HM::RuleActions>();

      std::shared_ptr<HM::RuleActions> stored = std::shared_ptr<HM::RuleActions>(new HM::RuleActions(rule.GetID()));
      stored->Refresh();
      return stored;
   }
}

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
      //
      // An action this save writes as its row holds it already, under the owner the
      // rule is stored under, is not held: the save changes nothing of it, and one
      // these checks refuse was stored before them, restored from a backup or written
      // to the table, and is not taken when the rule fires (RuleApplier skips the header
      // and the reply, ScriptServer::FireEvent refuses the name). Held here, it refused
      // every change to the rule around it - a criterion added or removed, another
      // action, the name - and the Control Panel, which stores a criterion or an action
      // before it saves the rule, told of the stored part as not saved, stored it again
      // at each retry, and left delivery with the rule as it was, the reload of the
      // owner's rules being asked for by the save refused (PersistentRule::SaveObject).
      std::shared_ptr<HM::RuleActions> storedActions = ReadStoredActionsUnderTheSameOwner(*object_);
      for (std::shared_ptr<HM::RuleAction> action : object_->GetActions()->GetSnapshot())
      {
         if (storedActions && action->GetID() > 0)
         {
            std::shared_ptr<HM::RuleAction> stored = storedActions->GetItemByDBID(action->GetID());
            if (stored && IsStoredAsItIs(*stored, *action))
               continue;
         }

         const HM::String refusal = HM::RuleGuard::WhyActionMayNotBeStored(object_->GetAccountID(), object_->GetDomainID(), *action);
         if (!refusal.IsEmpty())
            return COMError::GenerateError(refusal);

         // bind_to_address, send_using_route and script_function reach past the
         // rule's owner into the server's configuration (another tenant's
         // address, a route's stored credentials, the server's script), and on a
         // domain's or an account's rule only the server administrator may set
         // one - a domain administrator, who may change its own and its accounts'
         // rules, may not. A global rule's are the administrator's as before.
         if (HM::RuleGuard::ActionReachesBeyondRuleOwner(object_->GetAccountID(), object_->GetDomainID(), *action) &&
             !authentication_->GetIsServerAdmin())
            return COMError::GenerateError(
               L"This action binds to one of the server's addresses, sends through a server route, or calls a function of "
               L"the server's script, which only the server administrator may set on a domain's or an account's rule.");
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

