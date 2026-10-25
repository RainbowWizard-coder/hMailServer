// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once
#include "../hMailServer/resource.h"       // main symbols

#include "../hMailServer/hMailServer.h"

namespace HM
{
   class ServerStatus;
}

class ATL_NO_VTABLE InterfaceStatus : 
   public CComObjectRootEx<CComSingleThreadModel>,
   public CComCoClass<InterfaceStatus, &CLSID_Status>,
   public ISupportErrorInfo,
   public IDispatchImpl<IInterfaceStatus, &IID_IInterfaceStatus, &LIBID_hMailServer, /*wMajor =*/ 1, /*wMinor =*/ 0>,
   public HM::COMAuthenticator
{
public:
   InterfaceStatus();
   
   bool LoadSettings();

DECLARE_REGISTRY_RESOURCEID(IDR_INTERFACESTATUS)


BEGIN_COM_MAP(InterfaceStatus)
   COM_INTERFACE_ENTRY(IInterfaceStatus)
   COM_INTERFACE_ENTRY(IDispatch)
   COM_INTERFACE_ENTRY(ISupportErrorInfo)
END_COM_MAP()

   STDMETHOD(InterfaceSupportsErrorInfo)(REFIID riid)
   {
      return riid == IID_IInterfaceStatus ? S_OK : S_FALSE;
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
   STDMETHOD(get_UndeliveredMessages)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(get_StartTime)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(get_ProcessedMessages)(/*[out, retval]*/ long *pVal);
   STDMETHOD(get_RemovedViruses)(/*[out, retval]*/ long *pVal);
   STDMETHOD(get_RemovedSpamMessages)(/*[out, retval]*/ long *pVal);
   STDMETHOD(get_SessionCount)(eSessionType iType, long *pVal);
   STDMETHOD(get_ThreadID)(/*[out, retval]*/ long* pVal);
   STDMETHOD(get_UpdateState)(/*[out, retval]*/ long *pVal);
   STDMETHOD(get_AvailableVersion)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(get_AvailableVersionPublished)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(get_AvailableVersionUrl)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(get_UpdateLastChecked)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(get_UpdateLastError)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(CheckForUpdate)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(DownloadUpdate)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(get_UpdateInstallerPath)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(get_UpdateSignerIdentity)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(InstallUpdate)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(get_UpdateApplyOutcome)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(get_UpdateNeedsAdministrator)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(get_UpdateInstallAdvice)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(PrepareUpdateForAdministrator)(BSTR releaseVersion, VARIANT_BOOL skipBackup, /*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(get_UpdatePreparation)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(BeginUpdateForAdministrator)(BSTR releaseVersion, /*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(CancelUpdateForAdministrator)(BSTR cancelReason);

private:

   // Whether this call comes from a process on this computer (local RPC, whose
   // transport knows the calling process, where a remote caller's call comes
   // over TCP), and the caller's Windows account (CoQueryClientBlanket), for the
   // log and the audit trail. An administrator's install is started from the Control Panel on the
   // server and nowhere else: a remote administrator cannot be elevated here.
   static bool LocalCaller_(HM::String &windowsAccount);

   HM::ServerStatus *status_;
   HM::Application *application_;
};

OBJECT_ENTRY_AUTO(__uuidof(Status), InterfaceStatus)
