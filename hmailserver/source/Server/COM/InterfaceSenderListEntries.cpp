// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "COMError.h"
#include "InterfaceSenderListEntries.h"

#include "InterfaceSenderListEntry.h"

#include "../Common/BO/SenderListEntry.h"
#include "../Common/BO/SenderListEntries.h"

void
InterfaceSenderListEntries::Attach(std::shared_ptr<HM::SenderListEntries> pEntries)
{
   object_ = pEntries;
}

STDMETHODIMP
InterfaceSenderListEntries::Refresh()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->Refresh();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceSenderListEntries::Clear()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->DeleteAll();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSenderListEntries::get_Count(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetCount();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceSenderListEntries::get_Item(long Index, IInterfaceSenderListEntry **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      std::shared_ptr<HM::SenderListEntry> entry = object_->GetItem(Index);

      if (!entry)
         return DISP_E_BADINDEX;

      CComObject<InterfaceSenderListEntry>* pInterface = new CComObject<InterfaceSenderListEntry>();
      pInterface->SetAuthentication(authentication_);

      pInterface->AttachItem(entry);
      pInterface->AttachParent(object_, true);
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
InterfaceSenderListEntries::DeleteByDBID(long DBID)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->DeleteItemByDBID(DBID);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceSenderListEntries::get_ItemByDBID(long lDBID, IInterfaceSenderListEntry **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      std::shared_ptr<HM::SenderListEntry> entry = object_->GetItemByDBID(lDBID);

      if (!entry)
         return DISP_E_BADINDEX;

      CComObject<InterfaceSenderListEntry>* pInterface = new CComObject<InterfaceSenderListEntry>();
      pInterface->SetAuthentication(authentication_);

      pInterface->AttachItem(entry);
      pInterface->AttachParent(object_, true);
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
InterfaceSenderListEntries::get_ItemByName(BSTR sName, IInterfaceSenderListEntry **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      std::shared_ptr<HM::SenderListEntry> entry = object_->GetItemByName(sName);

      if (!entry)
         return DISP_E_BADINDEX;

      CComObject<InterfaceSenderListEntry>* pInterface = new CComObject<InterfaceSenderListEntry>();
      pInterface->SetAuthentication(authentication_);

      pInterface->AttachItem(entry);
      pInterface->AttachParent(object_, true);
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
InterfaceSenderListEntries::Add(IInterfaceSenderListEntry **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      CComObject<InterfaceSenderListEntry>* pInterface = new CComObject<InterfaceSenderListEntry>();
      pInterface->SetAuthentication(authentication_);

      // CreateEntry rather than a bare new: the collection knows its own scope
      // and owner, and an entry that had to be told them would be an entry a
      // caller could forget to tell - which is a row nobody's list contains and
      // nothing ever matches.
      std::shared_ptr<HM::SenderListEntry> entry = object_->CreateEntry();

      pInterface->AttachItem(entry);
      pInterface->AttachParent(object_, false);
      pInterface->AddRef();

      *pVal = pInterface;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}
