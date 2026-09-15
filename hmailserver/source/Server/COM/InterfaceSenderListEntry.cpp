// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "COMError.h"

#include "InterfaceSenderListEntry.h"

#include "../Common/AntiSpam/SenderPattern.h"
#include "../Common/BO/SenderListEntry.h"
#include "../Common/Persistence/PersistentSenderListEntry.h"

// InterfaceSenderListEntry

STDMETHODIMP
InterfaceSenderListEntry::Save()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Server-admin only, as the whole COM surface for these lists is: the
      // account's own list is edited by its owner over /api/v1/me/sender-list,
      // where being signed in as that account is the authority. There is no
      // per-account COM path on purpose - an interface reached with the
      // administrator password has no account to be.
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      // Checked here as well as in the REST layer, because this is the other
      // door into the same table and a rule enforced at one door is not a rule.
      HM::String problem;
      if (!HM::SenderPattern::Validate(object_->GetValue(), problem))
         return COMError::GenerateError(problem);

      if (HM::PersistentSenderListEntry::SaveObject(object_))
         AddToParentCollection();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceSenderListEntry::Delete()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      if (!parent_collection_)
         return HM::PersistentSenderListEntry::DeleteObject(object_) ? S_OK : S_FALSE;

      parent_collection_->DeleteItemByDBID(object_->GetID());

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSenderListEntry::get_ID(long *pVal)
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

STDMETHODIMP InterfaceSenderListEntry::get_Value(BSTR *pVal)
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

STDMETHODIMP InterfaceSenderListEntry::put_Value(BSTR newVal)
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

STDMETHODIMP InterfaceSenderListEntry::get_Blocked(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetBlocked() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSenderListEntry::put_Blocked(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetBlocked(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSenderListEntry::get_Reason(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetReason().AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSenderListEntry::put_Reason(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetReason(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSenderListEntry::get_Expires(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetExpires() ? VARIANT_TRUE : VARIANT_FALSE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSenderListEntry::put_Expires(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetExpires(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSenderListEntry::get_ExpiresTime(VARIANT *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetExpiresTime().GetVariant();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSenderListEntry::put_ExpiresTime(VARIANT newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetExpiresTime(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceSenderListEntry::get_Scope(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Read-only. An entry's scope is decided by the collection it came out
      // of, and a scope that could be written would let a caller move a
      // domain's entry to the server's - which is a change of authority, not a
      // change of field.
      *pVal = (long) object_->GetScope();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}
