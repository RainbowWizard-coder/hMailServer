// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../hMailServer/resource.h"       // main symbols
#include "../hMailServer/hMailServer.h"

#include "../Common/Application/MailboxRestore.h"

// InterfaceBackupManager

class ATL_NO_VTABLE InterfaceBackupManager : 
   public CComObjectRootEx<CComSingleThreadModel>,
   public CComCoClass<InterfaceBackupManager, &CLSID_BackupManager>,
   public ISupportErrorInfo,
   public IDispatchImpl<IInterfaceBackupManager, &IID_IInterfaceBackupManager, &LIBID_hMailServer, /*wMajor =*/ 1, /*wMinor =*/ 0>,
   public HM::COMAuthenticator
{
public:
   InterfaceBackupManager()
   {
   }

   bool LoadSettings();

DECLARE_REGISTRY_RESOURCEID(IDR_INTERFACEBACKUPMANAGER)


BEGIN_COM_MAP(InterfaceBackupManager)
   COM_INTERFACE_ENTRY(IInterfaceBackupManager)
   COM_INTERFACE_ENTRY(IDispatch)
   COM_INTERFACE_ENTRY(ISupportErrorInfo)
END_COM_MAP()

   STDMETHOD(InterfaceSupportsErrorInfo)(REFIID riid)
   {
      return riid == IID_IInterfaceBackupManager ? S_OK : S_FALSE;
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
   STDMETHOD(StartBackup)();
   STDMETHOD(LoadBackup)(/*[in]*/ BSTR sXMLFile, /*[out, retval]*/ IInterfaceBackup **pVal);
   STDMETHOD(LoadEncryptedBackup)(/*[in]*/ BSTR File, /*[in]*/ BSTR Passphrase, /*[out, retval]*/ IInterfaceBackup **pVal);

   // One mailbox, or one message, from an archive (MailboxRestore).
   STDMETHOD(RestoreMailbox)(/*[in]*/ BSTR File, /*[in]*/ BSTR Address, /*[in]*/ BSTR Mode, /*[in]*/ BSTR Passphrase, /*[out, retval]*/ long *Job);
   STDMETHOD(RestoreMessage)(/*[in]*/ BSTR File, /*[in]*/ BSTR Address, /*[in]*/ hyper MessageID, /*[in]*/ BSTR Folder, /*[in]*/ BSTR Passphrase, /*[out, retval]*/ long *Job);
   STDMETHOD(DescribeMailbox)(/*[in]*/ BSTR File, /*[in]*/ BSTR Address, /*[in]*/ BSTR Passphrase, /*[out, retval]*/ BSTR *Json);
   STDMETHOD(MailboxRestoreStatus)(/*[in]*/ long Job, /*[out, retval]*/ BSTR *Json);

private:
   // The two restores share everything but the request: checked on this
   // thread, queued under the run slot, recorded on the audit trail.
   HRESULT StartMailboxRestore_(const HM::MailboxRestore::Request &request, long *Job);

private:
   std::shared_ptr<HM::BackupManager> backup_manager_;
};

OBJECT_ENTRY_AUTO(__uuidof(BackupManager), InterfaceBackupManager)
