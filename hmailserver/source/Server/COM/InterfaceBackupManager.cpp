// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "InterfaceBackupManager.h"
#include "InterfaceBackup.h"

#include "../Common/Application/BackupManager.h"
#include "../Common/Application/MailboxRestore.h"
#include "../Common/Util/AuditTrail.h"

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

HRESULT
InterfaceBackupManager::StartMailboxRestore_(const HM::MailboxRestore::Request &request, long *Job)
{
   if (!Job)
      return COMError::WithoutMessage(E_POINTER);

   *Job = 0;

   if (!backup_manager_)
      return GetAccessDenied();

   // Asked again here, on this thread: the question is what installs the
   // actor the audit row below is attributed to. See InterfaceBackup::StartRestore.
   if (!GetIsServerAdmin())
      return GetAccessDenied();

   // Checked on this thread, with no bound - a COM caller's thread is its own -
   // so that a wrong request fails here with the reason and nothing is queued.
   int statusCode = 0;
   HM::String failureReason;
   std::shared_ptr<HM::MailboxRestore> job = HM::MailboxRestore::Prepare(request, 0, statusCode, failureReason);

   if (!job)
      return COMError::GenerateError(failureReason);

   if (!backup_manager_->StartMailboxRestore(job, failureReason))
   {
      // Registered by Prepare and refused the slot: its status says so.
      HM::MailboxRestore::MarkNotStarted(job->GetJob(), failureReason);
      return COMError::GenerateError(failureReason);
   }

   // The request, recorded here because nothing the restore itself does can
   // be: it runs on the maintenance work queue, where no actor is installed.
   // The same row the REST route writes, in the same words.
   HM::AuditTrail::Instance()->RecordEvent(_T("backup"), job->GetRequest().archive_file, _T("mailbox restore started"),
      job->Describe());

   *Job = job->GetJob();
   return S_OK;
}

STDMETHODIMP
InterfaceBackupManager::RestoreMailbox(BSTR File, BSTR Address, BSTR Mode, BSTR Passphrase, long *Job)
{
   try
   {
      HM::MailboxRestore::Request request;
      request.archive = File;
      request.address = Address;
      request.passphrase = Passphrase;

      if (!HM::MailboxRestore::ParseMode(HM::String(Mode), request.mode))
         return COMError::GenerateError("Mode must be merge or replace.");

      return StartMailboxRestore_(request, Job);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceBackupManager::RestoreMessage(BSTR File, BSTR Address, hyper MessageID, BSTR Folder, BSTR Passphrase, long *Job)
{
   try
   {
      if (MessageID <= 0)
         return COMError::GenerateError("MessageID must be a message id from DescribeMailbox's listing.");

      HM::MailboxRestore::Request request;
      request.archive = File;
      request.address = Address;
      request.passphrase = Passphrase;
      request.message_id = MessageID;
      request.folder = Folder;

      return StartMailboxRestore_(request, Job);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceBackupManager::DescribeMailbox(BSTR File, BSTR Address, BSTR Passphrase, BSTR *Json)
{
   try
   {
      if (!Json)
         return COMError::WithoutMessage(E_POINTER);

      if (!backup_manager_)
         return GetAccessDenied();

      HM::String archiveFile;
      HM::String failureReason;
      bool notFound = false;

      if (!HM::MailboxRestore::ResolveArchive(HM::String(File), archiveFile, notFound, failureReason))
         return COMError::GenerateError(failureReason);

      HM::ArchivedMailbox mailbox;
      bool accountNotFound = false;

      if (!HM::MailboxRestore::ReadMailbox(archiveFile, HM::String(Passphrase), HM::String(Address), 0, nullptr,
                                           mailbox, accountNotFound, failureReason))
      {
         return COMError::GenerateError(failureReason);
      }

      HM::String subjectsNote;
      const bool subjects = HM::MailboxRestore::ReadSubjects(archiveFile, HM::String(Passphrase), mailbox, 0, subjectsNote);

      HM::String json = HM::MailboxRestore::ListingJson(archiveFile, mailbox, subjects, subjectsNote);
      *Json = json.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceBackupManager::MailboxRestoreStatus(long Job, BSTR *Json)
{
   try
   {
      if (!Json)
         return COMError::WithoutMessage(E_POINTER);

      if (!backup_manager_)
         return GetAccessDenied();

      HM::MailboxRestore::Outcome outcome;

      if (Job <= 0 || !HM::MailboxRestore::GetOutcome((int) Job, outcome))
         return COMError::GenerateError("No such mailbox restore job; the last fifty are kept for the life of the process.");

      HM::String json = HM::MailboxRestore::OutcomeJson(outcome);
      *Json = json.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

