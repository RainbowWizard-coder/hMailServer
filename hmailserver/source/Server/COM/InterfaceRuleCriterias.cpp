// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"
#include "COMError.h"
#include "../Common/Util/AuditTrail.h"
#include "InterfaceRuleCriterias.h"

#include "InterfaceRuleCriteria.h"

#include "../Common/BO/RuleCriterias.h"
#include "../Common/BO/RuleCriteria.h"

void 
InterfaceRuleCriterias::Attach(std::shared_ptr<HM::RuleCriterias> pCriterias)
{
   rule_criterias = pCriterias;
}

STDMETHODIMP InterfaceRuleCriterias::get_ItemByDBID(long lDBID, IInterfaceRuleCriteria** pVal)
{
   try
   {
      if (!rule_criterias)
         return GetAccessDenied();

      std::shared_ptr<HM::RuleCriteria> pRule = rule_criterias->GetItemByDBID(lDBID);
      if (!pRule)
         return COMError::WithoutMessage(DISP_E_BADINDEX);
   
      CComObject<InterfaceRuleCriteria>* pInterfaceRuleCriteria = new CComObject<InterfaceRuleCriteria>();
      pInterfaceRuleCriteria->SetAuthentication(authentication_);
      pInterfaceRuleCriteria->AttachItem(pRule);
      pInterfaceRuleCriteria->AttachParent(rule_criterias, true);
      pInterfaceRuleCriteria->AddRef();
      *pVal = pInterfaceRuleCriteria;   
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleCriterias::get_Item(long lIndex, IInterfaceRuleCriteria** pVal)
{
   try
   {
      if (!rule_criterias)
         return GetAccessDenied();

      std::shared_ptr<HM::RuleCriteria> pRule = rule_criterias->GetItem(lIndex);
      if (!pRule)
         return COMError::WithoutMessage(DISP_E_BADINDEX);
   
      CComObject<InterfaceRuleCriteria>* pInterfaceRuleCriteria = new CComObject<InterfaceRuleCriteria>();
      pInterfaceRuleCriteria->SetAuthentication(authentication_);
      pInterfaceRuleCriteria->AttachItem(pRule);
      pInterfaceRuleCriteria->AttachParent(rule_criterias, true);
   
      pInterfaceRuleCriteria->AddRef();
      *pVal = pInterfaceRuleCriteria;   
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleCriterias::get_Count(LONG* pVal)
{
   try
   {
      if (!rule_criterias)
         return GetAccessDenied();

      *pVal = rule_criterias->GetCount();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleCriterias::Add(IInterfaceRuleCriteria** pVal)
{
   try
   {
      if (!rule_criterias)
         return GetAccessDenied();

      if (!rule_criterias)
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceRuleCriteria>* pIntDA = new CComObject<InterfaceRuleCriteria>();
      pIntDA->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::RuleCriteria> pDA = std::shared_ptr<HM::RuleCriteria>(new HM::RuleCriteria);
   
      pDA->SetRuleID(rule_criterias->GetRuleID());
   
      pIntDA->AttachItem(pDA);
      pIntDA->AttachParent(rule_criterias, false);
      pIntDA->AddRef();
   
      *pVal = pIntDA;
   
      return S_OK;   
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleCriterias::DeleteByDBID(LONG DBID)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!rule_criterias)
         return GetAccessDenied();

      if (!authentication_->MayChangeRule(rule_criterias->GetRuleID()))
         return COMError::GenerateError(HM::COMAuthentication::NotTakenIn);

      rule_criterias->DeleteItemByDBID(DBID);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleCriterias::Delete(LONG DBID)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!rule_criterias)
         return GetAccessDenied();

      if (!authentication_->MayChangeRule(rule_criterias->GetRuleID()))
         return COMError::GenerateError(HM::COMAuthentication::NotTakenIn);

      rule_criterias->DeleteItem(DBID);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRuleCriterias::Refresh(void)
{
   try
   {
      if (!rule_criterias)
         return GetAccessDenied();

      rule_criterias->Refresh();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


