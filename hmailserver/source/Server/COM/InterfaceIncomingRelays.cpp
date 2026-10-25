// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"
#include "COMError.h"
#include "../Common/Util/AuditTrail.h"
#include "InterfaceIncomingRelays.h"

#include "../Common/Persistence/PersistentIncomingRelay.h"
#include "InterfaceIncomingRelay.h"

void 
InterfaceIncomingRelays::Attach(std::shared_ptr<HM::IncomingRelays> incomingRelays)
{
   incoming_relays_ = incomingRelays;
}

STDMETHODIMP 
InterfaceIncomingRelays::get_Count(long *pVal)
{
   try
   {
      if (!incoming_relays_)
         return GetAccessDenied();

      *pVal = incoming_relays_->GetCount();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceIncomingRelays::Delete(long Index)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!incoming_relays_)
         return GetAccessDenied();

      incoming_relays_->DeleteItem(Index);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceIncomingRelays::DeleteByDBID(long DBID)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!incoming_relays_)
         return GetAccessDenied();

      incoming_relays_->DeleteItemByDBID(DBID);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIncomingRelays::Refresh()
{
   try
   {
      if (!incoming_relays_)
         return GetAccessDenied();

      incoming_relays_->Refresh();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIncomingRelays::get_Item(long Index, IInterfaceIncomingRelay **pVal)
{
   try
   {
      if (!incoming_relays_)
         return GetAccessDenied();

      std::shared_ptr<HM::IncomingRelay> pRange = incoming_relays_->GetItem(Index);
   
      if (!pRange)
         return COMError::WithoutMessage(DISP_E_BADINDEX);
   
      CComObject<InterfaceIncomingRelay>* pRangeInt = new CComObject<InterfaceIncomingRelay>();
      pRangeInt->SetAuthentication(authentication_);
      pRangeInt->AttachItem(pRange);
      pRangeInt->AttachSharedParent(incoming_relays_, true);
      pRangeInt->AddRef();
      *pVal = pRangeInt;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIncomingRelays::get_ItemByDBID(long DBID, IInterfaceIncomingRelay **pVal)
{
   try
   {
      if (!incoming_relays_)
         return GetAccessDenied();

      std::shared_ptr<HM::IncomingRelay> pRange = incoming_relays_->GetItemByDBID(DBID);
   
      if (!pRange)
         return COMError::WithoutMessage(DISP_E_BADINDEX);
   
      CComObject<InterfaceIncomingRelay>* pRangeInt = new CComObject<InterfaceIncomingRelay>();
      pRangeInt->SetAuthentication(authentication_);
      pRangeInt->AttachItem(pRange);
      pRangeInt->AttachSharedParent(incoming_relays_, true);
      pRangeInt->AddRef();
      *pVal = pRangeInt;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIncomingRelays::Add(IInterfaceIncomingRelay **pVal)
{
   try
   {
      if (!incoming_relays_)
         return GetAccessDenied();

      if (!incoming_relays_)
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceIncomingRelay>* pInterfaceRange = new CComObject<InterfaceIncomingRelay>();
      pInterfaceRange->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::IncomingRelay> pRange = std::shared_ptr<HM::IncomingRelay>(new HM::IncomingRelay); 
   
      pInterfaceRange->AttachItem(pRange);
      pInterfaceRange->AttachSharedParent(incoming_relays_, false);
   
      pInterfaceRange->AddRef();
      *pVal = pInterfaceRange;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIncomingRelays::get_ItemByName(BSTR sName, IInterfaceIncomingRelay **pVal)
{
   try
   {
      if (!incoming_relays_)
         return GetAccessDenied();

      std::shared_ptr<HM::IncomingRelay> pRange = incoming_relays_->GetItemByName(sName);
   
      if (!pRange)
         return COMError::WithoutMessage(DISP_E_BADINDEX);
   
      CComObject<InterfaceIncomingRelay>* pRangeInt = new CComObject<InterfaceIncomingRelay>();
      pRangeInt->SetAuthentication(authentication_);
      pRangeInt->AttachItem(pRange);
      pRangeInt->AttachSharedParent(incoming_relays_, true);
      pRangeInt->AddRef();
      *pVal = pRangeInt;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


