// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"
#include "COMError.h"
#include "InterfaceStatus.h"

#include "../Common/Util/ServerStatus.h"
#include "../Common/Util/UpdateChecker.h"
#include "../Common/Util/UpdateDownloader.h"
#include "../Common/Util/UpdateInstaller.h"

#include <rpcdcep.h>
#pragma comment(lib, "rpcrt4.lib")

InterfaceStatus::InterfaceStatus() :
   status_(nullptr),
   application_(nullptr)
{

}


bool 
InterfaceStatus::LoadSettings()
{
   if (!GetIsServerAdmin())
      return false;

   status_ = HM::ServerStatus::Instance();
   application_ = HM::Application::Instance();

   return true;
}


STDMETHODIMP 
InterfaceStatus::get_UndeliveredMessages(BSTR *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      HM::String sRetVal = status_->GetUnsortedMessageStatus();
      *pVal = sRetVal.AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceStatus::get_StartTime(BSTR *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      HM::String sRetVal = application_->GetStartTime();
      *pVal = sRetVal.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceStatus::get_ProcessedMessages(long *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      *pVal = status_->GetNumberOfProcessedMessages();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceStatus::get_RemovedViruses(long *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      *pVal = status_->GetNumberOfRemovedViruses();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceStatus::get_RemovedSpamMessages(long *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      *pVal = status_->GetNumberOfDetectedSpamMessages();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceStatus::get_SessionCount(eSessionType iType, long *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      *pVal = status_->GetNumberOfSessions(iType);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceStatus::get_ThreadID(long* pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      *pVal = status_->GetThreadID();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// The update check. The verdict lives in UpdateChecker, process-wide, so the
// Control Panel and the REST API read the same one; these are its view over COM.

STDMETHODIMP
InterfaceStatus::get_UpdateState(long *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      *pVal = (long) HM::UpdateChecker::Current().state;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceStatus::get_AvailableVersion(BSTR *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      *pVal = HM::UpdateChecker::Current().available_version.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceStatus::get_AvailableVersionPublished(BSTR *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      *pVal = HM::UpdateChecker::Current().published_at.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceStatus::get_AvailableVersionUrl(BSTR *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      *pVal = HM::UpdateChecker::Current().release_url.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceStatus::get_UpdateLastChecked(BSTR *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      *pVal = HM::UpdateChecker::Current().last_checked.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceStatus::get_UpdateLastError(BSTR *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      *pVal = HM::UpdateChecker::Current().last_error.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceStatus::CheckForUpdate(VARIANT_BOOL *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      // On the caller's thread, so that when this returns the properties are the
      // answer. A feed that cannot be read is false with UpdateLastError set, not
      // a COM error: the caller asked a question and this is the answer to it.
      HM::String error;
      *pVal = HM::UpdateChecker::CheckNow(error) ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceStatus::DownloadUpdate(VARIANT_BOOL *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      // On the caller's thread, as CheckForUpdate is: an installer is tens of
      // megabytes, and the caller asked to wait for the answer.
      HM::String error;
      *pVal = HM::UpdateDownloader::DownloadAndVerify(error) ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceStatus::get_UpdateInstallerPath(BSTR *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      *pVal = HM::UpdateChecker::Current().installer_path.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceStatus::get_UpdateSignerIdentity(BSTR *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      *pVal = HM::String(HM::UpdateChecker::Current().signer_identity).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceStatus::InstallUpdate(VARIANT_BOOL *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      // A refusal is recorded as the verdict, so UpdateLastError says why the
      // helper was not started - there is no other channel for it over COM.
      // Except where this service cannot run an installer at all: then nothing
      // failed, the installer is still verified and waiting, and the state stays
      // 3 while UpdateLastError says who installs it and how
      // (UpdateInstaller::InstallAdvice). Until 6.3.6 that refusal made the state
      // 5, which took Install update away from the Control Panel and offered
      // Download again.
      HM::String error;
      bool started = HM::UpdateInstaller::Apply(error);
      if (!started)
      {
         if (HM::UpdateInstaller::NeedsAdministrator())
            HM::UpdateChecker::RecordLastError(error);
         else
            HM::UpdateChecker::RecordFailure(error);
      }
      *pVal = started ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

bool
InterfaceStatus::LocalCaller_(HM::String &windowsAccount)
{
   windowsAccount.Empty();

   // The caller's account, as the RPC runtime authenticated it.
   RPC_AUTHZ_HANDLE privileges = nullptr;
   if (SUCCEEDED(CoQueryClientBlanket(nullptr, nullptr, nullptr, nullptr, nullptr, &privileges, nullptr)) && privileges != nullptr)
      windowsAccount = (const wchar_t *) privileges;
   if (windowsAccount.IsEmpty())
      windowsAccount = _T("(unknown)");

   // A COM call from a process on this computer arrives over local RPC (ALPC),
   // which knows the calling process; one that came over the network arrives
   // over TCP, which does not. The RPC runtime is asked about the call this
   // thread is serving (the NULL binding): its transport must be local, and the
   // caller's process id must be there to be had. Anything else is not local.
   unsigned int transport = 0;
   if (I_RpcBindingInqTransportType(nullptr, &transport) != RPC_S_OK || transport != TRANSPORT_TYPE_LPC)
      return false;
   unsigned long process = 0;
   if (I_RpcBindingInqLocalClientPID(nullptr, &process) != RPC_S_OK || process == 0)
      return false;
   return true;
}

STDMETHODIMP
InterfaceStatus::get_UpdateNeedsAdministrator(VARIANT_BOOL *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      *pVal = HM::UpdateInstaller::NeedsAdministrator() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceStatus::get_UpdateInstallAdvice(BSTR *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      *pVal = HM::UpdateInstaller::InstallAdvice().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceStatus::get_UpdatePreparation(BSTR *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      const HM::UpdateChecker::Snapshot snapshot = HM::UpdateChecker::Current();
      const bool preparing = snapshot.state == HM::UpdateChecker::StatePreparing || snapshot.state == HM::UpdateChecker::StateReadyForAdministrator ||
                             snapshot.state == HM::UpdateChecker::StateFailed;
      *pVal = (preparing ? snapshot.preparation : HM::String()).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceStatus::PrepareUpdateForAdministrator(BSTR releaseVersion, VARIANT_BOOL skipBackup, VARIANT_BOOL *pVal)
{
   try
   {
      // Asked again on this call's own thread: it is what records who asked
      // (the audit trail's actor), and only the server's administrator asks.
      if (!status_ || !GetIsServerAdmin())
         return GetAccessDenied();

      // A refusal leaves the state as it is: UpdateLastError says why.
      HM::String account;
      HM::String error;
      bool prepared = false;
      if (!LocalCaller_(account))
         error = HM::Formatter::Format(_T("An administrator's install is started from the Control Panel on this server, not over the network: open the hMailServer Control Panel on {0} and click Install update there."),
            HM::UpdateInstaller::ServerName());
      else
         prepared = HM::UpdateInstaller::PrepareForAdministrator(HM::String(releaseVersion), skipBackup == VARIANT_TRUE, account, error);
      if (!prepared)
         HM::UpdateChecker::RecordLastError(error);
      *pVal = prepared ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceStatus::BeginUpdateForAdministrator(BSTR releaseVersion, VARIANT_BOOL *pVal)
{
   try
   {
      // Asked again on this call's own thread: it is what records who asked
      // (the audit trail's actor), and only the server's administrator asks.
      if (!status_ || !GetIsServerAdmin())
         return GetAccessDenied();

      HM::String account;
      HM::String error;
      bool begun = false;
      if (!LocalCaller_(account))
         error = HM::Formatter::Format(_T("An administrator's install is started from the Control Panel on this server, not over the network: open the hMailServer Control Panel on {0} and click Install update there."),
            HM::UpdateInstaller::ServerName());
      else
         begun = HM::UpdateInstaller::BeginForAdministrator(HM::String(releaseVersion), account, error);
      if (!begun)
         HM::UpdateChecker::RecordLastError(error);
      *pVal = begun ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceStatus::CancelUpdateForAdministrator(BSTR cancelReason)
{
   try
   {
      // Asked again on this call's own thread: it is what records who asked
      // (the audit trail's actor), and only the server's administrator asks.
      if (!status_ || !GetIsServerAdmin())
         return GetAccessDenied();

      HM::String account;
      if (!LocalCaller_(account))
         return S_OK;
      HM::String reason(cancelReason);
      if (reason.GetLength() > 500)
         reason = reason.Left(500);
      HM::UpdateInstaller::CancelForAdministrator(reason.IsEmpty() ? HM::String(_T("the Control Panel cancelled it")) : reason, account);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceStatus::get_UpdateApplyOutcome(BSTR *pVal)
{
   try
   {
      if (!status_)
         return GetAccessDenied();

      HM::UpdateChecker::Snapshot snapshot = HM::UpdateChecker::Current();
      HM::String outcome;
      if (!snapshot.apply_status.IsEmpty())
         outcome = snapshot.apply_status + _T(" ") + snapshot.apply_version + _T(": ") + snapshot.apply_detail;
      *pVal = outcome.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

