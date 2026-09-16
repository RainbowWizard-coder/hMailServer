// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "../hMailServer/resource.h"       // main symbols

#include "../hMailServer/hMailServer.h"


// InterfaceLanguage
namespace HM
{
   class Language;
}

class ATL_NO_VTABLE InterfaceLanguage : 
   public CComObjectRootEx<CComSingleThreadModel>,
   public CComCoClass<InterfaceLanguage, &CLSID_Language>,
   public ISupportErrorInfo,
   public IDispatchImpl<IInterfaceLanguage, &IID_IInterfaceLanguage, &LIBID_hMailServer, /*wMajor =*/ 1, /*wMinor =*/ 0>,
   public HM::COMAuthenticator
{
public:
   InterfaceLanguage()
   {
   }

DECLARE_REGISTRY_RESOURCEID(IDR_INTERFACELANGUAGE)


BEGIN_COM_MAP(InterfaceLanguage)
   COM_INTERFACE_ENTRY(IInterfaceLanguage)
   COM_INTERFACE_ENTRY(IDispatch)
   COM_INTERFACE_ENTRY(ISupportErrorInfo)
END_COM_MAP()

   STDMETHOD(InterfaceSupportsErrorInfo)(REFIID riid)
   {
      return riid == IID_IInterfaceLanguage ? S_OK : S_FALSE;
   }


   DECLARE_PROTECT_FINAL_CONSTRUCT()

   HRESULT FinalConstruct()
   {
      return S_OK;
   }
   
   void FinalRelease() 
   {
   }

   void Attach(std::shared_ptr<HM::Language> pLanguage);
   
public:

   STDMETHOD(get_Name)(BSTR *pVal);
   STDMETHOD(get_String)(BSTR EnglishString, BSTR *pVal);
   STDMETHOD(get_IsDownloaded)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(Download)(); 
   
private:

   std::shared_ptr<HM::Language> language_;
};

OBJECT_ENTRY_AUTO(__uuidof(Language), InterfaceLanguage)
