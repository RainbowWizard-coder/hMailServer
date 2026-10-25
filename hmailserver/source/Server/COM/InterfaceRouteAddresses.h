// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once
#include "../hMailServer/resource.h"       // main symbols

#include "../hMailServer/hMailServer.h"

#include "../Common/BO/RouteAddresses.h"

class ATL_NO_VTABLE InterfaceRouteAddresses : 
   public CComObjectRootEx<CComSingleThreadModel>,
   public CComCoClass<InterfaceRouteAddresses, &CLSID_RouteAddresses>,
   public ISupportErrorInfo,
   public IDispatchImpl<IInterfaceRouteAddresses, &IID_IInterfaceRouteAddresses, &LIBID_hMailServer, /*wMajor =*/ 1, /*wMinor =*/ 0>,
   public HM::COMAuthenticator
{
public:
   InterfaceRouteAddresses()
   {
   }

DECLARE_REGISTRY_RESOURCEID(IDR_INTERFACEROUTEADDRESSES)


BEGIN_COM_MAP(InterfaceRouteAddresses)
   COM_INTERFACE_ENTRY(IInterfaceRouteAddresses)
   COM_INTERFACE_ENTRY(IDispatch)
   COM_INTERFACE_ENTRY(ISupportErrorInfo)
END_COM_MAP()

   STDMETHOD(InterfaceSupportsErrorInfo)(REFIID riid)
   {
      return riid == IID_IInterfaceRouteAddresses ? S_OK : S_FALSE;
   }


   DECLARE_PROTECT_FINAL_CONSTRUCT()

   HRESULT FinalConstruct()
   {
      return S_OK;
   }
   
   void FinalRelease() 
   {
   }

   STDMETHOD(get_Item)(/*[in]*/ long Index, /*[out, retval]*/ IInterfaceRouteAddress **pVal);
   STDMETHOD(get_ItemByDBID)(/*[in]*/ long lDBID, /*[out, retval]*/ IInterfaceRouteAddress **pVal);
   STDMETHOD(get_Count)(/*[out, retval]*/ long *pVal);

   STDMETHOD(DeleteByDBID)(/*[in]*/ long DBID);
   STDMETHOD(DeleteByAddress)(/*[in]*/ BSTR sAddress);
   STDMETHOD(Add)(/*[out, retval]*/ IInterfaceRouteAddress **pVal);

   void Attach(std::shared_ptr<HM::RouteAddresses> pRouteAddresses);

   // After an address is saved or deleted through list: when list is not the
   // running route's own - it was read from the route before a refresh of the
   // routes (a route's COM Save, Routes.Refresh, a REST update) replaced the
   // route - the running route's list is read again, so the change is in
   // effect as it would have been through the running list. The list itself
   // still reads as it was, with its own change; Addresses read again from
   // the route is the running list.
   static void ReachRunningRoute(std::shared_ptr<HM::RouteAddresses> list);

public:

   std::shared_ptr<HM::RouteAddresses> route_addresses_;

};

OBJECT_ENTRY_AUTO(__uuidof(RouteAddresses), InterfaceRouteAddresses)
