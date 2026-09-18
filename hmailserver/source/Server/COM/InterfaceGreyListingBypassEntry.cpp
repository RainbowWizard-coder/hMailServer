// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "COMError.h"
#include "InterfaceGreyListingBypassEntry.h"

#include "../Common/Persistence/PersistentGreyListingBypassEntry.h"

STDMETHODIMP
InterfaceGreyListingBypassEntry::Save()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      // The persistence layer's refusal - an empty pattern, one that is not a
      // domain, a row requiring nothing - reaches the caller as a sentence.
      HM::String result;
      if (HM::PersistentGreyListingBypassEntry::SaveObject(object_, result, HM::PersistenceModeNormal))
      {
         AddToParentCollection();

         return S_OK;
      }

      return COMError::GenerateError(result);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceGreyListingBypassEntry::get_ID(long *pVal)
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

STDMETHODIMP InterfaceGreyListingBypassEntry::put_Pattern(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetPattern(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceGreyListingBypassEntry::get_Pattern(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetPattern().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceGreyListingBypassEntry::put_MatchOn(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetMatchOn((int) newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceGreyListingBypassEntry::get_MatchOn(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetMatchOn();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

HRESULT
InterfaceGreyListingBypassEntry::GetRequireFlag_(int flag, VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (object_->GetRequire() & flag) != 0 ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

HRESULT
InterfaceGreyListingBypassEntry::PutRequireFlag_(int flag, VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      int require = object_->GetRequire();
      if (newVal == VARIANT_TRUE)
         require |= flag;
      else
         require &= ~flag;

      object_->SetRequire(require);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceGreyListingBypassEntry::get_RequireSPF(VARIANT_BOOL *pVal)
{
   return GetRequireFlag_(HM::GreyListingBypassEntry::RequireSpf, pVal);
}

STDMETHODIMP InterfaceGreyListingBypassEntry::put_RequireSPF(VARIANT_BOOL newVal)
{
   return PutRequireFlag_(HM::GreyListingBypassEntry::RequireSpf, newVal);
}

STDMETHODIMP InterfaceGreyListingBypassEntry::get_RequireDKIM(VARIANT_BOOL *pVal)
{
   return GetRequireFlag_(HM::GreyListingBypassEntry::RequireDkim, pVal);
}

STDMETHODIMP InterfaceGreyListingBypassEntry::put_RequireDKIM(VARIANT_BOOL newVal)
{
   return PutRequireFlag_(HM::GreyListingBypassEntry::RequireDkim, newVal);
}

STDMETHODIMP InterfaceGreyListingBypassEntry::get_RequireDMARC(VARIANT_BOOL *pVal)
{
   return GetRequireFlag_(HM::GreyListingBypassEntry::RequireDmarc, pVal);
}

STDMETHODIMP InterfaceGreyListingBypassEntry::put_RequireDMARC(VARIANT_BOOL newVal)
{
   return PutRequireFlag_(HM::GreyListingBypassEntry::RequireDmarc, newVal);
}

STDMETHODIMP InterfaceGreyListingBypassEntry::put_Note(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetNote(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceGreyListingBypassEntry::get_Note(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetNote().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceGreyListingBypassEntry::Delete()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      if (!parent_collection_)
         return HM::PersistentGreyListingBypassEntry::DeleteObject(object_) ? S_OK : S_FALSE;

      parent_collection_->DeleteItemByDBID(object_->GetID());

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}
