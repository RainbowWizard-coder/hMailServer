// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../hMailServer/resource.h"       // main symbols
#include "../hMailServer/hMailServer.h"

namespace HM
{
   class SenderListEntries;
}


#if defined(_WIN32_WCE) && !defined(_CE_DCOM) && !defined(_CE_ALLOW_SINGLE_THREADED_OBJECTS_IN_MTA)
#error "Single-threaded COM objects are not properly supported on Windows CE platform, such as the Windows Mobile platforms that do not include full DCOM support. Define _CE_ALLOW_SINGLE_THREADED_OBJECTS_IN_MTA to force ATL to support creating single-thread COM object's and allow use of it's single-threaded COM object implementations. The threading model in your rgs file was set to 'Free' as that is the only threading model supported in non DCOM Windows CE platforms."
#endif



// InterfaceSenderListEntries
//
// One scope's allow and block list. The same coclass serves the server's list
// (reached as Settings.AntiSpam.SenderList) and a domain's (reached as
// Domain.SenderList), because they are the same rows at different scopes; the
// collection it is attached to carries the scope, and Add returns an entry that
// already belongs to it.

class ATL_NO_VTABLE InterfaceSenderListEntries :
   public CComObjectRootEx<CComSingleThreadModel>,
   public CComCoClass<InterfaceSenderListEntries, &CLSID_SenderListEntries>,
   public IDispatchImpl<IInterfaceSenderListEntries, &IID_IInterfaceSenderListEntries, &LIBID_hMailServer, /*wMajor =*/ 1, /*wMinor =*/ 0>,
   public HM::COMAuthenticator
{
public:
   InterfaceSenderListEntries()
   {
   }

DECLARE_REGISTRY_RESOURCEID(IDR_INTERFACESENDERLISTENTRIES)


BEGIN_COM_MAP(InterfaceSenderListEntries)
   COM_INTERFACE_ENTRY(IInterfaceSenderListEntries)
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
   STDMETHOD(Refresh)();
   STDMETHOD(Clear)();

   STDMETHOD(get_Item)(/*[in]*/ long Index, /*[out, retval]*/ IInterfaceSenderListEntry **pVal);
   STDMETHOD(get_Count)(/*[out, retval]*/ long *pVal);
   STDMETHOD(get_ItemByDBID)(/*[in]*/ long lDBID, /*[out, retval]*/ IInterfaceSenderListEntry** pVal);
   STDMETHOD(get_ItemByName)(/*[in]*/ BSTR sName, /*[out, retval]*/ IInterfaceSenderListEntry** pVal);
   STDMETHOD(DeleteByDBID)(/*[in]*/ long DBID);
   STDMETHOD(Add)(/*[out, retval]*/ IInterfaceSenderListEntry **pVal);

   void Attach(std::shared_ptr<HM::SenderListEntries> pEntries);

public:

   std::shared_ptr<HM::SenderListEntries> object_;

};

OBJECT_ENTRY_AUTO(__uuidof(SenderListEntries), InterfaceSenderListEntries)
