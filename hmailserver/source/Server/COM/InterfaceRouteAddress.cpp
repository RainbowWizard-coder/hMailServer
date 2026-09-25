// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "InterfaceRouteAddress.h"
#include "InterfaceRouteAddresses.h"

#include "../Common/Persistence/PersistentRouteAddress.h"
#include "../Common/BO/RouteAddresses.h"
#include "../Common/BO/RouteAddress.h"

#include "COMError.h"
#include "../Common/Util/AuditTrail.h"

STDMETHODIMP 
InterfaceRouteAddress::get_ID(long *pVal)
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

STDMETHODIMP 
InterfaceRouteAddress::get_RouteID(long *pVal)
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

STDMETHODIMP 
InterfaceRouteAddress::put_RouteID(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetRouteID(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceRouteAddress::get_Address(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
   
      *pVal = object_->GetAddress().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceRouteAddress::put_Address(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetAddress(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceRouteAddress::Save()
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!object_)
         return GetAccessDenied();

      // Onto a copy even when nothing was put before it: the running item is
      // never what the persister is handed, and neither is one a refresh has
      // replaced since this object was given it (COMCollection.h).
      Writable_();

      if (HM::PersistentRouteAddress::SaveObject(object_))
      {
         // A new item joins the running collection; a changed one - this
         // object's copy - reaches it by the collection being read again
         // (COMCollection.h).
         PublishSaved_("InterfaceRouteAddress::Save");

         // And into the running route, when the list this address came from
         // is one a refresh of the routes has retired since it was read.
         InterfaceRouteAddresses::ReachRunningRoute(parent_collection_);
   
         return S_OK;
      }
   
      return COMError::GenerateError("Failed to save object. See hMailServer error log.");
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRouteAddress::Delete()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      if (!parent_collection_)
         return HM::PersistentRouteAddress::DeleteObject(object_) ? S_OK : S_FALSE;
   
      parent_collection_->DeleteItemByDBID(object_->GetID());
      InterfaceRouteAddresses::ReachRunningRoute(parent_collection_);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

