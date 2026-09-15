// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../hMailServer/resource.h"       // main symbols
#include "../hMailServer/hMailServer.h"

#include "COMCollection.h"

#include "../Common/BO/SenderListEntry.h"
#include "../Common/BO/SenderListEntries.h"

#if defined(_WIN32_WCE) && !defined(_CE_DCOM) && !defined(_CE_ALLOW_SINGLE_THREADED_OBJECTS_IN_MTA)
#error "Single-threaded COM objects are not properly supported on Windows CE platform, such as the Windows Mobile platforms that do not include full DCOM support. Define _CE_ALLOW_SINGLE_THREADED_OBJECTS_IN_MTA to force ATL to support creating single-thread COM object's and allow use of it's single-threaded COM object implementations. The threading model in your rgs file was set to 'Free' as that is the only threading model supported in non DCOM Windows CE platforms."
#endif


// InterfaceSenderListEntry

class ATL_NO_VTABLE InterfaceSenderListEntry :
   public COMCollectionItem<HM::SenderListEntry, HM::SenderListEntries>,
   public CComObjectRootEx<CComSingleThreadModel>,
   public CComCoClass<InterfaceSenderListEntry, &CLSID_SenderListEntry>,
   public IDispatchImpl<IInterfaceSenderListEntry, &IID_IInterfaceSenderListEntry, &LIBID_hMailServer, /*wMajor =*/ 1, /*wMinor =*/ 0>,
   public HM::COMAuthenticator
{
public:
   InterfaceSenderListEntry()
   {
   }

DECLARE_REGISTRY_RESOURCEID(IDR_INTERFACESENDERLISTENTRY)


BEGIN_COM_MAP(InterfaceSenderListEntry)
   COM_INTERFACE_ENTRY(IInterfaceSenderListEntry)
   COM_INTERFACE_ENTRY(IDispatch)
END_COM_MAP()



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

   STDMETHOD(get_Value)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_Value)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_Blocked)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_Blocked)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_Reason)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_Reason)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_Expires)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_Expires)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_ExpiresTime)(/*[out, retval]*/ VARIANT *pVal);
   STDMETHOD(put_ExpiresTime)(/*[in]*/ VARIANT newVal);
   STDMETHOD(get_Scope)(/*[out, retval]*/ long *pVal);

};

OBJECT_ENTRY_AUTO(__uuidof(SenderListEntry), InterfaceSenderListEntry)
