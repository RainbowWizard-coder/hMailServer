// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "../hMailServer/resource.h"       // main symbols

#include "../hMailServer/hMailServer.h"

#include "COMCollection.h"

#include "../Common/BO/GreyListingBypassEntry.h"
#include "../Common/BO/GreyListingBypassEntries.h"

#if defined(_WIN32_WCE) && !defined(_CE_DCOM) && !defined(_CE_ALLOW_SINGLE_THREADED_OBJECTS_IN_MTA)
#error "Single-threaded COM objects are not properly supported on Windows CE platform, such as the Windows Mobile platforms that do not include full DCOM support. Define _CE_ALLOW_SINGLE_THREADED_OBJECTS_IN_MTA to force ATL to support creating single-thread COM object's and allow use of it's single-threaded COM object implementations. The threading model in your rgs file was set to 'Free' as that is the only threading model supported in non DCOM Windows CE platforms."
#endif

// InterfaceGreyListingBypassEntry: one entry of the greylisting bypass list.

class ATL_NO_VTABLE InterfaceGreyListingBypassEntry :
   public COMCollectionItem<HM::GreyListingBypassEntry, HM::GreyListingBypassEntries>,
   public CComObjectRootEx<CComSingleThreadModel>,
   public CComCoClass<InterfaceGreyListingBypassEntry, &CLSID_GreyListingBypassEntry>,
   public IDispatchImpl<IInterfaceGreyListingBypassEntry, &IID_IInterfaceGreyListingBypassEntry, &LIBID_hMailServer, /*wMajor =*/ 1, /*wMinor =*/ 0>,
   public ISupportErrorInfo,
   public HM::COMAuthenticator
{
public:
   InterfaceGreyListingBypassEntry()
   {
   }

DECLARE_REGISTRY_RESOURCEID(IDR_INTERFACEGREYLISTINGBYPASSENTRY)


BEGIN_COM_MAP(InterfaceGreyListingBypassEntry)
   COM_INTERFACE_ENTRY(IInterfaceGreyListingBypassEntry)
   COM_INTERFACE_ENTRY(IDispatch)
   COM_INTERFACE_ENTRY(ISupportErrorInfo)
END_COM_MAP()

   STDMETHOD(InterfaceSupportsErrorInfo)(REFIID riid)
   {
      return riid == IID_IInterfaceGreyListingBypassEntry ? S_OK : S_FALSE;
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
   STDMETHOD(Save)();
   STDMETHOD(Delete)();
   STDMETHOD(get_ID)(/*[out, retval]*/ long *pVal);

   STDMETHOD(get_Pattern)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_Pattern)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_MatchOn)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_MatchOn)(/*[in]*/ long newVal);
   STDMETHOD(get_RequireSPF)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_RequireSPF)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_RequireDKIM)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_RequireDKIM)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_RequireDMARC)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_RequireDMARC)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_Note)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_Note)(/*[in]*/ BSTR newVal);

private:

   HRESULT GetRequireFlag_(int flag, VARIANT_BOOL *pVal);
   HRESULT PutRequireFlag_(int flag, VARIANT_BOOL newVal);
};

OBJECT_ENTRY_AUTO(__uuidof(GreyListingBypassEntry), InterfaceGreyListingBypassEntry)
