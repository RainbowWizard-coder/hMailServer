// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../hMailServer/resource.h"       // main symbols
#include "../hMailServer/hMailServer.h"

#include "../Common/BO/Account.h"
#include "../Common/Util/DirectoryStore.h"
#include "COMCollection.h"

namespace HM 
{ 
   class Accounts; 
   class COMAuthentication;
}

class ATL_NO_VTABLE InterfaceAccount : 
   public COMCollectionItem<HM::Account, HM::Accounts>,
   public CComObjectRootEx<CComSingleThreadModel>,
   public ISupportErrorInfo,
   public CComCoClass<InterfaceAccount, &CLSID_Account>,
   public IDispatchImpl<IInterfaceAccount, &IID_IInterfaceAccount, &LIBID_hMailServer, /*wMajor =*/ 1, /*wMinor =*/ 0>,
   public HM::COMAuthenticator
{
public:
   InterfaceAccount()
   {
#ifdef _DEBUG
      InterlockedIncrement(&counter);
#endif
   }

   ~InterfaceAccount()
   {
#ifdef _DEBUG
      InterlockedDecrement(&counter);
#endif
   }

   void SetAuthentication(std::shared_ptr<HM::COMAuthentication> pAuthentication);

DECLARE_REGISTRY_RESOURCEID(IDR_INTERFACEACCOUNT)


BEGIN_COM_MAP(InterfaceAccount)
   COM_INTERFACE_ENTRY(IInterfaceAccount)
   COM_INTERFACE_ENTRY(IDispatch)
   COM_INTERFACE_ENTRY(ISupportErrorInfo)
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

   HRESULT STDMETHODCALLTYPE get_ID(LONG* pVal)
   {
      *pVal = (int) object_->GetID();
      return S_OK;
   }

   STDMETHOD(InterfaceSupportsErrorInfo)(REFIID riid);

   STDMETHOD(get_Messages)(/*[out, retval]*/ IInterfaceMessages* *pVal);
   STDMETHOD(get_FetchAccounts)(/*[out, retval]*/ IInterfaceFetchAccounts * *pVal);
   STDMETHOD(get_AppPasswords)(/*[out, retval]*/ IInterfaceAppPasswords * *pVal);
   STDMETHOD(get_TOTPEnabled)(VARIANT_BOOL *pVal);
   STDMETHOD(EnrolTOTP)(BSTR *pVal);
   STDMETHOD(DisableTOTP)();
   STDMETHOD(ExportMessages)(BSTR Directory, long *ExportedCount);
   STDMETHOD(ExportArchive)(BSTR File, long *ExportedCount);
   STDMETHOD(Delete)(); 

   STDMETHOD(DeleteMessages)();
   STDMETHOD(get_ADUsername)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_ADUsername)(/*[in]*/ BSTR newVal);
   STDMETHOD(Save)();
   STDMETHOD(UnlockMailbox)();
   STDMETHOD(ValidatePassword)(/*[in]*/ BSTR Password, /*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(get_Size)(/*[out, retval]*/ float *pVal);
   STDMETHOD(get_Password)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_Password)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_IsAD)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_IsAD)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_DomainID)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_DomainID)(LONG newVal);
   STDMETHOD(get_Address)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_Address)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_ADDomain)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_ADDomain)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_Active)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_Active)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_MaxSize)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_MaxSize)(/*[in]*/ long newVal);
   STDMETHOD(get_MessageRetentionDays)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_MessageRetentionDays)(/*[in]*/ long newVal);

   STDMETHOD(get_VacationMessageIsOn)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_VacationMessageIsOn)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_VacationMessage)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_VacationMessage)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_VacationSubject)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_VacationSubject)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_VacationMessageExpires)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_VacationMessageExpires)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_VacationMessageExpiresDate)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_VacationMessageExpiresDate)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_VacationMessageBeginDate)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_VacationMessageBeginDate)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_RoleName)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_RoleName)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_RoleDomains)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_RoleDomains)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_RolePermissions)(/*[out, retval]*/ BSTR *pVal);

   // The account's entry in the organisation's directory (DirectoryStore.h).
   // The row is read at the first get or put and written by Save(), so the
   // whole entry is one round trip however many properties a caller sets.
   STDMETHOD(get_DirectoryTitle)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_DirectoryTitle)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_DirectoryDepartment)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_DirectoryDepartment)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_DirectoryPhone)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_DirectoryPhone)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_DirectoryMobile)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_DirectoryMobile)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_DirectoryOffice)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_DirectoryOffice)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_DirectoryOptOut)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_DirectoryOptOut)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_DirectoryNote)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_DirectoryNote)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_DirectoryPhoto)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_DirectoryPhoto)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_DirectoryPhotoType)(/*[out, retval]*/ BSTR *pVal);

   STDMETHOD(get_OpenPGPPublish)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_OpenPGPPublish)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_OpenPGPKeys)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(get_VacationMessageAbortSpamFlagged)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_VacationMessageAbortSpamFlagged)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_VacationMessageDays)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_VacationMessageDays)(/*[in]*/ long newVal);
   STDMETHOD(get_VacationInternalSubject)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_VacationInternalSubject)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_VacationInternalMessage)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_VacationInternalMessage)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_VacationMessageInternalOnly)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_VacationMessageInternalOnly)(/*[in]*/ VARIANT_BOOL newVal);


   STDMETHOD(get_AdminLevel)(/*[out, retval]*/ eAdminLevel *pVal);
   STDMETHOD(put_AdminLevel)(/*[in]*/ eAdminLevel newVal);

   STDMETHOD(get_Rules)(/*[out, retval]*/ IInterfaceRules **pVal);

   STDMETHOD(get_IMAPFolders)(/*[out, retval]*/ IInterfaceIMAPFolders **pVal);

   STDMETHOD(get_QuotaUsed)(/*[out, retval]*/ long *pVal);

   STDMETHOD(get_ForwardEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_ForwardEnabled)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_ForwardAddress)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_ForwardAddress)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_ForwardKeepOriginal)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_ForwardKeepOriginal)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_ForwardAbortSpamFlagged)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_ForwardAbortSpamFlagged)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(get_SignatureEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_SignatureEnabled)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_SignaturePlainText)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_SignaturePlainText)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_SignatureHTML)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_SignatureHTML)(/*[in]*/ BSTR newVal);

   STDMETHOD(get_LastLogonTime)(/*[out, retval]*/ VARIANT *pVal);

   STDMETHOD(get_PersonFirstName)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_PersonFirstName)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_PersonLastName)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_PersonLastName)(/*[in]*/ BSTR newVal);

   STDMETHOD(get_SieveScript)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_SieveScript)(/*[in]*/ BSTR newVal);

   STDMETHOD(get_AntiSpamEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_AntiSpamEnabled)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_SpamMarkThreshold)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_SpamMarkThreshold)(/*[in]*/ long newVal);
   STDMETHOD(get_SpamDeleteThreshold)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_SpamDeleteThreshold)(/*[in]*/ long newVal);

   // The clients that have signed in to this account, and the one call that ends
   // what one of them holds. A JSON document rather than a collection: see the
   // IDL for why, and SignInStore.h for what a revoke does.
   STDMETHOD(GetDevices)(/*[out, retval]*/ BSTR *Json);
   STDMETHOD(RevokeDevice)(/*[in]*/ BSTR DeviceID, /*[in]*/ VARIANT_BOOL RevokeCredential, /*[out, retval]*/ VARIANT_BOOL *Revoked);

   STDMETHOD(get_PushSubscriptionCount)(/*[out, retval]*/ long *pVal);
   STDMETHOD(RemovePushSubscriptions)(/*[out, retval]*/ long *RemovedCount);

   STDMETHOD(get_RecoveryAddress)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(get_RecoveryAddressVerified)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(get_RecoveryCodesUnused)(/*[out, retval]*/ long *pVal);
   STDMETHOD(ClearRecoveryAddress)();
   STDMETHOD(ClearRecoveryCodes)();

   STDMETHOD(MoveTo)(/*[in]*/ BSTR NewAddress, /*[in]*/ VARIANT_BOOL KeepOldAddressAsAlias, /*[out, retval]*/ BSTR *Summary);
   STDMETHOD(get_InUseReason)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(get_LastMove)(/*[out, retval]*/ BSTR *pVal);


private:

   // The directory entry, read once per object and written by Save().
   bool EnsureDirectory_();
   HRESULT SetDirectoryField_(HM::String &field, BSTR newVal, int maximum, const char *name);

   bool directory_loaded_ = false;
   bool directory_dirty_ = false;
   // What the stored opt-out was when the row was read, so that Save() can
   // tell a change of it from a save that left it alone: reversing somebody's
   // opt-out is the one thing in the entry that goes on the audit trail.
   bool directory_opt_out_read_ = false;
   HM::DirectoryRecord directory_;

   std::shared_ptr<HM::COMAuthentication> authentication_;

#ifdef _DEBUG
   static long counter;
#endif

};

OBJECT_ENTRY_AUTO(__uuidof(Account), InterfaceAccount)
