// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "InterfaceIMAPFolderPermission.h"
#include "InterfaceAccount.h"
#include "InterfaceGroup.h"

#include "../Common/BO/ACLPermissions.h"
#include "../Common/BO/Groups.h"
#include "../Common/Persistence/PersistentACLPermission.h"
#include "../Common/Persistence/PersistentAccount.h"
#include "../Common/Cache/CacheContainer.h"
#include "../IMAP/IMAPConfiguration.h"

#include "COMError.h"
#include "../Common/Util/AuditTrail.h"

STDMETHODIMP InterfaceIMAPFolderPermission::InterfaceSupportsErrorInfo(REFIID riid)
{
   try
   {
      static const IID* arr[] = 
      {
         &IID_IInterfaceIMAPFolderPermission,
      };
   
      for (int i=0;i<sizeof(arr)/sizeof(arr[0]);i++)
      {
         if (InlineIsEqualGUID(*arr[i],riid))
            return S_OK;
      }
      return S_FALSE;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIMAPFolderPermission::get_ShareFolderID(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetShareFolderID();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIMAPFolderPermission::get_PermissionType(eACLPermissionType *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (eACLPermissionType) object_->GetPermissionType();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIMAPFolderPermission::put_PermissionType(eACLPermissionType newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetPermissionType((HM::ACLPermission::ePermissionType) newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIMAPFolderPermission::get_PermissionGroupID(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetPermissionGroupID();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIMAPFolderPermission::put_PermissionGroupID(LONG newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetPermissionGroupID(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIMAPFolderPermission::get_PermissionAccountID(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetPermissionAccountID();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIMAPFolderPermission::put_PermissionAccountID(LONG newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetPermissionAccountID(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIMAPFolderPermission::get_Value(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetValue();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIMAPFolderPermission::put_Value(LONG newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetValue(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// WHY THERE IS NO ADMINISTRATOR CHECK HERE, AND WHY THAT IS THE DECISION.
//
// Adding a grant asks GetIsDomainAdmin (InterfaceIMAPFolderPermissions::Add).
// Changing one here, and deleting one (::Delete below, and the collection's
// Delete and DeleteByDBID), ask nothing - and are meant not to. The rule this
// object keeps is IMAP's: a folder's OWNER administers it. ACLManager::
// GetPermissionForFolder says so by answering GrantAll for the owner before it
// reads a single row, and SETACL and DELETEACL have let an owner exercise that
// over the wire on their own mailbox since account folders became shareable.
// Adding a domain-administrator check here would not close anything - the same
// account could make the same change from any IMAP client - it would only make
// COM disagree with the protocol about who owns a mailbox.
//
// What bounds it is where the object comes from, not what is asked here. A
// permission is reached through a folder, a folder through an account, and an
// account through InterfaceDomain::get_Accounts, which hands a credential that
// is not a domain administrator a collection holding its own account and nothing
// else. A grant on somebody else's folder is therefore not reachable to be
// edited or deleted, and a grant the caller does reach is one on a folder it
// owns, where IMAP would let it do the same. Nor can a grant be redirected at
// another folder: ShareFolderID is read-only in the IDL and is written by the
// collection that created the row, from the folder it was reached through. The
// public folders, whose owner is the server, are behind a server-administrator
// check two levels up (InterfaceApplication::get_Settings).
//
// Every change made through this object is on the audit trail as a folder
// permission, under the credential that made it - the AuditScope below - which
// is what makes the decision reviewable rather than merely argued.
//
// The boundary is pinned by
// Permissions.AnAccountCannotReachAGrantOnAFolderItDoesNotOwn.
STDMETHODIMP InterfaceIMAPFolderPermission::Save()
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!object_)
         return GetAccessDenied();

      if (HM::PersistentACLPermission::SaveObject(object_))
      {
         // Add to parent collection
         AddToParentCollection();
         return S_OK;
      }
   
      return COMError::GenerateError("Failed to save object. See hMailServer error log.");
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceIMAPFolderPermission::get_Permission(eACLPermission iPermission, VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetAllow((HM::ACLPermission::ePermission) iPermission) ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceIMAPFolderPermission::put_Permission(eACLPermission iPermission, VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetAllow((HM::ACLPermission::ePermission) iPermission, newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIMAPFolderPermission::Delete()
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!object_)
         return GetAccessDenied();

      if (!parent_collection_)
         return HM::PersistentACLPermission::DeleteObject(object_) ? S_OK : S_FALSE;
   
      parent_collection_->DeleteItemByDBID(object_->GetID());
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceIMAPFolderPermission::get_Account(IInterfaceAccount **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      std::shared_ptr<HM::Account> pAccount = std::shared_ptr<HM::Account>(new HM::Account);
   
      if (!HM::PersistentAccount::ReadObject(pAccount, (__int64) object_->GetPermissionAccountID()))
         return COMError::WithoutMessage(DISP_E_BADINDEX);
   
      // The account a permission names is handed out only to somebody who could
      // reach it anyway: the server administrator, a domain administrator of
      // its domain, or the account itself - the rule Links.Account applies.
      // Without this, any account that could sign in over COM reached another
      // account's full object through the permission list of a folder of its
      // own (a row it can write with IMAP SETACL), and could set that account's
      // password and save it. That made "an auditor changes nothing" false
      // through one property, and made every mailbox holder a password reset
      // away from any other mailbox that was not a server administrator's.
      const bool itself = authentication_->GetAccountID() == pAccount->GetID();
      const bool domainAdministrator = authentication_->GetIsDomainAdmin() && authentication_->GetDomainID() == pAccount->GetDomainID();

      if (!authentication_->GetIsServerAdmin() && !domainAdministrator && !itself)
         return authentication_->GetAccessDenied();

      CComObject<InterfaceAccount>* pInterfaceAccount = new CComObject<InterfaceAccount>();
      pInterfaceAccount->SetAuthentication(authentication_);

      pInterfaceAccount->AttachItem(pAccount);
      pInterfaceAccount->AddRef();
      *pVal = pInterfaceAccount;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceIMAPFolderPermission::get_Group(IInterfaceGroup **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      CComObject<InterfaceGroup>* pInterfaceGroup = new CComObject<InterfaceGroup>();
      pInterfaceGroup->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::Groups> groups = HM::Configuration::Instance()->GetIMAPConfiguration()->GetGroups();
      std::shared_ptr<HM::Group> pGroup = groups->GetItemByDBID(object_->GetPermissionGroupID());

      if (!pGroup)
         return COMError::WithoutMessage(DISP_E_BADINDEX);

      // The running server's group, as InterfaceGroups hands it out: a put_*
      // on it writes a copy, and Save reads the groups again (COMCollection.h).
      pInterfaceGroup->AttachItem(pGroup);
      pInterfaceGroup->AttachSharedParent(groups, true);
      pInterfaceGroup->AddRef();
      *pVal = pInterfaceGroup;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

