// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "COMError.h"
#include "../Common/Util/AuditTrail.h"
#include "InterfaceDirectories.h"

InterfaceDirectories::InterfaceDirectories() :
   ini_file_settings_(nullptr)
{

}

// [Directories] lives in hMailServer.INI, which the Windows service's own account
// may not write (docs/ServiceAccount.md). The folders are how the server finds
// everything else, and the elevated /GrantServiceAccess reads them, so a change is
// an administrator's, made in the file itself; one outside the program folder is
// then approved for the service before it is granted.
static HM::String DirectoryRefusal_()
{
   return HM::IniFileSettings::InitializationFileRefusal(_T("The folder"),
      _T("An administrator changes [Directories] in the file with an editor run as an administrator, approves a folder outside the program folder for the service - hMailServer.exe --approve-modify \"<folder>\" /GrantServiceAccess, or Grant access on the Control Panel's Windows service account card - and restarts the service."));
}

STDMETHODIMP InterfaceDirectories::get_ProgramDirectory(BSTR *pVal)
{
   try
   {
      if (!ini_file_settings_)
         return GetAccessDenied();

      *pVal = ini_file_settings_->GetProgramDirectory().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDirectories::put_ProgramDirectory(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!ini_file_settings_)
         return GetAccessDenied();

      if (!HM::IniFileSettings::InitializationFileWritable())
         return COMError::GenerateError(DirectoryRefusal_());

      ini_file_settings_->SetProgramDirectory(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDirectories::get_DatabaseDirectory(BSTR *pVal)
{
   try
   {
      if (!ini_file_settings_)
         return GetAccessDenied();

      *pVal = ini_file_settings_->GetDatabaseDirectory().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDirectories::put_DatabaseDirectory(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!ini_file_settings_)
         return GetAccessDenied();

      if (!HM::IniFileSettings::InitializationFileWritable())
         return COMError::GenerateError(DirectoryRefusal_());

      ini_file_settings_->SetDatabaseDirectory(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDirectories::get_DataDirectory(BSTR *pVal)
{
   try
   {
      if (!ini_file_settings_)
         return GetAccessDenied();

      *pVal = ini_file_settings_->GetDataDirectory().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDirectories::put_DataDirectory(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!ini_file_settings_)
         return GetAccessDenied();

      if (!HM::IniFileSettings::InitializationFileWritable())
         return COMError::GenerateError(DirectoryRefusal_());

      ini_file_settings_->SetDataDirectory(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDirectories::get_LogDirectory(BSTR *pVal)
{
   try
   {
      if (!ini_file_settings_)
         return GetAccessDenied();

      *pVal = ini_file_settings_->GetLogDirectory().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDirectories::put_LogDirectory(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!ini_file_settings_)
         return GetAccessDenied();

      if (!HM::IniFileSettings::InitializationFileWritable())
         return COMError::GenerateError(DirectoryRefusal_());

      ini_file_settings_->SetLogDirectory(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDirectories::get_TempDirectory(BSTR *pVal)
{
   try
   {
      if (!ini_file_settings_)
         return GetAccessDenied();

      *pVal = ini_file_settings_->GetTempDirectory().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDirectories::put_TempDirectory(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!ini_file_settings_)
         return GetAccessDenied();

      if (!HM::IniFileSettings::InitializationFileWritable())
         return COMError::GenerateError(DirectoryRefusal_());

      ini_file_settings_->SetTempDirectory(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDirectories::get_EventDirectory(BSTR *pVal)
{
   try
   {
      if (!ini_file_settings_)
         return GetAccessDenied();

      *pVal = ini_file_settings_->GetEventDirectory().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDirectories::put_EventDirectory(BSTR newVal)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!ini_file_settings_)
         return GetAccessDenied();

      if (!HM::IniFileSettings::InitializationFileWritable())
         return COMError::GenerateError(DirectoryRefusal_());

      ini_file_settings_->SetEventDirectory(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDirectories::get_DBScriptDirectory(BSTR *pVal)
{
   try
   {
      if (!ini_file_settings_)
         return GetAccessDenied();

      *pVal = ini_file_settings_->GetDBScriptDirectory().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


