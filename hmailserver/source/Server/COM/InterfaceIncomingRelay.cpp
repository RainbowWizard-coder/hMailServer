// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "InterfaceIncomingRelay.h"
#include "../Common/Persistence/PersistentIncomingRelay.h"
#include "../Common/BO/IncomingRelays.h"
#include "../Common/BO/IncomingRelay.h"

#include "COMError.h"

STDMETHODIMP InterfaceIncomingRelay::Save()
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

      if (HM::PersistentIncomingRelay::SaveObject(object_))
      {
         // A new item joins the running collection; a changed one - this
         // object's copy - reaches it by the collection being read again
         // (COMCollection.h).
         PublishSaved_("InterfaceIncomingRelay::Save");
   
         return S_OK;
      }
   
      return COMError::GenerateError("Failed to save object. See hMailServer error log.");
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIncomingRelay::get_ID(long *pVal)
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

STDMETHODIMP InterfaceIncomingRelay::get_LowerIP(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetLowerIPString().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIncomingRelay::put_LowerIP(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetLowerIPString(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIncomingRelay::get_UpperIP(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetUpperIPString().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIncomingRelay::put_UpperIP(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetUpperIPString(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIncomingRelay::get_Name(BSTR *pVal)
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

STDMETHODIMP InterfaceIncomingRelay::put_Name(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetName(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIncomingRelay::Delete()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      if (!parent_collection_)
         return HM::PersistentIncomingRelay::DeleteObject(object_) ? S_OK : S_FALSE;
   
      parent_collection_->DeleteItemByDBID(object_->GetID());
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

