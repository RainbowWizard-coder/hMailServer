// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "ACLPermissions.h"
#include "../Application/ACLManager.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   ACLPermissions::ACLPermissions() :
      folder_id_(0)
   {

   }

      ACLPermissions::ACLPermissions(__int64 iFolderID) :
      folder_id_(iFolderID)
   {

   }

   ACLPermissions::~ACLPermissions(void)
   {
   }

   void 
   ACLPermissions::Refresh()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Reads all groups form the database
   //---------------------------------------------------------------------------()
   {
      String sSQL;
      sSQL.Format(_T("select * from hm_acl where aclsharefolderid = %I64d"), folder_id_);

      DBLoad_(sSQL);
   }

   std::shared_ptr<ACLPermission> 
   ACLPermissions::GetPermissionForAccount(__int64 iAccountID)
   {
      auto iter = vecObjects.begin();
      auto iterEnd = vecObjects.end();

      for (; iter != iterEnd; iter++)
      {
         std::shared_ptr<ACLPermission> pPermission = (*iter);
         if (pPermission->GetPermissionType() == 0 && pPermission->GetPermissionAccountID() == iAccountID)
         {
            return pPermission;
         }
      }

      std::shared_ptr<ACLPermission> pEmpty;
      return pEmpty;
   }

   std::shared_ptr<ACLPermission> 
   ACLPermissions::GetPermissionForGroup(__int64 iGroupID)
   {
      auto iter = vecObjects.begin();
      auto iterEnd = vecObjects.end();

      for (; iter != iterEnd; iter++)
      {
         std::shared_ptr<ACLPermission> pPermission = (*iter);
         if (pPermission->GetPermissionType() == ACLPermission::PTGroup && pPermission->GetPermissionGroupID() == iGroupID)
         {
            return pPermission;
         }
      }

      std::shared_ptr<ACLPermission> pEmpty;
      return pEmpty;
   }

   // Called before save in DB
   bool
   ACLPermissions::PreSaveObject(std::shared_ptr<ACLPermission> pPermission, XNode *pNode)
   {
      pPermission->SetShareFolderID(folder_id_);

      // Collection::XMLLoad is the only caller, so everything below is the
      // restore's. A grant is stored as its holder's NAME, and XMLLoad leaves the
      // holder at 0 when this server has no account or group of that name YET: a
      // grant on an account's own folder is read back in the middle of the domain
      // phase, where the account it names may be two domains further down the
      // archive and a group is not restored until the settings phase after it.
      // PersistentACLPermission::Validate refuses a row with no holder, so saving
      // it here would fail the whole restore over the order two mailboxes happen
      // to appear in. It waits instead, and ACLManager::ApplyRestoredGrants writes
      // it when everything it could name is back.
      //
      // A GROUP grant always waits, even when a group of that name exists now:
      // the settings phase deletes and recreates every group, with new ids, so a
      // row written against the group's id during the domain phase named a
      // group that did not exist by the end of the restore, and the grant
      // silently vanished. Written at the end, it names the group that is there.
      bool holderIsBack = pPermission->GetPermissionType() == ACLPermission::PTAnyone ||
                          (pPermission->GetPermissionType() == ACLPermission::PTUser && pPermission->GetPermissionAccountID() != 0);

      if (!holderIsBack)
      {
         ACLManager::RememberRestoredGrant(folder_id_, (int) pPermission->GetPermissionType(),
                                           pPermission->GetValue(), pNode->GetAttrValue(_T("Holder")));

         return false;
      }

      return true;
   }

   bool 
   ACLPermissions::DeletePermissionsForAccount(__int64 iAccountID)
   {
      auto iter = vecObjects.begin();

      while (iter != vecObjects.end())
      {
         std::shared_ptr<ACLPermission> pPermission = (*iter);
         if (pPermission->GetPermissionType() == 0 && pPermission->GetPermissionAccountID() == iAccountID)
         {
            if (!PersistentACLPermission::DeleteObject(pPermission))
               return false;

            iter = vecObjects.erase(iter);
         }
         else
            iter++;
      }

      return true;
   }
}