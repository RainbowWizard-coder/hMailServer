// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "COMError.h"
#include "../Common/Util/AuditTrail.h"
#include "InterfaceIMAPFolderPermissions.h"
#include "InterfaceIMAPFolderPermission.h"

#include "../Common/BO/IMAPFolder.h"
#include "../Common/BO/ACLPermissions.h"
#include "../Common/BO/ACLPermission.h"
#include "../Common/Application/ACLManager.h"

void 
InterfaceIMAPFolderPermissions::AttachItem(std::shared_ptr<HM::IMAPFolder> pFolder)
{
   folder_ = pFolder;

   // The rows stored for this folder, read through ACLManager and not through
   // IMAPFolder::GetPermissions, which skips the database read for an
   // account-level folder ("account level folders never have permissions set").
   // That stopped being true when a private folder's ACL became how a mailbox is
   // shared and how Send-As is granted, and reading it here left this collection
   // empty for every account folder: Count 0, Item 0 a DISP_E_BADINDEX, Delete
   // and DeleteByDBID reporting success and removing nothing - while Add() wrote a
   // real hm_acl row. It is the same read GETACL, SETACL and DELETEACL make, for
   // the reason written at ACLManager::GetPermissionsSetOnFolder, and a public
   // folder is unaffected: that call hands one straight back to GetPermissions.
   HM::ACLManager aclManager;
   acl_permission_ = aclManager.GetPermissionsSetOnFolder(folder_);
}

STDMETHODIMP InterfaceIMAPFolderPermissions::get_Count(long *pVal)
{
   try
   {
      if (!acl_permission_)
         return GetAccessDenied();

      *pVal = acl_permission_->GetCount();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIMAPFolderPermissions::Delete(long Index)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!acl_permission_)
         return GetAccessDenied();

      acl_permission_->DeleteItem(Index);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIMAPFolderPermissions::Refresh()
{
   try
   {
      if (!acl_permission_)
         return GetAccessDenied();

      acl_permission_->Refresh();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIMAPFolderPermissions::get_Item(long Index, IInterfaceIMAPFolderPermission **pVal)
{
   try
   {
      if (!acl_permission_)
         return GetAccessDenied();

      CComObject<InterfaceIMAPFolderPermission>* pACLPermission = new CComObject<InterfaceIMAPFolderPermission>();
      pACLPermission->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::ACLPermission> pPersACLPermission = acl_permission_->GetItem(Index);
   
      if (!pPersACLPermission)
         return COMError::WithoutMessage(DISP_E_BADINDEX);  
   
      pACLPermission->AttachItem(pPersACLPermission);
      pACLPermission->AttachParent(acl_permission_, true);
      pACLPermission->AddRef();
      *pVal = pACLPermission;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIMAPFolderPermissions::Add(IInterfaceIMAPFolderPermission **pVal)
{
   try
   {
      if (!acl_permission_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      if (!acl_permission_)
         return authentication_->GetAccessDenied();
   
      CComObject<InterfaceIMAPFolderPermission>* pIntACLPermission = new CComObject<InterfaceIMAPFolderPermission>();
      pIntACLPermission->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::ACLPermission> pACLPermission = std::shared_ptr<HM::ACLPermission>(new HM::ACLPermission);
   
      pACLPermission->SetShareFolderID(folder_->GetID());
   
      pIntACLPermission->AttachItem(pACLPermission);
      pIntACLPermission->AttachParent(acl_permission_, false);
   
      pIntACLPermission->AddRef();
      *pVal = pIntACLPermission;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIMAPFolderPermissions::get_ItemByDBID(long DBID, IInterfaceIMAPFolderPermission **pVal)
{
   try
   {
      if (!acl_permission_)
         return GetAccessDenied();

      CComObject<InterfaceIMAPFolderPermission>* pACLPermission = new CComObject<InterfaceIMAPFolderPermission>();
      pACLPermission->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::ACLPermission> pPersACLPermission = acl_permission_->GetItemByDBID(DBID);
   
      if (pPersACLPermission)
      {
         pACLPermission->AttachItem(pPersACLPermission);
         pACLPermission->AttachParent(acl_permission_, true);
         pACLPermission->AddRef();
         *pVal = pACLPermission;
      }
      else
      {
         return COMError::WithoutMessage(DISP_E_BADINDEX);  
      }
   
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIMAPFolderPermissions::DeleteByDBID(long DBID)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!acl_permission_)
         return GetAccessDenied();

   
      if (acl_permission_)
         acl_permission_->DeleteItemByDBID(DBID);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceIMAPFolderPermissions::get_ItemByName(BSTR Name, IInterfaceIMAPFolderPermission **pVal)
{
   try
   {
      if (!acl_permission_)
         return GetAccessDenied();

      CComObject<InterfaceIMAPFolderPermission>* pACLPermission = new CComObject<InterfaceIMAPFolderPermission>();
      pACLPermission->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::ACLPermission> pPersACLPermission = acl_permission_->GetItemByName(Name);
   
      if (pPersACLPermission)
      {
         pACLPermission->AttachItem(pPersACLPermission);
         pACLPermission->AttachParent(acl_permission_, true);
         pACLPermission->AddRef();
         *pVal = pACLPermission;
      }
      else
      {
         return COMError::WithoutMessage(DISP_E_BADINDEX);  
      }
   
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


