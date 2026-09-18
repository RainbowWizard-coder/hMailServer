// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../hMailServer/resource.h"       // main symbols
#include "../hMailServer/hMailServer.h"

#include "../Common/BO/GreyListingBypassEntries.h"

#if defined(_WIN32_WCE) && !defined(_CE_DCOM) && !defined(_CE_ALLOW_SINGLE_THREADED_OBJECTS_IN_MTA)
#error "Single-threaded COM objects are not properly supported on Windows CE platform, such as the Windows Mobile platforms that do not include full DCOM support. Define _CE_ALLOW_SINGLE_THREADED_OBJECTS_IN_MTA to force ATL to support creating single-thread COM object's and allow use of it's single-threaded COM object implementations. The threading model in your rgs file was set to 'Free' as that is the only threading model supported in non DCOM Windows CE platforms."
#endif

// InterfaceGreyListingBypassEntries: the per-domain greylisting bypass list
// (AntiSpam.GreyListingBypassEntries), in the greylisting white list's shape.

class ATL_NO_VTABLE InterfaceGreyListingBypassEntries :
   public CComObjectRootEx<CComSingleThreadModel>,
   public CComCoClass<InterfaceGreyListingBypassEntries, &CLSID_GreyListingBypassEntries>,
   public ISupportErrorInfo,
   public IDispatchImpl<IInterfaceGreyListingBypassEntries, &IID_IInterfaceGreyListingBypassEntries, &LIBID_hMailServer, /*wMajor =*/ 1, /*wMinor =*/ 0>,
   public HM::COMAuthenticator
{
public:
   InterfaceGreyListingBypassEntries()
   {
   }

DECLARE_REGISTRY_RESOURCEID(IDR_INTERFACEGREYLISTINGBYPASSENTRIES)


BEGIN_COM_MAP(InterfaceGreyListingBypassEntries)
   COM_INTERFACE_ENTRY(IInterfaceGreyListingBypassEntries)
   COM_INTERFACE_ENTRY(IDispatch)
   COM_INTERFACE_ENTRY(ISupportErrorInfo)
END_COM_MAP()

   STDMETHOD(InterfaceSupportsErrorInfo)(REFIID riid)
   {
      return riid == IID_IInterfaceGreyListingBypassEntries ? S_OK : S_FALSE;
   }

   DECLARE_PROTECT_FINAL_CONSTRUCT()

   HRESULT FinalConstruct()
   {
      return S_OK;
   }

   void FinalRelease()
   {
   }

public:
   STDMETHOD(Refresh)();

   STDMETHOD(get_Item)(/*[in]*/ long Index, /*[out, retval]*/ IInterfaceGreyListingBypassEntry **pVal);
   STDMETHOD(get_Count)(/*[out, retval]*/ long *pVal);
   STDMETHOD(get_ItemByDBID)(/*[in]*/ long lDBID, /*[out, retval]*/ IInterfaceGreyListingBypassEntry** pVal);
   STDMETHOD(get_ItemByName)(/*[in]*/ BSTR sName, /*[out, retval]*/ IInterfaceGreyListingBypassEntry** pVal);
   STDMETHOD(DeleteByDBID)(/*[in]*/ long DBID);
   STDMETHOD(Add)(/*[out, retval]*/ IInterfaceGreyListingBypassEntry **pVal);

   void Attach(std::shared_ptr<HM::GreyListingBypassEntries> pEntries);

public:

   std::shared_ptr<HM::GreyListingBypassEntries> entries_;

};

OBJECT_ENTRY_AUTO(__uuidof(GreyListingBypassEntries), InterfaceGreyListingBypassEntries)
