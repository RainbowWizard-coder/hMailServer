// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "../hMailServer/resource.h"       // main symbols

#include "../hMailServer/hMailServer.h"

// InterfaceEventLog

class ATL_NO_VTABLE InterfaceEventLog : 
   public CComObjectRootEx<CComSingleThreadModel>,
   public CComCoClass<InterfaceEventLog, &CLSID_EventLog>,
   public ISupportErrorInfo,
   public IDispatchImpl<IInterfaceEventLog, &IID_IInterfaceEventLog, &LIBID_hMailServer, /*wMajor =*/ 1, /*wMinor =*/ 0>
{
public:
   InterfaceEventLog()
   {
   }

DECLARE_REGISTRY_RESOURCEID(IDR_INTERFACEEVENTLOG)


BEGIN_COM_MAP(InterfaceEventLog)
   COM_INTERFACE_ENTRY(IInterfaceEventLog)
   COM_INTERFACE_ENTRY(IDispatch)
   COM_INTERFACE_ENTRY(ISupportErrorInfo)
END_COM_MAP()

   STDMETHOD(InterfaceSupportsErrorInfo)(REFIID riid)
   {
      return riid == IID_IInterfaceEventLog ? S_OK : S_FALSE;
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
   
   STDMETHOD(Write)(BSTR sMessage);

};

OBJECT_ENTRY_AUTO(__uuidof(EventLog), InterfaceEventLog)
