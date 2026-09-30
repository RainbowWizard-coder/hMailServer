// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "COMError.h"
#include "../Common/Util/AuditTrail.h"
#include "InterfaceRouteAddresses.h"
#include "InterfaceRouteAddress.h"

#include "../Common/BO/Route.h"
#include "../Common/BO/Routes.h"
#include "../SMTP/SMTPConfiguration.h"

void
InterfaceRouteAddresses::Attach(std::shared_ptr<HM::RouteAddresses> pRouteAddresses)
{
   route_addresses_ = pRouteAddresses;
} 

void
InterfaceRouteAddresses::ReachRunningRoute(std::shared_ptr<HM::RouteAddresses> list)
{
   // The running route's list is found by the route's id, as a session finds
   // the route by its name, and read again only when it is another object:
   // the list InterfaceRoute::get_Addresses hands out is the running route's
   // while the route is running, and an add or a delete through it is in
   // effect already.
   if (!list || list->GetRouteID() == 0)
      return;

   std::shared_ptr<HM::Routes> routes = HM::Configuration::Instance()->GetSMTPConfiguration()->GetRoutes();
   if (!routes)
      return;

   std::shared_ptr<HM::Route> running = routes->GetItemByDBID(list->GetRouteID());
   if (!running)
      return;

   std::shared_ptr<HM::RouteAddresses> runningList = running->GetAddresses();
   if (runningList && runningList != list)
      runningList->Refresh();
}

STDMETHODIMP 
InterfaceRouteAddresses::get_Count(long *pVal)
{
   try
   {
      if (!route_addresses_)
         return GetAccessDenied();

      *pVal = route_addresses_->GetCount();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceRouteAddresses::get_Item(long Index, IInterfaceRouteAddress **pVal)
{
   try
   {
      if (!route_addresses_)
         return GetAccessDenied();

      std::shared_ptr<HM::RouteAddress> pRouteAddress = route_addresses_->GetItem(Index);
   
      if (!pRouteAddress)
         return COMError::WithoutMessage(DISP_E_BADINDEX);  
   
      CComObject<InterfaceRouteAddress>* pInterfaceRouteAddress = new CComObject<InterfaceRouteAddress>();
      pInterfaceRouteAddress->SetAuthentication(authentication_);
      pInterfaceRouteAddress->AttachItem(pRouteAddress);
      pInterfaceRouteAddress->AttachSharedParent(route_addresses_, true);
      pInterfaceRouteAddress->AddRef();
      *pVal = pInterfaceRouteAddress;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceRouteAddresses::DeleteByDBID(long DBID)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!route_addresses_)
         return GetAccessDenied();

      route_addresses_->DeleteItemByDBID(DBID);
      ReachRunningRoute(route_addresses_);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceRouteAddresses::DeleteByAddress(BSTR bstrAddress)
{
   try
   {
      if (!route_addresses_)
         return GetAccessDenied();

      HM::String sAddress = bstrAddress;
      route_addresses_->DeleteByAddress(sAddress);
      ReachRunningRoute(route_addresses_);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceRouteAddresses::Add(IInterfaceRouteAddress **pVal)
{
   try
   {
      if (!route_addresses_)
         return GetAccessDenied();

      if (!route_addresses_)
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceRouteAddress>* pRouteAddressInterface = new CComObject<InterfaceRouteAddress>();
      pRouteAddressInterface->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::RouteAddress> pRouteAddress = std::shared_ptr<HM::RouteAddress>(new HM::RouteAddress);
   
      // Set the ID of the new route.
      pRouteAddress->SetRouteID(route_addresses_->GetRouteID());
   
      pRouteAddressInterface->AttachSharedParent(route_addresses_, false);
      pRouteAddressInterface->AttachItem(pRouteAddress);
   
      pRouteAddressInterface->AddRef();
      *pVal = pRouteAddressInterface;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceRouteAddresses::get_ItemByDBID(long lDBID, IInterfaceRouteAddress **pVal)
{
   try
   {
      if (!route_addresses_)
         return GetAccessDenied();

      std::shared_ptr<HM::RouteAddress> pRouteAddress = route_addresses_->GetItemByDBID(lDBID);
   
      if (!pRouteAddress)
         return COMError::WithoutMessage(DISP_E_BADINDEX);
   
      CComObject<InterfaceRouteAddress>* pInterfaceRouteAddress = new CComObject<InterfaceRouteAddress>();
      pInterfaceRouteAddress->SetAuthentication(authentication_);
      pInterfaceRouteAddress->AttachItem(pRouteAddress);
      pInterfaceRouteAddress->AttachSharedParent(route_addresses_, true);
      pInterfaceRouteAddress->AddRef();
      *pVal = pInterfaceRouteAddress;

      return S_OK;
   
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


