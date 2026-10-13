// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "InterfaceRuleAction.h"

#include "../Common/BO/RuleAction.h"
#include "../Common/BO/RuleActions.h"
#include "../Common/Persistence/PersistentRule.h"
#include "../Common/Persistence/PersistentRuleAction.h"
#include "../Common/Rules/RuleGuard.h"
#include "../Common/Util/Encoding/ModifiedUTF7.h"

#include "COMError.h"
#include "../Common/Util/AuditTrail.h"

STDMETHODIMP InterfaceRuleAction::get_ID(long *pVal)
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

STDMETHODIMP InterfaceRuleAction::Save()
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!object_)
         return GetAccessDenied();

      // The rule the action is saved under, and the one it was reached
      // through, must be rules this credential may change.
      if (!authentication_->MayChangeRule(object_->GetRuleID()) ||
          (parent_collection_ && !authentication_->MayChangeRule(parent_collection_->GetRuleID())))
         return COMError::GenerateError(HM::COMAuthentication::NotTakenIn);

      // A rule may not set X-hMailServer-LoopCount, which ends a mail loop, nor write
      // a field under a name that is not one; a domain's or an account's rule may not
      // set who the message is from or the signature it came with; a reply goes only
      // from an address the rule's owner may send as; and a Run function action names
      // a plain function, the only name the server runs. Refused here with the reason,
      // as the REST API refuses it; such an action stored before is skipped when the
      // rule fires. An action of a rule not yet saved is held by Rule.Save, which saves
      // it then, to what depends on the rule's owner (RuleGuard::WhyActionMayNotBeStored);
      // what does not - a header name no rule may set, and the function's name
      // (RuleGuard::WhyScriptFunctionMayNotBeStored) - is refused here at once.
      {
         __int64 ruleId = object_->GetRuleID();
         if (ruleId <= 0 && parent_collection_)
            ruleId = parent_collection_->GetRuleID();

         HM::String refusal;
         if (ruleId > 0)
         {
            __int64 ruleAccountId = 0;
            __int64 ruleDomainId = 0;
            if (!HM::PersistentRule::ReadOwner(ruleId, ruleAccountId, ruleDomainId))
               return COMError::GenerateError("The rule this action belongs to could not be read, so the action was not saved. See hMailServer error log.");

            refusal = HM::RuleGuard::WhyActionMayNotBeStored(ruleAccountId, ruleDomainId, *object_);

            // bind_to_address, send_using_route and script_function reach past
            // the rule's owner into the server's configuration, so on a domain's
            // or an account's rule only the server administrator may set one, as
            // Rule.Save and the REST rule routes hold them
            // (RuleGuard::ActionReachesBeyondRuleOwner). A global rule's are the
            // administrator's as before.
            if (refusal.IsEmpty() &&
                HM::RuleGuard::ActionReachesBeyondRuleOwner(ruleAccountId, ruleDomainId, *object_) &&
                !authentication_->GetIsServerAdmin())
               return COMError::GenerateError(
                  L"This action binds to one of the server's addresses, sends through a server route, or calls a function of "
                  L"the server's script, which only the server administrator may set on a domain's or an account's rule.");
         }
         else if (object_->GetType() == HM::RuleAction::SetHeaderValue)
            refusal = HM::RuleGuard::WhyHeaderMayNotBeSet(object_->GetHeaderName());
         else if (object_->GetType() == HM::RuleAction::ScriptFunction)
            refusal = HM::RuleGuard::WhyScriptFunctionMayNotBeStored(object_->GetScriptFunction());

         if (!refusal.IsEmpty())
            return COMError::GenerateError(refusal);
      }

      // Set the sort order of the rule.
      if (object_->GetID() == 0 && object_->GetSortOrder() == 0)
      {
         std::vector<std::shared_ptr<HM::RuleAction> > vecExistingRuleActions = parent_collection_->GetSnapshot();
   
         // Determine the highest SortOrder.
         if (vecExistingRuleActions.size() == 0)
            object_->SetSortOrder(1);
         else
         {
            std::shared_ptr<HM::RuleAction> pLastRuleAction = vecExistingRuleActions[vecExistingRuleActions.size() -1];
            object_->SetSortOrder(pLastRuleAction->GetSortOrder() +1);
         }
      }
   
      if (parent_collection_->GetRuleID() > 0)
      {
         // An action made by Rule.Actions.Add before its rule had an id carries rule id
         // 0, and is stored under the rule its collection now belongs to
         // (PersistentRule::SaveObject binds the collection): a row of rule id 0 is no
         // rule's, and the foreign key every database holds hm_rule_actions to refuses it.
         if (object_->GetRuleID() <= 0)
            object_->SetRuleID(parent_collection_->GetRuleID());

         if (!HM::PersistentRuleAction::SaveObject(object_))
         {
            // Saving failed.
            return COMError::GenerateError("Failed to save object. See hMailServer error log.");
         }
      }
   
      AddToParentCollection();   
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::MoveUp()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->MayChangeRule(object_->GetRuleID()))
         return COMError::GenerateError(HM::COMAuthentication::NotTakenIn);

      parent_collection_->MoveUp(object_);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::MoveDown()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->MayChangeRule(object_->GetRuleID()))
         return COMError::GenerateError(HM::COMAuthentication::NotTakenIn);

      parent_collection_->MoveDown(object_);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::get_RuleID(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetRuleID();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::put_RuleID(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Moving the action to another rule is changing both rules.
      if (!authentication_->MayChangeRule(object_->GetRuleID()) || !authentication_->MayChangeRule(newVal))
         return COMError::GenerateError(HM::COMAuthentication::NotTakenIn);

      object_->SetRuleID(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::get_Type(eRuleActionType *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (eRuleActionType) object_->GetType();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::put_Type(eRuleActionType newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetType((HM::RuleAction::Type) newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::get_RouteID(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetRouteID();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::put_RouteID(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetRouteID(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::get_Subject(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetSubject().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::put_Subject(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetSubject(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::get_Body(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetBody().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::put_Body(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetBody(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::get_FromName(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetFromName().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::put_FromName(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetFromName(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::get_FromAddress(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetFromAddress().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::put_FromAddress(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetFromAddress(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::get_Filename(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetFilename().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::put_Filename(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetFilename(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::get_To(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetTo().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::put_To(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetTo(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::get_ScriptFunction(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetScriptFunction().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::put_ScriptFunction(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetScriptFunction(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::get_IMAPFolder(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      USES_CONVERSION;
   
      // Convert the modified UTF7 string into unicode.
      // Since hMailServer core don't know unicode, we
      // have to do it out here in the interface.
      
      HM::String sValue = object_->GetIMAPFolder();
      HM::String sResult = HM::ModifiedUTF7::Decode(T2A(sValue.GetBuffer()));
   
      *pVal = sResult.AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::put_IMAPFolder(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      HM::String sUnicode (newVal);
   
      // Convert the unciode string to a modified UTF7.
      // Since hMailServer core don't know unicode, we
      // have to do it out here in the interface.
      HM::AnsiString sRetVal = HM::ModifiedUTF7::Encode(sUnicode);
      object_->SetIMAPFolder(sRetVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::get_HeaderName(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetHeaderName().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::put_HeaderName(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetHeaderName(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::get_Value(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetValue().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::put_Value(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetValue(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::get_AbortSpamFlagged(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetAbortSpamFlagged() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::put_AbortSpamFlagged(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetAbortSpamFlagged(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleAction::Delete()
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!object_)
         return GetAccessDenied();

      if (!authentication_->MayChangeRule(object_->GetRuleID()))
         return COMError::GenerateError(HM::COMAuthentication::NotTakenIn);

      if (!parent_collection_)
         return HM::PersistentRuleAction::DeleteObject(object_) ? S_OK : S_FALSE;
   
      parent_collection_->DeleteItemByDBID(object_->GetID());
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

