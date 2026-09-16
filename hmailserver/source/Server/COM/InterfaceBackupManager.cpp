// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "InterfaceBackupManager.h"
#include "InterfaceBackup.h"

#include "../Common/Application/BackupManager.h"

#include "COMError.h"

bool 
InterfaceBackupManager::LoadSettings()
{
   if (!GetIsServerAdmin())
      return false;

   backup_manager_ = HM::Application::Instance()->GetBackupManager();
   if (!backup_manager_)
      return false;

   return true;

}

STDMETHODIMP 
InterfaceBackupManager::StartBackup()
{
   try
   {
      if (!backup_manager_)
         return GetAccessDenied();

      backup_manager_->StartBackup();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceBackupManager::LoadBackup(BSTR sXMLFile, IInterfaceBackup **pVal)
{
   try
   {
      CComBSTR noPassphrase(L"");
      return LoadEncryptedBackup(sXMLFile, noPassphrase, pVal);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceBackupManager::LoadEncryptedBackup(BSTR File, BSTR Passphrase, IInterfaceBackup **pVal)
{
   try
   {
      if (!backup_manager_)
         return GetAccessDenied();

      // The archive is opened BEFORE the COM object is made, unlike before: the
      // object used to be created first and leaked on every archive that could not
      // be opened. The reason the manager gives is passed on rather than flattened
      // into DISP_E_BADINDEX, because "this backup is encrypted and you gave no
      // passphrase", "that passphrase is wrong", "this archive was written by a
      // newer hMailServer" and "this file is not an archive at all" are four
      // different things for the administrator to do next.
      //
      // The passphrase is carried on the Backup object that comes back, so the
      // restore that follows opens the archive the same way; it is stored nowhere
      // and logged nowhere.
      HM::String failureReason;
      std::shared_ptr<HM::Backup> pBackup = backup_manager_->LoadBackup(File, Passphrase, failureReason);

      if (!pBackup)
         return COMError::GenerateError(failureReason);

      CComObject<InterfaceBackup>* pBackupInt = new CComObject<InterfaceBackup>();
      pBackupInt->SetAuthentication(authentication_);

      pBackupInt->Attach(pBackup);
      pBackupInt->AddRef();
      *pVal = pBackupInt;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

