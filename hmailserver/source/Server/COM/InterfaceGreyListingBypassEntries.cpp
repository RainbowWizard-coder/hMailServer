// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "COMError.h"
#include "../Common/Util/AuditTrail.h"
#include "InterfaceGreyListingBypassEntries.h"

#include "../Common/BO/GreyListingBypassEntry.h"
#include "InterfaceGreyListingBypassEntry.h"

void
InterfaceGreyListingBypassEntries::Attach(std::shared_ptr<HM::GreyListingBypassEntries> pEntries)
{
   entries_ = pEntries;
}

STDMETHODIMP
InterfaceGreyListingBypassEntries::Refresh()
{
   try
   {
      if (!entries_)
         return GetAccessDenied();

      entries_->Refresh();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceGreyListingBypassEntries::get_Count(long *pVal)
{
   try
   {
      if (!entries_)
         return GetAccessDenied();

      *pVal = entries_->GetCount();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceGreyListingBypassEntries::get_Item(long Index, IInterfaceGreyListingBypassEntry **pVal)
{
   try
   {
      if (!entries_)
         return GetAccessDenied();

      CComObject<InterfaceGreyListingBypassEntry>* pInterface = new CComObject<InterfaceGreyListingBypassEntry>();
      pInterface->SetAuthentication(authentication_);

      std::shared_ptr<HM::GreyListingBypassEntry> pEntry = entries_->GetItem(Index);

      if (!pEntry)
         return COMError::WithoutMessage(DISP_E_BADINDEX);

      pInterface->AttachItem(pEntry);
      pInterface->AttachParent(entries_, true);
      pInterface->AddRef();
      *pVal = pInterface;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceGreyListingBypassEntries::DeleteByDBID(long DBID)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!entries_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      entries_->DeleteItemByDBID(DBID);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceGreyListingBypassEntries::get_ItemByDBID(long lDBID, IInterfaceGreyListingBypassEntry **pVal)
{
   try
   {
      if (!entries_)
         return GetAccessDenied();

      CComObject<InterfaceGreyListingBypassEntry>* pInterface = new CComObject<InterfaceGreyListingBypassEntry>();
      pInterface->SetAuthentication(authentication_);

      std::shared_ptr<HM::GreyListingBypassEntry> pEntry = entries_->GetItemByDBID(lDBID);

      if (!pEntry)
         return COMError::WithoutMessage(DISP_E_BADINDEX);

      pInterface->AttachItem(pEntry);
      pInterface->AttachParent(entries_, true);
      pInterface->AddRef();

      *pVal = pInterface;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceGreyListingBypassEntries::get_ItemByName(BSTR sName, IInterfaceGreyListingBypassEntry **pVal)
{
   try
   {
      if (!entries_)
         return GetAccessDenied();

      CComObject<InterfaceGreyListingBypassEntry>* pInterface = new CComObject<InterfaceGreyListingBypassEntry>();
      pInterface->SetAuthentication(authentication_);

      // The pattern is stored lower-cased, so it is looked up that way.
      HM::String pattern = sName;
      pattern.Trim();
      pattern.ToLower();

      std::shared_ptr<HM::GreyListingBypassEntry> pEntry = entries_->GetItemByName(pattern);

      if (!pEntry)
         return COMError::WithoutMessage(DISP_E_BADINDEX);

      pInterface->AttachItem(pEntry);
      pInterface->AttachParent(entries_, true);
      pInterface->AddRef();

      *pVal = pInterface;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceGreyListingBypassEntries::Add(IInterfaceGreyListingBypassEntry **pVal)
{
   try
   {
      if (!entries_)
         return GetAccessDenied();

      CComObject<InterfaceGreyListingBypassEntry>* pInterface = new CComObject<InterfaceGreyListingBypassEntry>();
      pInterface->SetAuthentication(authentication_);

      std::shared_ptr<HM::GreyListingBypassEntry> pEntry = std::shared_ptr<HM::GreyListingBypassEntry>(new HM::GreyListingBypassEntry);

      pInterface->AttachItem(pEntry);
      pInterface->AttachParent(entries_, false);
      pInterface->AddRef();

      *pVal = pInterface;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}
