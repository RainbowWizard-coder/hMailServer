// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "InterfaceDatabase.h"

#include "COMError.h"

#include "../Common/SQL/SQLCEConnection.h"
#include "../Common/SQL/DatabaseSettings.h"
#include "../Common/SQL/DALConnectionFactory.h"
#include "../Common/SQL/DALConnection.h"
#include "../Common/SQL/SQLScriptParser.h"
#include "../Common/SQL/SQLScriptRunner.h"

#include "../Common/Util/PasswordGenerator.h"
#include "../Common/Application/ErrorManager.h"
#include "../Common/Application/NewInstallDefaults.h"
#include "../Common/SQL/SQLCommand.h"

STDMETHODIMP 
InterfaceDatabase::InterfaceSupportsErrorInfo(REFIID riid)
{
   static const IID* arr[] = 
   {
      &IID_IInterfaceDatabase,
   };

   for (int i=0;i<sizeof(arr)/sizeof(arr[0]);i++)
   {
      if (InlineIsEqualGUID(*arr[i],riid))
         return S_OK;
   }
   return S_FALSE;   
}

InterfaceDatabase::InterfaceDatabase() :
   config_(nullptr),
   ini_file_settings_(nullptr)
{

}

InterfaceDatabase::~InterfaceDatabase()
{
   // A transaction still open is rolled back by transaction_'s own end.
}

bool 
InterfaceDatabase::LoadSettings()
{
   config_ = HM::Configuration::Instance();
   db_manager_ = HM::Application::Instance()->GetDBManager();
   ini_file_settings_ = HM::IniFileSettings::Instance();
   return true;
}

STDMETHODIMP InterfaceDatabase::get_RequiresUpgrade(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      HRESULT hRes = EnsureDatabaseConnectivity_();
      if (hRes != S_OK)
         return hRes;
      
      bool bUpgradeRequired = config_->GetRequiredDBVersion() > db_manager_->GetCurrentDatabaseVersion();
   
      *pVal = bUpgradeRequired ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDatabase::get_IsConnected(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      // The manager in force, not the one this object took when it was made - a
      // reinitialise since has replaced that one, and it is not connected.
      std::shared_ptr<HM::DatabaseConnectionManager> pDBManager = HM::Application::Instance()->GetDBManager();
      if (pDBManager)
         *pVal = pDBManager->GetIsConnected() ? VARIANT_TRUE : VARIANT_FALSE;
      else
         *pVal = VARIANT_FALSE;
   
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDatabase::get_DatabaseExists(VARIANT_BOOL *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      *pVal = ini_file_settings_->GetDatabaseSettingsExists() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDatabase::get_RequiredVersion(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      /*
         Server administration privileges should not be required for this function. Reason is that
         user should be able to determine whether a database upgrade is needed without entering his
         password. Because we don't want the user to enter his password during every installation
         unless there's changes in the database.
      */
   
      *pVal = config_->GetRequiredDBVersion();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDatabase::get_CurrentVersion(long *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      /*
         Server administration privileges should not be required for this function. Reason is that
         user should be able to determine whether a database upgrade is needed without entering his
         password. Because we don't want the user to enter his password during every installation
         unless there's changes in the database.
      */
   
      HRESULT hRes = EnsureDatabaseConnectivity_();
      if (hRes != S_OK)
         return hRes;
   
      *pVal = db_manager_->GetCurrentDatabaseVersion();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDatabase::get_DatabaseType(eDBtype *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      if (!GetIsServerAdmin())
         return GetAccessDenied();
   
      HM::DatabaseSettings::SQLDBType DBType = ini_file_settings_->GetDatabaseType();
      *pVal = (eDBtype) DBType;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDatabase::get_ServerName(BSTR *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      if (!GetIsServerAdmin())
         return GetAccessDenied();
   
      HM::String sServerName = ini_file_settings_->GetDatabaseServer();
   
      *pVal = sServerName.AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDatabase::get_DatabaseName(BSTR *pVal)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      if (!GetIsServerAdmin())
         return GetAccessDenied();
   
      HM::String sServerName = ini_file_settings_->GetDatabaseName();
   
      *pVal = sServerName.AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDatabase::BeginTransaction()
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      if (!GetIsServerAdmin())
         return GetAccessDenied();
   
      HRESULT hRes = EnsureDatabaseConnectivity_();
      if (hRes != S_OK)
         return hRes;
   
      HM::String sErrorMessage;
      if (!transaction_.Begin(db_manager_, sErrorMessage))
      {
         return COMError::GenerateError(sErrorMessage);
      }
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDatabase::CommitTransaction()
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      if (!GetIsServerAdmin())
         return GetAccessDenied();
   
      if (!transaction_.IsOpen())
         return COMError::GenerateError("No transaction started");
   
      // Rolled back and refused for a transaction the database has ended
      // (ScriptTransaction::Commit).
      HM::String sErrorMessage;
      if (!transaction_.Commit(sErrorMessage))
      {
         return COMError::GenerateError(sErrorMessage);
      }
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDatabase::RollbackTransaction()
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      if (!GetIsServerAdmin())
         return GetAccessDenied();
   
      if (!transaction_.IsOpen())
         return COMError::GenerateError("No transaction started");
   
      // Answered with success for a transaction the database has ended already
      // (ScriptTransaction::Rollback).
      HM::String sErrorMessage;
      if (!transaction_.Rollback(sErrorMessage))
      {
         return COMError::GenerateError(sErrorMessage);
      }
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDatabase::ExecuteSQL(BSTR sSQLStatement)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      return ExecuteSQLWithReturn(sSQLStatement, 0);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDatabase::ExecuteSQLWithReturn(BSTR sSQLStatement, long *uniqueID)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      if (!GetIsServerAdmin())
         return GetAccessDenied();
   
      if (uniqueID)
         *uniqueID = 0;
   
      HRESULT hRes = EnsureDatabaseConnectivity_();
      if (hRes != S_OK)
         return hRes;
   
      HM::String sStatement = sSQLStatement;
   
      HM::String sErrorMessage;
      if (transaction_.IsOpen())
      {
         // In the transaction BeginTransaction started: sent once, and refused
         // without being sent once the database has ended it (ScriptTransaction::
         // Execute), whose error is the whole of what the script is told.
         __int64 value = 0;
         if (transaction_.Execute(sStatement, &value, sErrorMessage) != HM::ScriptTransaction::Ran)
            return COMError::GenerateError(sErrorMessage);

         if (uniqueID)
            *uniqueID = (long) value;

         return S_OK;
      }
      else
      {
         __int64 value = 0;
   
         if (db_manager_->Execute(HM::SQLCommand(sStatement), &value, 0, sErrorMessage))
         {
            if (uniqueID)
               *uniqueID = (long) value;
   
            return S_OK;
         }
   
      }
   
      return COMError::GenerateError("Execution of SQL statement failed. Error: " + sErrorMessage);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDatabase::ExecuteSQLScript(BSTR sFilename)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      if (!GetIsServerAdmin())
         return GetAccessDenied();
   
      HRESULT hRes = EnsureDatabaseConnectivity_();
      if (hRes != S_OK)
         return hRes;
   
      HM::String sErrorMessage;
      if (transaction_.IsOpen())
      {
         // ScriptTransaction::ExecuteScript: stopped where it fails, and refused
         // once the database has ended the transaction.
         if (transaction_.ExecuteScript(sFilename, sErrorMessage) != HM::ScriptTransaction::Ran)
            return COMError::GenerateError(sErrorMessage);
      }
      else
      {
         if (!db_manager_->ExecuteScript(sFilename, sErrorMessage))
         {
            return COMError::GenerateError("Execution of SQL statements failed. Error: " + sErrorMessage);
         }
      }
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDatabase::UtilGetFileNameByMessageID(hyper lMessageID, BSTR *Output)
{
   try
   {
      if (!GetIsServerAdmin())
         return GetAccessDenied();
   
      HRESULT hRes = EnsureDatabaseConnectivity_();
      if (hRes != S_OK)
         return hRes;
   
      HM::String sSQL;
      sSQL.Format(_T("select messagefilename from hm_messages where messageid = %d"), lMessageID);
   
      std::shared_ptr<HM::DALRecordset> pRS = db_manager_->OpenRecordset(HM::SQLCommand(sSQL));
      if (!pRS)
         return S_OK;
   
      HM::String sRetVal = "";
      if (!pRS->IsEOF())
         sRetVal = pRS->GetStringValue("messagefilename");
   
   
      *Output = sRetVal.AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// What a database change is refused with while hMailServer.INI is read-only to
// the service's account: the installer's way, which sets the database up while
// the service still runs as LocalSystem and moves it afterwards, or the file.
static HM::String DatabaseRefusal_()
{
   return HM::IniFileSettings::InitializationFileRefusal(_T("How the server reaches its database"),
      _T("Set the database up while the service runs as LocalSystem, as the installer does: from an elevated prompt in the Bin folder, hMailServer.exe --service-account localsystem /Register, restart the service, run DBSetup.exe, then hMailServer.exe --confirm /GrantServiceAccess and hMailServer.exe --service-account virtual /Register, and restart it again. Or change [Database] in the file with an editor run as an administrator and restart the service."));
}

STDMETHODIMP InterfaceDatabase::CreateInternalDatabase()
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      if (!GetIsServerAdmin())
         return GetAccessDenied();

      // [Database] lives in hMailServer.INI, which the Windows service's own
      // account may not write (docs/ServiceAccount.md). Refused before anything
      // is created, so that no database is made that the file cannot name.
      if (!HM::IniFileSettings::InitializationFileWritable())
         return COMError::GenerateError(DatabaseRefusal_());

      // Make sure we have the latest settings.
      ini_file_settings_->LoadSettings();
   
      HM::String sDirectory = ini_file_settings_->GetDatabaseDirectory();
      HM::String sDatabaseName = "hMailServer";
      HM::String sPassword = HM::PasswordGenerator::Generate();
   
      HM::String sErrorMessage;
      if (!HM::SQLCEConnection::CreateDatabase(sDirectory, sDatabaseName, sPassword, sErrorMessage))
         return COMError::GenerateError(sErrorMessage);
   
      HM::String sEmpty;
   
      // Create a settings object which we use to connect to the server.
      std::shared_ptr<HM::DatabaseSettings> pSettings = std::shared_ptr<HM::DatabaseSettings>(
         new HM::DatabaseSettings(sEmpty, sEmpty, sDatabaseName, sEmpty, sPassword, sDirectory, sEmpty, HM::DatabaseSettings::TypeMSSQLCompactEdition, 0));
   
      // Connect to the new database
      std::shared_ptr<HM::DALConnection> pConn = HM::DALConnectionFactory::CreateConnection(pSettings);
   
      if (pConn->Connect(sErrorMessage) != HM::DALConnection::Connected)
      {
         return COMError::GenerateError(sErrorMessage);
      }
   
      // Create the tables
      HM::SQLScriptRunner scriptRunner;
      if (!scriptRunner.ExecuteScript(pConn, pSettings->GetDefaultScript(), sErrorMessage))
      {
         return COMError::GenerateError(sErrorMessage);
      }

      // A database made here is one a new installation's defaults may secure
      // (Common/Application/NewInstallDefaults.h), and the mark says so. Only the
      // two create methods write it - SetDefaultDatabase, which attaches a
      // database that already exists, does not - so the installer's run of
      // --apply-new-install-defaults never changes a database it did not make. A
      // mark that could not be written costs the defaults and nothing else, so it
      // does not fail the create; it is an error in the error log (HM7512), so
      // that the installer's exit 5 for this database has its reason written down.
      HM::String markError;
      if (!HM::NewInstallDefaults::MarkCreated(pConn, markError))
      {
         HM::String sentence = _T("The new database could not be marked as a new installation's (");
         sentence += markError;
         sentence += _T("), so --apply-new-install-defaults leaves it as the create script made it, its listeners without TLS; the setup checklist says what to do.");
         HM::ErrorManager::Instance()->ReportError(HM::ErrorManager::Medium, 7512, "InterfaceDatabase::CreateInternalDatabase", sentence);
      }
   
      ini_file_settings_->SetDatabaseDirectory(sDirectory);
      ini_file_settings_->SetDatabaseType(HM::DatabaseSettings::TypeMSSQLCompactEdition);
      ini_file_settings_->SetUsername("");
      ini_file_settings_->SetPassword(sPassword);
      ini_file_settings_->SetDatabasePort(0);
      ini_file_settings_->SetDatabaseServer("");
      ini_file_settings_->SetDatabaseName(sDatabaseName);
      ini_file_settings_->SetIsInternalDatabase(true);
   
      return S_OK;   
   }
   catch (_com_error &err)
   {
      _bstr_t bstrSource(err.Source());
      _bstr_t bstrDescription(err.Description());

      LPCSTR lpcSource = bstrSource;
      HM::String sErrSource = lpcSource;

      LPCSTR lpcDesc = bstrDescription;
      HM::String sErrDesc = lpcDesc;

      return COMError::GenerateGenericMessage();
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDatabase::CreateExternalDatabase(eDBtype ServerType, BSTR ServerName, long lPort, BSTR DatabaseName, BSTR Username, BSTR Password)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      if (!GetIsServerAdmin())
         return GetAccessDenied();

      // [Database] lives in hMailServer.INI, which the Windows service's own
      // account may not write (docs/ServiceAccount.md). Refused before anything
      // is created, so that no database is made that the file cannot name.
      if (!HM::IniFileSettings::InitializationFileWritable())
         return COMError::GenerateError(DatabaseRefusal_());

      const HM::String sEmpty;
      const HM::String sServerName = ServerName;
      const HM::String sDatabaseName = DatabaseName;
      const HM::String sUsername = Username;
      const HM::String sPassword = Password;
      const HM::String sProvider = ini_file_settings_->GetDatabaseProvider();

      if (sDatabaseName.Find(_T(" ")) >= 0)
         return COMError::GenerateError("The database name may not contain spaces.");
   
      // Create a settings object for the connection ...
      std::shared_ptr<HM::DatabaseSettings> pSettings = std::shared_ptr<HM::DatabaseSettings>(
         new HM::DatabaseSettings(sProvider, sServerName, sEmpty, sUsername, sPassword, sEmpty, sEmpty, (HM::DatabaseSettings::SQLDBType) ServerType, lPort));
   
      // Connect to the database serve   
      std::shared_ptr<HM::DALConnection> pConn = HM::DALConnectionFactory::CreateConnection(pSettings);
   
      HM::String sErrorMessage;
      if (pConn->Connect(sErrorMessage) != HM::DALConnection::Connected)
         return COMError::GenerateError(sErrorMessage);
   
      // Create the database
      HM::String sCreateDB = HM::SQLStatement::GetCreateDatabase(pSettings, sDatabaseName);
   
      if (!pConn->Execute(HM::SQLCommand(sCreateDB), sErrorMessage, 0, 0))
         return COMError::GenerateError(sErrorMessage);
   
      // Disconnect again.
      pConn->Disconnect();
   
      // Create a new settings object where we specify the database name as well.
      pSettings = std::shared_ptr<HM::DatabaseSettings>(
         new HM::DatabaseSettings(sProvider, sServerName, sDatabaseName, sUsername, sPassword, sEmpty, sEmpty, (HM::DatabaseSettings::SQLDBType) ServerType, lPort));
   
      // Reconnect to the new database.
      pConn = HM::DALConnectionFactory::CreateConnection(pSettings);
      if (pConn->Connect(sErrorMessage) != HM::DALConnection::Connected)
         return COMError::GenerateError(sErrorMessage);
   
      // Run the scripts
      HM::SQLScriptRunner scriptRunner;
      if (!scriptRunner.ExecuteScript(pConn, pSettings->GetDefaultScript(), sErrorMessage))
         return COMError::GenerateError(sErrorMessage);

      // A database made here is one a new installation's defaults may secure
      // (Common/Application/NewInstallDefaults.h), and the mark says so. Only the
      // two create methods write it - SetDefaultDatabase, which attaches a
      // database that already exists, does not - so the installer's run of
      // --apply-new-install-defaults never changes a database it did not make. A
      // mark that could not be written costs the defaults and nothing else, so it
      // does not fail the create; it is an error in the error log (HM7512), so
      // that the installer's exit 5 for this database has its reason written down.
      HM::String markError;
      if (!HM::NewInstallDefaults::MarkCreated(pConn, markError))
      {
         HM::String sentence = _T("The new database could not be marked as a new installation's (");
         sentence += markError;
         sentence += _T("), so --apply-new-install-defaults leaves it as the create script made it, its listeners without TLS; the setup checklist says what to do.");
         HM::ErrorManager::Instance()->ReportError(HM::ErrorManager::Medium, 7512, "InterfaceDatabase::CreateExternalDatabase", sentence);
      }

      ini_file_settings_->SetDatabaseDirectory("");
      ini_file_settings_->SetDatabaseType((HM::DatabaseSettings::SQLDBType) ServerType);
      ini_file_settings_->SetUsername(sUsername);
      ini_file_settings_->SetPassword(sPassword);
      ini_file_settings_->SetDatabasePort(lPort);
      ini_file_settings_->SetDatabaseServer(sServerName);
      ini_file_settings_->SetDatabaseName(sDatabaseName);
      ini_file_settings_->SetIsInternalDatabase(false);
   
      return S_OK;   
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDatabase::SetDefaultDatabase(eDBtype ServerType, BSTR ServerName, long lPort, BSTR DatabaseName, BSTR Username, BSTR Password)
{
   try
   {
      if (!GetIsServerAdmin())
         return GetAccessDenied();

      // [Database] lives in hMailServer.INI, which the Windows service's own
      // account may not write (docs/ServiceAccount.md). Refused before anything
      // is created, so that no database is made that the file cannot name.
      if (!HM::IniFileSettings::InitializationFileWritable())
         return COMError::GenerateError(DatabaseRefusal_());

      HM::String sEmpty;
      HM::String sServerName = ServerName;
      HM::String sDatabaseName = DatabaseName;
      HM::String sUsername = Username;
      HM::String sPassword = Password;
   
      if (sDatabaseName.Find(_T(" ")) >= 0)
         return COMError::GenerateError("The database name may not contain spaces.");
   
      // Create a settings object for the connection ...
      HM::String sProvider = ini_file_settings_->GetDatabaseProvider();

      std::shared_ptr<HM::DatabaseSettings> pSettings = std::shared_ptr<HM::DatabaseSettings>(
         new HM::DatabaseSettings(sProvider, sServerName, sEmpty, sUsername, sPassword, sEmpty, sEmpty, (HM::DatabaseSettings::SQLDBType) ServerType, lPort));
   
      // Connect to the database server.
      std::shared_ptr<HM::DALConnection> pConn = HM::DALConnectionFactory::CreateConnection(pSettings);
   
      HM::String sErrorMessage;
      if (pConn->Connect(sErrorMessage) != HM::DALConnection::Connected)
         return COMError::GenerateError(sErrorMessage);
   
      // Disconnect again.
      pConn->Disconnect();
   
      ini_file_settings_->SetDatabaseDirectory("");
      ini_file_settings_->SetDatabaseType((HM::DatabaseSettings::SQLDBType) ServerType);
      ini_file_settings_->SetUsername(sUsername);
      ini_file_settings_->SetPassword(sPassword);
      ini_file_settings_->SetDatabasePort(lPort);
      ini_file_settings_->SetDatabaseServer(sServerName);
      ini_file_settings_->SetDatabaseName(sDatabaseName);
      ini_file_settings_->SetIsInternalDatabase(ServerType == hDBTypeMSSQLCE);

      // The database named exists already, and may be older than this build:
      // DBSetup restarts the server on it next (Reinitialize), reads its version,
      // and upgrades an older one with DBUpdater - the way an installer's
      // /createdatabase=0 finishes. That restart's refusal of an older database is
      // then expected, as at the installer's start for an upgrade, and is not
      // reported as HM5011 (Application::SetStartedForDatabaseUpgrade); it is
      // still what Reinitialize answers, and DBSetup's wizard shows it. Once: any
      // later start or reinitialise reports it.
      HM::Application::Instance()->SetStartedForDatabaseUpgrade();

      return S_OK;   
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}   
   
HRESULT
InterfaceDatabase::EnsureDatabaseConnectivity_()
{
   db_manager_ = HM::Application::Instance()->GetDBManager();
   if (!db_manager_)
   {
      return COMError::GenerateError("The connection to the database is not available. Please check the hMailServer error log for details.");
   }

   if (!db_manager_->GetIsConnected())
   {
      HM::String sError;
      sError.Format(_T("The connection to the database is not available.\r\n%s"), HM::Application::Instance()->GetLastErrorMessage().c_str());

      return COMError::GenerateError(sError);
   }

   return S_OK;
}

STDMETHODIMP InterfaceDatabase::EnsurePrerequisites(long DBVersion)
{
   try
   {
      if (!config_)
         return GetAccessDenied();

      if (!GetIsServerAdmin())
         return GetAccessDenied();
   
      HM::String sErrorMessage;
   
      if (transaction_.IsOpen())
      {
         // ScriptTransaction::EnsurePrerequisites: refused once the database has
         // ended the transaction.
         if (transaction_.EnsurePrerequisites(DBVersion, sErrorMessage) != HM::ScriptTransaction::Ran)
            return COMError::GenerateError(sErrorMessage);
      }
      else
      {
         // The manager in force, not the one this object took when it was made,
         // which a reinitialise since has replaced.
         std::shared_ptr<HM::DatabaseConnectionManager> manager = HM::Application::Instance()->GetDBManager();
         if (!manager)
            return COMError::GenerateError("The connection to the database is not available. Please check the hMailServer error log for details.");

         if (!manager->EnsuresPrerequisites(DBVersion, sErrorMessage))
         {
            return COMError::GenerateError("Execution of SQL statements failed. Error: " + sErrorMessage);
         }
      }
     
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

