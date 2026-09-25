// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "InterfaceDNSBlackList.h"

#include "../Common/Persistence/PersistentDNSBlacklist.h"

#include "../Common/BO/DNSBlackLists.h"

#include "COMError.h"

STDMETHODIMP 
InterfaceDNSBlackList::Save()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      // Onto a copy even when nothing was put before it: the running item is
      // never what the persister is handed, and neither is one a refresh has
      // replaced since this object was given it (COMCollection.h).
      Writable_();

      if (HM::PersistentDNSBlackList::SaveObject(object_))
      {
         // A new item joins the running collection; a changed one - this
         // object's copy - reaches it by the collection being read again
         // (COMCollection.h).
         PublishSaved_("InterfaceDNSBlackList::Save");
         return S_OK;
      }
   
      return COMError::GenerateError("Failed to save object. See hMailServer error log.");
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDNSBlackList::get_Active(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetIsActive() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDNSBlackList::put_Active(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetIsActive(newVal == VARIANT_TRUE);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDNSBlackList::get_ID(long *pVal)
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

STDMETHODIMP InterfaceDNSBlackList::put_DNSHost(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetDNSHost(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDNSBlackList::get_DNSHost(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetDNSHost().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDNSBlackList::put_RejectMessage(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetRejectMessage(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDNSBlackList::get_RejectMessage(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetRejectMessage().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDNSBlackList::put_ExpectedResult(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetExpectedResult(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDNSBlackList::get_ExpectedResult(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetExpectedResult().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDNSBlackList::put_Score(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetScore(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDNSBlackList::get_Score(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetScore();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDNSBlackList::Delete()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      if (!parent_collection_)
         return HM::PersistentDNSBlackList::DeleteObject(object_) ? S_OK : S_FALSE;
   
      parent_collection_->DeleteItemByDBID(object_->GetID());
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

