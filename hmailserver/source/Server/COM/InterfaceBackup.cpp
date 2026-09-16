// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "COMError.h"
#include "InterfaceBackup.h"

#include "../Common/Application/BackupManager.h"
#include "../Common/Util/AuditTrail.h"

void
InterfaceBackup::Attach(std::shared_ptr<HM::Backup> pBackup)
{
   backup_ = pBackup;
}

STDMETHODIMP
InterfaceBackup::StartRestore()
{
   try
   {
      if (!backup_)
         return GetAccessDenied();

      // Asked here, and not only because an object nobody but a server
      // administrator can obtain is being used for the most destructive thing in
      // this interface. Asking is what installs the actor on this thread
      // (COMAuthentication::InstallActor_), and the row below has to say who made
      // the request: a COM pool thread keeps the last caller's actor, so without
      // the question this would be attributed to whoever used the thread before.
      if (!GetIsServerAdmin())
         return GetAccessDenied();

      // Start a restore of this backup.
      const bool started = HM::Application::Instance()->GetBackupManager()->StartRestore(backup_);

      // The request, recorded here because nothing the restore itself does can be:
      // it runs on the maintenance work queue, where no actor is installed, so
      // every delete and insert it makes reaches the statement chokepoint with
      // nobody to attribute it to. Recorded only when it actually started - a
      // restore refused because one is already running changed nothing, and a
      // trail that says otherwise is worse than one that is silent.
      if (started)
      {
         HM::AuditTrail::Instance()->RecordEvent(_T("backup"), backup_->GetBackupFile(), _T("restore started"),
            HM::BackupManager::DescribeRestore(backup_));
      }

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceBackup::get_ContainsSettings(VARIANT_BOOL* pVal)
{
   try
   {
      if (!backup_)
         return GetAccessDenied();

      *pVal = backup_->GetContains(HM::Backup::BOSettings) ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceBackup::get_ContainsDomains(VARIANT_BOOL* pVal)
{
   try
   {
      if (!backup_)
         return GetAccessDenied();

      *pVal = backup_->GetContains(HM::Backup::BODomains) ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceBackup::get_ContainsMessages(VARIANT_BOOL* pVal)
{
   try
   {
      if (!backup_)
         return GetAccessDenied();

      *pVal = backup_->GetContains(HM::Backup::BOMessages) ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceBackup::get_RestoreSettings(VARIANT_BOOL* pVal)
{
   try
   {
      if (!backup_)
         return GetAccessDenied();

      *pVal = backup_->GetRestoreOption(HM::Backup::BOSettings) ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceBackup::put_RestoreSettings(VARIANT_BOOL newVal)
{
   try
   {
      if (!backup_)
         return GetAccessDenied();

      backup_->SetRestoreOption(HM::Backup::BOSettings, newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceBackup::get_RestoreDomains(VARIANT_BOOL* pVal)
{
   try
   {
      if (!backup_)
         return GetAccessDenied();

      *pVal = backup_->GetRestoreOption(HM::Backup::BODomains) ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceBackup::put_RestoreDomains(VARIANT_BOOL newVal)
{
   try
   {
      if (!backup_)
         return GetAccessDenied();

      backup_->SetRestoreOption(HM::Backup::BODomains, newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceBackup::get_RestoreMessages(VARIANT_BOOL* pVal)
{
   try
   {
      if (!backup_)
         return GetAccessDenied();

      *pVal = backup_->GetRestoreOption(HM::Backup::BOMessages) ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceBackup::put_RestoreMessages(VARIANT_BOOL newVal)
{
   try
   {
      if (!backup_)
         return GetAccessDenied();

      backup_->SetRestoreOption(HM::Backup::BOMessages, newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


