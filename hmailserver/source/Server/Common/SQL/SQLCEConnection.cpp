// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "SQLCEConnection.h"
#include "SQLCERecordset.h"
#include "Macros/SQLCEMacroExpander.h"
#include "DatabaseSettings.h"
#include "ADOInt64Helper.h"
#include "ADOConnection.h"
#include "../Util/Time.h"
#include "../Util/Unicode.h"
#include "../Util/GUIDCreator.h"

#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <boost/filesystem.hpp>
// The SQL Server Compact SDK headers, which ship with the Windows redistributable
// and exist nowhere else. Guarded for the same reason the body below is; see the
// note above the namespace.
#ifdef _MSC_VER
#include <sqlce_oledb.h>
#include <sqlce_err.h>
#include <sqlce_sync.h>
#endif

using namespace std;

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

// ADO and SQL Server Compact are COM, and the roadmap section "Linux and
// AArch64" - the row "Database backends that survive" - leaves both out of the
// POSIX build, where MySQL and PostgreSQL are the two backends that work. Every
// member below is written in terms of the ADO smart pointers the Windows
// precompiled header #imports (_ConnectionPtr, _RecordsetPtr, _CommandPtr),
// which have no POSIX shape at all, so the file is Windows-only in its entirety
// rather than a set of stubs that would look like a database backend.
//
// A POSIX build asked for either backend is refused by name in
// DALConnectionFactory::CreateConnection, which reports HM6390 saying which
// backend was refused and why - so nothing here is reached silently, and a link
// error rather than a stub is what a mistaken caller would get.
#ifndef HM_PLATFORM_POSIX

namespace HM
{
   namespace
   {
      // Source/description of a COM error as one line, for the transaction
      // functions below. err.Source()/err.Description() hand back BSTRs owned by
      // _bstr_t temporaries, so they are copied out before those go out of
      // scope, and a null one becomes an empty string rather than a fault.
      String DescribeComError(_com_error &err)
      {
         _bstr_t bstrSource(err.Source());
         _bstr_t bstrDescription(err.Description());

         LPCSTR lpcSource = bstrSource;
         LPCSTR lpcDescription = bstrDescription;

         String source;
         if (lpcSource != nullptr)
            source = lpcSource;

         String description;
         if (lpcDescription != nullptr)
            description = lpcDescription;

         String result;
         result.Format(_T("Source: %s, Description: %s"), source.c_str(), description.c_str());

         return result;
      }
   }

   SQLCEConnection::SQLCEConnection(std::shared_ptr<DatabaseSettings> pSettings) :
      DALConnection(pSettings)
   {
      HRESULT hr =cSQLCEConnection.CreateInstance(__uuidof(Connection));
      
      if ( !SUCCEEDED( hr ) )
      {
         HM_ASSERT(0);
      }  

      connected_ = false;
   }


   SQLCEConnection::~SQLCEConnection()
   {
   
      try
      {
         cSQLCEConnection = NULL;
      }
      catch(...)
      {
      
      } 
   
   }


   bool
   SQLCEConnection::Disconnect()
   {
      try
      {

         if (!connected_)
            return false; // --- already disconnected.

         if (cSQLCEConnection->State == 1)
            cSQLCEConnection->Close();
      }
      catch ( ... )
      {
         HM_ASSERT(0);
      }

      connected_ = false;

      return true;
   }

   

   String 
   SQLCEConnection::GetConnectionString_(const String &sDatabase, const String &sPassword) const
   {
      return MakeConnectionString(GetDatabaseFileName_(sDatabase), sPassword);
   }

   String
   SQLCEConnection::MakeConnectionString(const String &databaseFile, const String &password)
   {
      String quotedFile;
      if (!ADOConnection::QuoteValue(databaseFile, quotedFile))
         quotedFile = databaseFile;

      String quotedPassword;
      if (!ADOConnection::QuoteValue(password, quotedPassword))
         quotedPassword = password;

      // The flush interval is the one the close before the process ends waits
      // out for a connection still in use (DatabaseSettings::SqlCeFlushIntervalSeconds).
      const int flushIntervalSeconds = DatabaseSettings::SqlCeFlushIntervalSeconds;

      String sConnectionString;
      sConnectionString.Format(_T("Provider=Microsoft.SQLSERVER.CE.OLEDB.4.0;Data Source=%s;SSCE:Database Password=%s;SSCE:Max Database Size=4000;SSCE:Flush Interval=%d;"), quotedFile.c_str(), quotedPassword.c_str(), flushIntervalSeconds);

      return sConnectionString;
   }


   String 
   SQLCEConnection::GetDatabaseFileName_(const String &sShortName) const
   {
      String sDatabaseDirectory = database_settings_->GetDatabaseDirectory();
      String sDatabaseFile = sDatabaseDirectory + "\\" + sShortName + ".sdf";

      return sDatabaseFile;
   }

   bool 
   SQLCEConnection::GetRequiresUpgrade_(const String &sConnectionString)
   {
      BSTR sUsername = 0;
      BSTR sPassword = 0;
      BSTR bsConnection = sConnectionString.AllocSysString();
      
      bool bResult = false;

      try
      {
         cSQLCEConnection->Open( bsConnection, sUsername, sPassword, NULL );
         cSQLCEConnection->Close();
      }
      catch (_com_error &err)
      {
         if (ErrorManager::GetNativeErrorCode(err.ErrorInfo()) == SSCE_M_DBUPGRADENEEDED)
         {
            bResult = true;
         }
      }
      
      ::SysFreeString(bsConnection);

      return bResult;

   }

   DALConnection::ConnectionResult
   SQLCEConnection::Connect(String &sErrorMessage)
   {
      String sPassword = database_settings_->GetPassword();
      String sDatabase = database_settings_->GetDatabaseName();

      if (connected_)
         return Connected;
      
      String sConnectionString = GetConnectionString_(sDatabase, sPassword);
      
      /*
         Check if the database engine needs to be upgraded.
      */
      if (GetRequiresUpgrade_(sConnectionString))
      {
         String upgradeError;
         if (!UpgradeDatabase_(upgradeError))
         {
            // Said here as well as in the error log, because this is the sentence
            // the start's own failure (HM4354) carries: it was empty. The upgrade's
            // own, word for word, since only it says where the file is: left as it
            // was on every path but one, where the upgraded copy could not take its
            // place and the 3.5 file could not be put back, and the sentence names
            // both files.
            sErrorMessage = upgradeError;
            return FatalError;
         }
      }

      DALConnection::ConnectionResult result = DALConnection::FatalError;

      try
      {
         BSTR sUsername = 0;
         BSTR sPassword = 0;
         BSTR bsConnection = sConnectionString.AllocSysString();
         HRESULT hr = cSQLCEConnection->Open( bsConnection, sUsername, sPassword, NULL );

         if ( !SUCCEEDED( hr ) )
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5097, "SQLCEConnection::Connect", "Open failed");
            return TemporaryFailure;
         }   

         SysFreeString(bsConnection);


         connected_ = true;
         sErrorMessage = "";
         
         return DALConnection::Connected;
      }
      catch ( _com_error &err )
      {
         String sErrSource = err.Source();
         String sErrDesc =  err.Description();

         int iNativeErrorCode = ErrorManager::GetNativeErrorCode(err.ErrorInfo());
         sErrorMessage.Format(_T("Database connection error. Source: %s, Error: %d, Description: %s Check database settings in hMailServer.ini."), sErrSource.c_str(), iNativeErrorCode, sErrDesc.c_str());

         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5098, "SQLCEConnection::Connect", sErrorMessage);

         return TemporaryFailure;
      }
      catch( ... )
      {
         sErrorMessage = "ADO: Unknown database connection error";
         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5099, "SQLCEConnection::Connect", "An unknown error occurred when connecting to database. Check your database settings in hMailServer.ini.");

         return TemporaryFailure;
      }

   }

   namespace
   {
      String DescribeHresult(HRESULT hr)
      {
         String text;
         text.Format(_T("0x%08X"), (unsigned int) hr);
         return text;
      }

      // What the engine recorded about its last call, as "native error:
      // description" phrases, or nothing when it kept no record. The engine's
      // HRESULT alone is E_FAIL for a damaged file, a file in use and a file
      // already at 4.0 alike.
      String DescribeEngineErrors(ISSCEEngine *engine)
      {
         String result;

         ISSCEErrors *errors = nullptr;
         if (FAILED(engine->get_ErrorRecords(&errors)) || errors == nullptr)
            return result;

         long count = 0;
         if (SUCCEEDED(errors->get_Count(&count)))
         {
            for (long i = 0; i < count && i < 4; i++)
            {
               VARIANT index;
               VariantInit(&index);
               index.vt = VT_I4;
               index.lVal = i;

               ISSCEError *error = nullptr;
               if (FAILED(errors->get_Item(index, &error)) || error == nullptr)
                  continue;

               long native = 0;
               error->get_NativeError(&native);

               BSTR description = nullptr;
               error->get_Description(&description);

               String phrase;
               phrase.Format(_T("%ld: %s"), native, description != nullptr ? (const wchar_t *) description : L"");
               if (!result.IsEmpty())
                  result += _T("; ");
               result += phrase;

               if (description != nullptr)
                  SysFreeString(description);
               error->Release();
            }
         }

         errors->Release();
         return result;
      }

      // A file removed, saying why not rather than reporting it: the upgrade
      // reports once, with the whole story (FileUtilities::DeleteFile reports
      // on its own, and the tester's cases must leave the error log alone).
      bool RemoveFile(const String &file, String &error)
      {
         for (int attempt = 1; ; attempt++)
         {
            boost::system::error_code code;
            boost::filesystem::remove(boost::filesystem::path(std::wstring(file)), code);
            if (!code)
               return true;

            if (attempt == 5)
            {
               error = String(_T("it could not be removed (")) + String(code.message().c_str()) + _T(")");
               return false;
            }

            // A scanner or a backup holding it a moment.
            Sleep(250);
         }
      }

      // A file renamed, as FileUtilities::Move does it, without its report.
      bool RenameFile(const String &from, const String &to, String &error)
      {
         for (int attempt = 1; ; attempt++)
         {
            boost::system::error_code code;
            boost::filesystem::rename(boost::filesystem::path(std::wstring(from)), boost::filesystem::path(std::wstring(to)), code);
            if (!code)
               return true;

            if (attempt == 5)
            {
               error = String(_T("it could not be renamed (")) + String(code.message().c_str()) + _T(")");
               return false;
            }

            Sleep(250);
         }
      }
   }

   bool
   SQLCEConnection::UpgradeDatabase_(String &error)
   {
      const String name = database_settings_->GetDatabaseName();
      const String liveFile = GetDatabaseFileName_(name);
      const String upgradedFile = GetDatabaseFileName_(name + "_upgraded");

      String backupFile;
      int errorCode = 0;
      if (!UpgradeDatabaseFile_(liveFile, upgradedFile, database_settings_->GetPassword(), backupFile, error, errorCode))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Critical, errorCode, "SQLCEConnection::UpgradeDatabase_()", error);
         return false;
      }

      LOG_APPLICATION(_T("SQL Server Compact: the database file ") + liveFile + _T(" was upgraded from 3.5 to 4.0. The 3.5 file is kept as ") + backupFile + _T("."));
      return true;
   }

   bool
   SQLCEConnection::UpgradeDatabaseFile_(const String &liveFile, const String &upgradedFile, const String &password,
                                         String &backupFile, String &error, int &errorCode)
   {
      // A copy an earlier attempt left behind - a start that stopped half way.
      // The live file is the 3.5 one (Connect would not be here otherwise), so
      // the copy is nothing to keep, and the engine refuses to write over it:
      // left, it would fail every upgrade from now on.
      String removeError;
      if (FileUtilities::Exists(upgradedFile) && !RemoveFile(upgradedFile, removeError))
      {
         errorCode = 5123;
         error = _T("The database file ") + liveFile + _T(" is a SQL Server Compact 3.5 database, and the upgraded copy an earlier attempt left, ") + upgradedFile +
            _T(", stands where the upgrade writes its copy: ") + removeError + _T(". The database file is left as it was. Remove that copy and start the service again.");
         return false;
      }

      ISSCEEngine *engine = NULL;
      HRESULT hr = CoCreateInstance(CLSID_Engine, NULL, CLSCTX_INPROC_SERVER, IID_ISSCEEngine, (void**)&engine);
      if (FAILED(hr) || engine == NULL)
      {
         errorCode = 5122;
         error = _T("The database file ") + liveFile + _T(" is a SQL Server Compact 3.5 database, and SQL Server Compact 4.0's engine, which upgrades it, could not be created (") +
            DescribeHresult(hr) + _T("). The database file is left as it was. Is SQL Server Compact 4.0 installed for this platform?");
         return false;
      }

      HRESULT result = engine->UpgradeDatabase(_bstr_t(MakeConnectionString(liveFile, password).c_str()), _bstr_t(MakeConnectionString(upgradedFile, password).c_str()));
      const String engineSaid = FAILED(result) ? DescribeEngineErrors(engine) : String();
      engine->Release();

      // The engine's own answer. Until 6.3.6 this read the HRESULT of creating
      // the engine instead, which had succeeded, so a failed upgrade went on to
      // move the live file aside and put nothing in its place.
      if (FAILED(result))
      {
         // Whatever the engine wrote of a copy is not a database to keep.
         String ignored;
         if (FileUtilities::Exists(upgradedFile))
            RemoveFile(upgradedFile, ignored);

         errorCode = 5123;
         error = _T("SQL Server Compact could not upgrade the database file ") + liveFile + _T(" from 3.5 to 4.0 (") + DescribeHresult(result) +
            (engineSaid.IsEmpty() ? String() : _T("; the engine said ") + engineSaid) +
            _T("). The database file is left as it was and nothing was moved; the service does not start on it, and tries the upgrade again at its next start. ")
            _T("A file another program holds open is upgraded once that program lets it go; a damaged one needs SQL Server Compact 3.5's repair, on a copy, first.");
         return false;
      }

      if (!InstallUpgradedCopy_(liveFile, upgradedFile, password, backupFile, error, errorCode))
         return false;

      return true;
   }

   bool
   SQLCEConnection::ProveDatabaseFile_(const String &file, const String &password, String &error)
   {
      const String connectionString = MakeConnectionString(file, password);

      ISSCEEngine *engine = NULL;
      HRESULT hr = CoCreateInstance(CLSID_Engine, NULL, CLSCTX_INPROC_SERVER, IID_ISSCEEngine, (void**)&engine);
      if (FAILED(hr) || engine == NULL)
      {
         error = _T("SQL Server Compact 4.0's engine could not be created to check it (") + DescribeHresult(hr) + _T(")");
         return false;
      }

      // Every page read and its checksum compared.
      HRESULT verified = engine->Verify(_bstr_t(connectionString.c_str()));
      const String engineSaid = FAILED(verified) ? DescribeEngineErrors(engine) : String();
      engine->Release();

      if (FAILED(verified))
      {
         error = _T("the engine's check of every page failed (") + DescribeHresult(verified) +
            (engineSaid.IsEmpty() ? String() : _T("; the engine said ") + engineSaid) + _T(")");
         return false;
      }

      // And opened by the provider, as Connect is about to open it.
      _ConnectionPtr connection;
      if (FAILED(connection.CreateInstance(__uuidof(Connection))))
      {
         error = _T("no ADO connection could be made to open it");
         return false;
      }

      try
      {
         connection->Open(_bstr_t(connectionString.c_str()), _bstr_t(), _bstr_t(), adConnectUnspecified);
         connection->Close();
      }
      catch (_com_error &err)
      {
         error = _T("the provider could not open it: ") + DescribeComError(err);
         return false;
      }

      return true;
   }

   bool
   SQLCEConnection::InstallUpgradedCopy_(const String &liveFile, const String &upgradedFile, const String &password,
                                         String &backupFile, String &error, int &errorCode)
   {
      errorCode = 5124;

      String why;
      String ignored;

      if (!ProveDatabaseFile_(upgradedFile, password, why))
      {
         RemoveFile(upgradedFile, ignored);
         error = _T("SQL Server Compact upgraded the database file ") + liveFile + _T(" from 3.5 to 4.0, but the upgraded copy is not sound: ") + why +
            _T(". The copy is removed, and the database file is left as it was; the service does not start on it, and tries the upgrade again at its next start.");
         return false;
      }

      // Where the 3.5 file goes: .backup_v52, as it always was, or - when an
      // earlier upgrade's backup is already there - the first .backup_v52.<n>
      // that holds nothing, so that no earlier backup is written over.
      backupFile = liveFile + _T(".backup_v52");
      for (int n = 2; FileUtilities::Exists(backupFile); n++)
      {
         if (n > 99)
         {
            RemoveFile(upgradedFile, ignored);
            error = _T("The database file ") + liveFile + _T(" was upgraded from 3.5 to 4.0, but there is no free name for its backup: ") + liveFile +
               _T(".backup_v52 to .backup_v52.99 all exist. The upgraded copy is removed and the database file is left as it was. Move the old backups away and start the service again.");
            return false;
         }
         backupFile = liveFile + _T(".backup_v52.") + StringParser::IntToString(n);
      }

      // The 3.5 file aside first: a rename fails without touching it.
      if (!RenameFile(liveFile, backupFile, why))
      {
         RemoveFile(upgradedFile, ignored);
         error = _T("The database file ") + liveFile + _T(" was upgraded from 3.5 to 4.0, but the 3.5 file could not be moved aside to ") + backupFile + _T(": ") + why +
            _T(". The upgraded copy is removed and the database file is left as it was; the service tries the upgrade again at its next start.");
         return false;
      }

      // Then the proven copy into its place - or, if it cannot go there, the
      // 3.5 file back where it was.
      if (!RenameFile(upgradedFile, liveFile, why))
      {
         errorCode = 5125;

         String back;
         if (RenameFile(backupFile, liveFile, back))
         {
            RemoveFile(upgradedFile, ignored);
            error = _T("The database file ") + liveFile + _T(" was upgraded from 3.5 to 4.0, but the upgraded copy could not take its place: ") + why +
               _T(". The 3.5 file is back where it was; the service tries the upgrade again at its next start.");
         }
         else
         {
            error = _T("The database file ") + liveFile + _T(" was upgraded from 3.5 to 4.0, but the upgraded copy could not take its place (") + why +
               _T("), and the 3.5 file could not be put back (") + back + _T("). The 3.5 file is ") + backupFile + _T(" and the upgraded, checked copy is ") + upgradedFile +
               _T(": rename the copy to ") + liveFile + _T(" and start the service again.");
         }
         return false;
      }

      return true;
   }

   _ConnectionPtr
   SQLCEConnection::GetConnection() const
   {
      return cSQLCEConnection;
   }

   DALConnection::ExecutionResult
   SQLCEConnection::TryExecute(const SQLCommand &command, String &sErrorMessage, __int64 *iInsertID, int iIgnoreErrors, __int64 *rowsAffected)
   {
      if (rowsAffected != nullptr)
         *rowsAffected = -1;

      if (!connected_)
         return DALConnection::DALConnectionProblem; // --- already disconnected.

      String queryString = command.GetQueryString();

      try
      {

         _CommandPtr adoCommand;
         adoCommand.CreateInstance(__uuidof(Command));

         InitializeCommandParameters(adoCommand, command, queryString);

         adoCommand->CommandType = adCmdText;
         adoCommand->CommandText = _bstr_t(queryString);
         adoCommand->ActiveConnection = cSQLCEConnection;
         adoCommand->NamedParameters = true;

         // RecordsAffected: the rows the statement matched (DALConnection.h). The
         // Compact provider counts a row an UPDATE matched and left as it was, as
         // SQL Server does (measured on 4.0.8876.1, 12 October 2026).
         _variant_t recordsAffected;
         adoCommand->Execute(&recordsAffected, NULL, adCmdText);
         adoCommand->ActiveConnection = NULL;

         if (rowsAffected != nullptr)
            *rowsAffected = ADOConnection::ReadRecordsAffected(recordsAffected);

         if (iInsertID > 0)
         {
            _RecordsetPtr pIdentityRS;
            BSTR bsIdentity;

            pIdentityRS.CreateInstance(__uuidof(Recordset));
            pIdentityRS->PutRefActiveConnection(cSQLCEConnection); 
            String sIdentitySQL = "SELECT @@IDENTITY AS IDENT";
            bsIdentity = sIdentitySQL.AllocSysString();

            HRESULT hr = pIdentityRS->Open( bsIdentity, vtMissing, adOpenForwardOnly, adLockUnspecified, -1);
                        
            *iInsertID = GetIdentityFromRS_(pIdentityRS);
            pIdentityRS->Close();
            pIdentityRS->PutRefActiveConnection(NULL); 

            ::SysFreeString( bsIdentity );
         }
      }
      catch ( _com_error &err )
      {
         // A failure, ignored or not, leaves no count of its own: a statement
         // that fails changes no row (DALConnection.h).
         if (rowsAffected != nullptr)
            *rowsAffected = -1;

         ExecutionResult dbErr = GetErrorType_(_com_error::WCodeToHRESULT(err.WCode()));

         // A caller that passes DALErrorInSQL forgives an integrity violation and
         // nothing else (DALConnection::TryExecute), as ADOConnection does. This was
         // "iIgnoreErrors & dbErr": ExecutionResult is not a set of bits - DALUnknown,
         // 3, has DALErrorInSQL's bit - and every failure here but an integrity
         // violation is DALUnknown, so every failure was forgiven and answered as done.
         if ((iIgnoreErrors & DALConnection::DALErrorInSQL) != 0 && dbErr == DALConnection::DALErrorInSQL)
         {
            if (rowsAffected != nullptr)
               *rowsAffected = 0;

            return DALConnection::DALSuccess;
         }

         // The marker means "this object may already exist". It must not also
         // discard a lost connection - see DALConnection::HasIgnoreErrorsMarker.
         // GetErrorType_ never classifies anything as DALConnectionProblem on
         // this backend today, so the test costs nothing here and stops the next
         // person who adds a mapping from re-opening the hole.
         if (dbErr != DALConnection::DALConnectionProblem &&
             HasIgnoreErrorsMarker(queryString))
         {
            if (rowsAffected != nullptr)
               *rowsAffected = 0;

            return DALConnection::DALSuccess;
         }

         _bstr_t bstrSource( err.Source() );
         _bstr_t bstrDescription( err.Description() );

         LPCSTR lpcSource = bstrSource;
         String sErrSource = lpcSource;

         LPCSTR lpcDesc = bstrDescription;
         String sErrDesc = lpcDesc;

         sErrorMessage = "Source: SQLCEConnection::Execute(), Code: HM10044, Description: Error while executing SQL statement: \n";
         sErrorMessage += queryString;
         sErrorMessage += "\n";
         sErrorMessage += sErrSource;
         sErrorMessage += "\n";
         sErrorMessage += sErrDesc;

         return dbErr;
      }
      catch ( ... )
      {
         if (rowsAffected != nullptr)
            *rowsAffected = -1;

         sErrorMessage = "Source: SQLCEConnection::Execute(), Code: HM10045, Description: Unknown error when executing SQL statement: \n";
         sErrorMessage.append(queryString);

         return DALConnection::DALUnknown;
      }

      return DALConnection::DALSuccess;
   }

   bool
   SQLCEConnection::IsConnected() const
   { 
      return connected_; 
   };

   __int64 
   SQLCEConnection::GetIdentityFromRS_(_RecordsetPtr pRS) const
   {
      try
      {
         if (pRS->GetadoEOF())
            return 0;
         
         _variant_t    vaField;
         FieldsPtr      pFields;
         FieldPtr       pField;
         HRESULT        hr;

         hr = pRS->get_Fields( &pFields );
         vaField = "IDENT";
         pFields->get_Item( vaField, &pField );

         vaField.Clear();
         if (pField)
         {
            pField->get_Value(&vaField);

            switch (vaField.vt)
            {
            case VT_NULL:
               return 0;
            case VT_I4:
               return vaField.intVal;
            default:
               if (vaField.decVal.sign == DECIMAL_NEG)
                  return -vaField.cyVal.int64;
               else
                  return vaField.cyVal.int64;
            }
         }

         return 0;
      }
      catch (...)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5100, "SQLCEConnection::GetIdentityFromRS_", "Error while determening @@IDENTITY");
         throw;
      }

      return 0;
   }

   void 
   SQLCEConnection::OnConnected()
   {
      // Nothing to do here. Only used my MySQL.
   }

   DALConnection::ExecutionResult 
   SQLCEConnection::GetErrorType_(int iErrorCode)
   {
      switch (iErrorCode)
      {
      case 0:
         return DALConnection::DALSuccess;
      case 0x80040E2FL: // DB_E_INTEGRITYVIOLATION:
         return DALConnection::DALErrorInSQL;
      default:
         return DALConnection::DALUnknown;
      }
   }

   bool 
   SQLCEConnection::CreateDatabase(const String &sPath, const String &sFile, const String &sPassword, String &sErrorMessage)
   {
      String sFullPath = sPath + "\\" + sFile + ".sdf";

      if (FileUtilities::Exists(sFullPath))
      {
         sErrorMessage.Format(_T("Creation of SQL database failed. The database file, %s, already exists."), sFullPath.c_str());
         return false;
      }

      // Object declarations
      HRESULT            hr                   = NOERROR; 
      DBPROPSET          dbpropset[2]; 
      DBPROP             dbprop[1]; // Property array to initialize the provider.
      DBPROP             sscedbprop[1]; // Property array for SSCE security properties
      INT                i                    = 0;
      IDBDataSourceAdmin *pIDBDataSourceAdmin = NULL; 
      IUnknown           *pIUnknownSession    = NULL;

      // Create an instance of the OLE DB provider.
      hr = CoCreateInstance(CLSID_SQLSERVERCE, 0, CLSCTX_INPROC_SERVER,
         IID_IDBDataSourceAdmin, (void**)& pIDBDataSourceAdmin);
      if(FAILED(hr))
      {
         sErrorMessage.Format(_T("CoCreateInstance failed. Result: %d, Last error: %d"), hr, GetLastError());
         return false;
      }

      // Initialize property structures.
      VariantInit(&dbprop[0].vValue);
      for (int i = 0; i < sizeof(sscedbprop) / sizeof(sscedbprop[0]); i++)
      {
         VariantInit(&sscedbprop[i].vValue);
      }

      // Specify the property with name of the database.
      dbprop[0].dwPropertyID  = DBPROP_INIT_DATASOURCE;
      dbprop[0].dwOptions   = DBPROPOPTIONS_REQUIRED;
      dbprop[0].vValue.vt   = VT_BSTR;
      dbprop[0].vValue.bstrVal = SysAllocString(sFullPath); 
      if(NULL == dbprop[0].vValue.bstrVal)
      {
         hr = E_OUTOFMEMORY;
         sErrorMessage.Format(_T("SysAllocString failed. Out of memory"));
         return false;
      }


      // Specify the password.
      sscedbprop[0].dwPropertyID = DBPROP_SSCE_DBPASSWORD;
      sscedbprop[0].dwOptions = DBPROPOPTIONS_REQUIRED;
      sscedbprop[0].vValue.vt = VT_BSTR;
      sscedbprop[0].vValue.bstrVal = SysAllocString(sPassword);
      if(NULL == sscedbprop[0].vValue.bstrVal)
      {
         hr = E_OUTOFMEMORY;
         sErrorMessage.Format(_T("SysAllocString failed. Out of memory"));
         return false;
      }


      // Initialize the property sets.
      dbpropset[0].guidPropertySet = DBPROPSET_DBINIT;
      dbpropset[0].rgProperties  = dbprop;
      dbpropset[0].cProperties  = sizeof(dbprop)/sizeof(dbprop[0]);

      dbpropset[1].guidPropertySet = DBPROPSET_SSCE_DBINIT;
      dbpropset[1].rgProperties  = sscedbprop;
      dbpropset[1].cProperties  = sizeof(sscedbprop)/sizeof(sscedbprop[0]);

      // Create and initialize the database.
      hr = pIDBDataSourceAdmin->CreateDataSource(sizeof(dbpropset)/sizeof(dbpropset[0]),
         dbpropset, NULL, IID_IUnknown, &pIUnknownSession);

      if(FAILED(hr)) 
      {
         sErrorMessage.Format(_T("CreateDataSource failed. Result: %d. Last error: %d"), hr, GetLastError());
         return false;
      }

      if (pIUnknownSession)
         pIUnknownSession->Release();

      if (pIDBDataSourceAdmin)
         pIDBDataSourceAdmin->Release();

      return true;
   }

   /*
      What a transaction is worth on SQL Server Compact, which is the installer
      default and therefore the case that matters most.

      BeginTrans/CommitTrans/RollbackTrans are accepted by the provider and are
      issued here, so what runs inside a transaction is atomic in the ordinary
      sense - DDL too: a CREATE TABLE and an ALTER TABLE ADD run through
      Database.BeginTransaction and ExecuteSQL were both gone after
      RollbackTransaction on the installer smoke VM (2 October 2026). An upgrade
      step that ALTERs three tables and fails on the third is taken back whole,
      by DBUpdater's transaction and by SchemaUpgrader's alike, both of which
      are this class's. Earlier text here said SQL CE committed schema changes
      as they ran; it does not. What a transaction cannot undo is a statement
      marked [IGNORE-ERRORS] whose failure was discarded, since nothing then
      fails: that is why DBUpdater's SchemaVerification proves each marked step
      before the commit rather than trusting the commit. And it is held in a
      temporary file of limited size (SSCE:Temp File Max Size, 128 MB unless
      the connection string says otherwise, which the one above does not), so
      DBUpdater commits before a step whose prerequisite rewrites a large table
      (formMain.SplitsAtLongPrerequisites).

      Separately, and true on every backend: DatabaseConnectionManager::Execute
      takes no connection, so it borrows a *different* pooled connection for each
      statement. A transaction opened here therefore covers only the statements
      routed onto this same connection object - which is exactly the COM API's
      IInterfaceDatabase.ExecuteSQL / ExecuteSQLScript / EnsurePrerequisites
      while a transaction is open, and nothing else. Server-internal work done
      through the object model during that window is outside it.

      What these three did *not* do was report failure. Each returned true
      unconditionally, so a commit that threw - a lost session, a provider error,
      a null connection object - was indistinguishable from one that had written
      the data. DBUpdater turns CommitTransaction() == S_OK into "the upgrade
      succeeded", so that lie became an installer exit code of 0.
   */
   bool
   SQLCEConnection::BeginTransaction(String &sErrorMessage)
   {
      try
      {
         cSQLCEConnection->BeginTrans();
         return true;
      }
      catch (_com_error &err)
      {
         sErrorMessage = "Failed to start a SQL Server Compact transaction: " + DescribeComError(err);
         LOG_APPLICATION(sErrorMessage);
         return false;
      }
      catch (...)
      {
         sErrorMessage = "Failed to start a SQL Server Compact transaction.";
         LOG_APPLICATION(sErrorMessage);
         return false;
      }
   }

   bool
   SQLCEConnection::CommitTransaction(String &sErrorMessage)
   {
      try
      {
         cSQLCEConnection->CommitTrans();
         return true;
      }
      catch (_com_error &err)
      {
         // The data is not committed. Saying so is the whole point: the caller
         // decides whether to retry or to leave the ERROR log in place, and it
         // cannot do either if this returns true.
         sErrorMessage = "Failed to commit a SQL Server Compact transaction. The changes it contained have not been saved. " + DescribeComError(err);
         LOG_APPLICATION(sErrorMessage);
         return false;
      }
      catch (...)
      {
         sErrorMessage = "Failed to commit a SQL Server Compact transaction. The changes it contained have not been saved.";
         LOG_APPLICATION(sErrorMessage);
         return false;
      }
   }

   bool
   SQLCEConnection::RollbackTransaction(String &sErrorMessage)
   {
      try
      {
         cSQLCEConnection->RollbackTrans();
         return true;
      }
      catch (_com_error &err)
      {
         sErrorMessage = "Failed to roll back a SQL Server Compact transaction. Data written inside it may still be present. " + DescribeComError(err);
         LOG_APPLICATION(sErrorMessage);
         return false;
      }
      catch (...)
      {
         sErrorMessage = "Failed to roll back a SQL Server Compact transaction. Data written inside it may still be present.";
         LOG_APPLICATION(sErrorMessage);
         return false;
      }
   }


   void 
   SQLCEConnection::SetTimeout(int seconds)
   {
      cSQLCEConnection->PutCommandTimeout(seconds);
   }

   bool 
   SQLCEConnection::CheckServerVersion(String &errorMessage)
   {
      return true;
   }

   std::shared_ptr<DALRecordset> 
   SQLCEConnection::CreateRecordset()
   {
      std::shared_ptr<SQLCERecordset> recordset = std::shared_ptr<SQLCERecordset>(new SQLCERecordset());
      return recordset;
   }

   std::shared_ptr<IMacroExpander> 
   SQLCEConnection::CreateMacroExpander()
   {
      std::shared_ptr<SQLCEMacroExpander> expander = std::shared_ptr<SQLCEMacroExpander>(new SQLCEMacroExpander());
      return expander;
   }

   void
   SQLCEConnection::EscapeString(String &sInput)
   {
      sInput.Replace(_T("'"), _T("''"));
   }

   namespace
   {
      bool IsWordCharacter(wchar_t c)
      {
         return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'@';
      }

      // Where a whole word first occurs in the statement, case-insensitively, or -1.
      int FindWord(const String &lowerText, const String &lowerWord, int from = 0)
      {
         int position = lowerText.Find(lowerWord, from);

         while (position >= 0)
         {
            int end = position + lowerWord.GetLength();
            bool startsClean = position == 0 || !IsWordCharacter(lowerText[position - 1]);
            bool endsClean = end >= lowerText.GetLength() || !IsWordCharacter(lowerText[end]);

            if (startsClean && endsClean)
               return position;

            position = lowerText.Find(lowerWord, position + 1);
         }

         return -1;
      }

      // Whether a placeholder is a value being WRITTEN, recognised by the only two
      // shapes a write has in the statements this server builds:
      //
      //    insert into t (a, b) values (@a, @b)       - inside the VALUES list
      //    update t set a = @a, b = @b where c = @c   - "column = @name" in the SET
      //                                                 list, followed by a comma,
      //                                                 WHERE or the end
      //
      // Everything else is treated as compared, because that binding fails cleanly
      // and the long one does not: a CASE in a SET list ("when c = @x then"), a
      // JOIN's ON, a function's argument ("replace(col, @old, @new)"), an insert ...
      // select, and anything after WHERE. Getting a write wrong costs a refused
      // write of a value over 4,000 characters; getting a comparison wrong crashed
      // the service, which on 15 September 2026 a native client reproduced for a
      // CASE in a SET list and for a JOIN, neither of which the tree yet contains.
      bool IsWrittenValue(const String &queryString, const String &parameterName)
      {
         String text = queryString;
         text.MakeLower();
         String name = parameterName;
         name.MakeLower();

         int position = FindWord(text, name);
         if (position < 0)
            return false;

         // A parameter used twice is written only if every use is a write, so the
         // one that is found first must also be the only one.
         if (FindWord(text, name, position + name.GetLength()) >= 0)
            return false;

         String head = text;
         head.TrimLeft();

         bool isInsert = head.Left(6) == _T("insert");
         bool isUpdate = head.Left(6) == _T("update");

         if (!isInsert && !isUpdate)
            return false;

         int where = FindWord(text, _T("where"));
         if (where >= 0 && position > where)
            return false;

         if (isInsert)
         {
            // Inside the parentheses that follow VALUES, and no SELECT or CASE anywhere:
            // a comparison inside a CASE in a VALUES list crashed the provider as surely
            // as one in a SET list.
            if (FindWord(text, _T("select")) >= 0 || FindWord(text, _T("case")) >= 0)
               return false;

            int values = FindWord(text, _T("values"));
            if (values < 0 || position < values)
               return false;

            int open = text.Find(_T("("), values);
            int close = text.Find(_T(")"), position);
            return open >= 0 && open < position && close > position;
         }

         int set = FindWord(text, _T("set"));
         if (set < 0 || position < set)
            return false;

         if (FindWord(text, _T("case")) >= 0 || FindWord(text, _T("join")) >= 0)
            return false;

         // Immediately before the placeholder, ignoring spaces: "=", and before that
         // a bare column name - no dot, no bracket, no function.
         int cursor = position - 1;
         while (cursor >= 0 && (text[cursor] == L' ' || text[cursor] == L'\t' || text[cursor] == L'\r' || text[cursor] == L'\n'))
            cursor--;
         if (cursor < 0 || text[cursor] != L'=')
            return false;
         cursor--;
         if (cursor >= 0 && (text[cursor] == L'<' || text[cursor] == L'>' || text[cursor] == L'!'))
            return false;
         while (cursor >= 0 && (text[cursor] == L' ' || text[cursor] == L'\t' || text[cursor] == L'\r' || text[cursor] == L'\n'))
            cursor--;
         int identifierEnd = cursor;
         while (cursor >= 0 && ((text[cursor] >= L'a' && text[cursor] <= L'z') || (text[cursor] >= L'0' && text[cursor] <= L'9') || text[cursor] == L'_'))
            cursor--;
         if (cursor == identifierEnd)
            return false;
         while (cursor >= 0 && (text[cursor] == L' ' || text[cursor] == L'\t' || text[cursor] == L'\r' || text[cursor] == L'\n'))
            cursor--;
         if (cursor < 0 || !(text[cursor] == L',' || cursor == set + 2))
            return false;

         // And after it: a comma, WHERE, or the end of the statement.
         int after = position + name.GetLength();
         while (after < text.GetLength() && (text[after] == L' ' || text[after] == L'\t' || text[after] == L'\r' || text[after] == L'\n'))
            after++;
         if (after >= text.GetLength() || text[after] == L',' || text[after] == L';')
            return true;

         return after == where;
      }
   }

   void
   SQLCEConnection::InitializeCommandParameters(_CommandPtr &adoCommand, const SQLCommand &sqlCommand, String &queryString) const
   {
      for(const SQLParameter &parameter : sqlCommand.GetParameters())
      {
         String parameterName = parameter.GetName();

         if (parameter.GetType() == SQLParameter::ParamTypeInt32)
         {
            VARIANT integerType;
            integerType.vt = VT_I4;
            integerType.intVal = parameter.GetInt32Value();

            adoCommand->Parameters->Append(adoCommand->CreateParameter(_bstr_t(parameterName),adInteger,adParamInput, 4, integerType));
         }
         else if (parameter.GetType() == SQLParameter::ParamTypeInt64)
         {
            // Windows 2000 backwards compatibility:
            ADO64Helper::AddInt64Parameter(adoCommand, parameterName, parameter.GetInt64Value());
         }
         if (parameter.GetType() == SQLParameter::ParamTypeUnsignedInt32)
         {
            VARIANT integerType;
            integerType.vt = VT_UI4;
            integerType.intVal = parameter.GetUnsignedInt32Value();

            adoCommand->Parameters->Append(adoCommand->CreateParameter(_bstr_t(parameterName),adInteger,adParamInput, 4, integerType));
         }
         else if (parameter.GetType() == SQLParameter::ParamTypeString)
         {
            const String &value = parameter.GetStringValue();

            // _variant_t owns its BSTR for the lifetime of this scope; the previous
            // _bstr_t temporary was destroyed at the end of the assignment, leaving
            // the parameter bound to freed memory.
            _variant_t stringType(value.c_str());

            // SQL Server Compact types an adWChar parameter as nvarchar, whose ceiling
            // is 4,000 characters, and refuses a longer value with "A literal value in
            // the command overflowed the range of the type of the associated column" -
            // even when the column is ntext. So a CalDAV object, a domain disclaimer or
            // an alert's detail over 4,000 characters could not be saved at all. A
            // longer value that is being WRITTEN is bound as a long string sized to
            // the value, which Compact 4.0 stores and reads back intact at 4,001, 9,000
            // and 1,048,576 characters; a value too long for an nvarchar column is still
            // refused, never truncated.
            //
            // Only a written value. Bound the same way and COMPARED with an nvarchar
            // column that has rows - "where accountaddress = @address" - the provider
            // crashes with an access violation inside Execute instead of refusing, and
            // it runs in this process, so the whole service goes. An IMAP LOGIN with a
            // long literal username reaches that comparison before anybody has signed
            // in. Compared values keep the binding that refuses cleanly: a value over
            // 4,000 characters cannot equal any nvarchar value anyway. Shorter values
            // keep the fixed size, so the plan cache is not filled with one plan per
            // length.
            if (value.GetLength() > 4000 && IsWrittenValue(queryString, parameterName))
               adoCommand->Parameters->Append(adoCommand->CreateParameter(_bstr_t(parameterName), adLongVarWChar, adParamInput, value.GetLength(), stringType));
            else
               adoCommand->Parameters->Append(adoCommand->CreateParameter(_bstr_t(parameterName), adWChar, adParamInput, value.GetLength() < 8000 ? 8000 : value.GetLength(), stringType));
         }
      }
   }

   namespace
   {
      void CheckCe(bool condition, const String &what)
      {
         if (!condition)
            throw std::logic_error(std::string("SQLCEConnectionTester: ") + Unicode::ToUtf8(what).c_str());
      }

      // What ADO reads for a property out of the string, without opening it - the
      // provider is loaded to name its own properties, and no file is touched.
      String CeProperty(const String &connectionString, const TCHAR *name)
      {
         _ConnectionPtr connection;
         if (FAILED(connection.CreateInstance(__uuidof(Connection))))
            throw std::logic_error("SQLCEConnectionTester: no ADO Connection could be made");

         try
         {
            connection->PutConnectionString(_bstr_t(connectionString.c_str()));
            _variant_t value = connection->Properties->GetItem(_variant_t(name))->Value;
            if (value.vt == VT_EMPTY || value.vt == VT_NULL)
               return String();
            return String((const wchar_t *) _bstr_t(value));
         }
         catch (_com_error &error)
         {
            // The answer ADO gave the unquoted path with a semicolon: "Multiple-step
            // OLE DB operation generated errors".
            return _T("(ADO refused the string: ") + String((const wchar_t *) error.Description()) + _T(")");
         }
      }
   }

   void
   SQLCEConnectionTester::Test()
   {
      const wchar_t *files[] =
      {
         L"C:\\Program Files\\hMailServer\\Database\\hMailServer.sdf",
         L"D:\\Mail;Data\\hMailServer.sdf",
         L"D:\\a=b\\hMailServer.sdf",
         L"D:\\it's\\hMailServer.sdf",
         L"D:\\x;Data Source=E:\\evil\\hMailServer.sdf",
         L"D:\\Mail \u65e5\u672c\\hMailServer.sdf",
      };

      const wchar_t *passwords[] =
      {
         L"0a1b2c3d4e5f",
         L"p;w",
         L"it's",
         L"double\"quote",
         L"a=b",
         L"x;SSCE:Max Database Size=1",
      };

      for (const wchar_t *file : files)
      {
         for (const wchar_t *password : passwords)
         {
            const String connectionString = SQLCEConnection::MakeConnectionString(file, password);
            CheckCe(CeProperty(connectionString, _T("Data Source")) == String(file),
               _T("ADO read the file as [") + CeProperty(connectionString, _T("Data Source")) + _T("] and not [") + String(file) + _T("] from ") + connectionString);
            CheckCe(CeProperty(connectionString, _T("SSCE:Database Password")) == String(password),
               _T("ADO read the password otherwise than given from ") + connectionString);
            CheckCe(CeProperty(connectionString, _T("SSCE:Max Database Size")) == _T("4000"),
               _T("a value moved the database size from ") + connectionString);
         }
      }

      // What every installation's string was before, byte for byte but for the
      // quotes, for the file and the password the installer makes.
      CheckCe(SQLCEConnection::MakeConnectionString(_T("C:\\Data\\hMailServer.sdf"), _T("0a1b2c3d4e5f")) ==
         _T("Provider=Microsoft.SQLSERVER.CE.OLEDB.4.0;Data Source=\"C:\\Data\\hMailServer.sdf\";SSCE:Database Password=\"0a1b2c3d4e5f\";SSCE:Max Database Size=4000;SSCE:Flush Interval=10;"),
         _T("the ordinary string is ") + SQLCEConnection::MakeConnectionString(_T("C:\\Data\\hMailServer.sdf"), _T("0a1b2c3d4e5f")));

      TestUpgrade_();
   }

   namespace
   {
      std::string ReadBytes(const String &file)
      {
         std::ifstream stream(file.c_str(), std::ios::binary);
         return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
      }

      void WriteBytes(const String &file, const std::string &bytes)
      {
         std::ofstream stream(file.c_str(), std::ios::binary | std::ios::trunc);
         stream.write(bytes.data(), (std::streamsize) bytes.size());
         CheckCe(stream.good(), _T("the fixture could not write ") + file);
      }

      // A statement run against a SQL Server Compact file through ADO, or the
      // provider's sentence when it could not be.
      bool ExecuteOn(const String &file, const String &password, const String &sql, String &error)
      {
         _ConnectionPtr connection;
         if (FAILED(connection.CreateInstance(__uuidof(Connection))))
         {
            error = _T("no ADO connection could be made");
            return false;
         }

         try
         {
            connection->Open(_bstr_t(SQLCEConnection::MakeConnectionString(file, password).c_str()), _bstr_t(), _bstr_t(), adConnectUnspecified);
            _RecordsetPtr result = connection->Execute(_bstr_t(sql.c_str()), NULL, adCmdText);
            if (result != nullptr && result->State == adStateOpen)
               result->Close();
            connection->Close();
         }
         catch (_com_error &err)
         {
            error = DescribeComError(err);
            return false;
         }

         return true;
      }

      // The fixture's directory, gone however the test ends.
      struct ScratchDirectory
      {
         String path;
         ScratchDirectory() :
            path(IniFileSettings::Instance()->GetTempDirectory() + FileUtilities::PathSeparator + _T("SQLCEUpgradeTest-") + GUIDCreator::GetGUID())
         {
            boost::system::error_code code;
            boost::filesystem::create_directories(boost::filesystem::path(std::wstring(path)), code);
            CheckCe(!code, _T("the fixture could not make ") + path);
         }
         ~ScratchDirectory()
         {
            boost::system::error_code code;
            boost::filesystem::remove_all(boost::filesystem::path(std::wstring(path)), code);
         }
         String File(const String &name) const { return path + FileUtilities::PathSeparator + name; }
      };
   }

   void
   SQLCEConnectionTester::TestUpgrade_()
   {
      // What every case below must leave when it fails: the live file where it
      // was and as it was, no backup of it, and no upgraded copy - the server is
      // to come back to exactly this at its next start. Each case runs whatever
      // the one before it found, and every case that failed is named.
      const String password = _T("0a1b2c3d4e5f");
      const std::string damaged = std::string("This is a damaged SQL Server Compact database: none of it is a page the engine can read.\r\n") + std::string(8192, 'x');

      ScratchDirectory scratch;

      String failures;
      auto runCase = [&failures](const wchar_t *name, const std::function<void()> &body)
      {
         try
         {
            body();
         }
         catch (const std::logic_error &failure)
         {
            failures += String(_T("\r\n  ")) + name + _T(": ") + String(failure.what());
         }
      };

      auto checkUntouched = [&](const String &what, const String &live, const std::string &before, const String &backup, const String &upgraded)
      {
         CheckCe(FileUtilities::Exists(live), what + _T(": the live database file is gone - moved aside with nothing put in its place"));
         CheckCe(ReadBytes(live) == before, what + _T(": the live database file was changed"));
         CheckCe(!FileUtilities::Exists(backup), what + _T(": the live file was moved aside to ") + backup + _T(" although nothing could replace it"));
         CheckCe(!FileUtilities::Exists(upgraded), what + _T(": an upgraded copy was left at ") + upgraded);
      };

      // 1. A damaged file: the engine cannot upgrade it. Until 6.3.6 the engine's
      //    answer was not read and the file was moved to .backup_v52 regardless.
      runCase(L"a damaged file", [&]()
      {
         const String live = scratch.File(_T("damaged.sdf"));
         const String upgraded = scratch.File(_T("damaged_upgraded.sdf"));
         WriteBytes(live, damaged);

         String backup, error;
         int code = 0;
         const bool upgradedIt = SQLCEConnection::UpgradeDatabaseFile_(live, upgraded, password, backup, error, code);
         checkUntouched(_T("a damaged file"), live, damaged, live + _T(".backup_v52"), upgraded);
         CheckCe(!upgradedIt, _T("a damaged database file was reported upgraded"));
         CheckCe(code == 5123, _T("a damaged database file's failed upgrade is not HM5123: ") + error);
         CheckCe(error.Find(live) >= 0 && error.Find(_T("left as it was")) >= 0, _T("the error does not name the file and say it is left as it was: ") + error);
      });

      // 2. A database already at 4.0, which the engine refuses to upgrade: the
      //    file the server would have opened is still there and still opens.
      runCase(L"a 4.0 file", [&]()
      {
         String createError;
         CheckCe(SQLCEConnection::CreateDatabase(scratch.path, _T("current"), password, createError), _T("the fixture could not make a 4.0 database: ") + createError);
         const String live = scratch.File(_T("current.sdf"));
         const String upgraded = scratch.File(_T("current_upgraded.sdf"));
         const std::string before = ReadBytes(live);

         String backup, error;
         int code = 0;
         const bool upgradedIt = SQLCEConnection::UpgradeDatabaseFile_(live, upgraded, password, backup, error, code);
         checkUntouched(_T("a 4.0 file"), live, before, live + _T(".backup_v52"), upgraded);
         CheckCe(!upgradedIt, _T("a 4.0 database was upgraded to 4.0"));

         String proof;
         CheckCe(SQLCEConnection::ProveDatabaseFile_(live, password, proof), _T("the 4.0 database no longer opens after a refused upgrade: ") + proof);
      });

      // 3. A file another process holds without sharing - a backup program, a
      //    scanner: the engine cannot read it, and nothing is moved.
      runCase(L"a held file", [&]()
      {
         const String live = scratch.File(_T("held.sdf"));
         const String upgraded = scratch.File(_T("held_upgraded.sdf"));
         WriteBytes(live, damaged);

         HANDLE held = CreateFileW(live.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
         CheckCe(held != INVALID_HANDLE_VALUE, _T("the fixture could not hold ") + live);

         String backup, error;
         int code = 0;
         const bool upgradedIt = SQLCEConnection::UpgradeDatabaseFile_(live, upgraded, password, backup, error, code);
         CloseHandle(held);

         checkUntouched(_T("a held file"), live, damaged, live + _T(".backup_v52"), upgraded);
         CheckCe(!upgradedIt, _T("a file another process held was reported upgraded"));
      });

      // 4. A copy an earlier attempt left where the engine writes its own. Until
      //    6.3.6 the engine's refusal to write over it was not read either, and
      //    the leftover was moved into the live file's place - another database
      //    altogether.
      runCase(L"a leftover copy", [&]()
      {
         String createError;
         CheckCe(SQLCEConnection::CreateDatabase(scratch.path, _T("leftover_upgraded"), password, createError), _T("the fixture could not make the leftover copy: ") + createError);
         const String live = scratch.File(_T("leftover.sdf"));
         const String upgraded = scratch.File(_T("leftover_upgraded.sdf"));
         WriteBytes(live, damaged);

         String backup, error;
         int code = 0;
         const bool upgradedIt = SQLCEConnection::UpgradeDatabaseFile_(live, upgraded, password, backup, error, code);
         checkUntouched(_T("a damaged file with a leftover copy"), live, damaged, live + _T(".backup_v52"), upgraded);
         CheckCe(!upgradedIt, _T("a damaged file with a leftover copy beside it was reported upgraded"));
      });

      // 5. The steps after the engine, which no 3.5 database is at hand to reach
      //    through it: a sound 4.0 copy takes the place of the file it upgraded,
      //    which is kept as .backup_v52; and again, with that backup there, the
      //    next is kept as .backup_v52.2 and the first is left alone.
      runCase(L"a sound copy, twice", [&]()
      {
         String createError;
         CheckCe(SQLCEConnection::CreateDatabase(scratch.path, _T("installed_upgraded"), password, createError), _T("the fixture could not make the upgraded copy: ") + createError);
         const String live = scratch.File(_T("installed.sdf"));
         const String upgraded = scratch.File(_T("installed_upgraded.sdf"));
         // Told apart by a table of its own, not by its bytes: the proof opens
         // the copy, and SQL Server Compact writes to a file it opens.
         CheckCe(ExecuteOn(upgraded, password, _T("create table hm_upgrade_marker (id int)"), createError), _T("the fixture could not mark the upgraded copy: ") + createError);
         WriteBytes(live, damaged);

         String backup, error;
         int code = 0;
         CheckCe(SQLCEConnection::InstallUpgradedCopy_(live, upgraded, password, backup, error, code), _T("a sound upgraded copy was not put in place: ") + error);
         CheckCe(backup == live + _T(".backup_v52"), _T("the 3.5 file was kept as ") + backup);
         CheckCe(ReadBytes(backup) == damaged, _T("the backup is not the file that was upgraded"));
         String marker;
         CheckCe(ExecuteOn(live, password, _T("select count(*) from hm_upgrade_marker"), marker), _T("the live file is not the upgraded copy: ") + marker);
         CheckCe(!FileUtilities::Exists(upgraded), _T("the upgraded copy is still beside the live file"));

         CheckCe(SQLCEConnection::CreateDatabase(scratch.path, _T("installed_upgraded"), password, createError), _T("the fixture could not make the second copy: ") + createError);
         WriteBytes(live, std::string("the second 3.5 file"));
         String second;
         CheckCe(SQLCEConnection::InstallUpgradedCopy_(live, upgraded, password, second, error, code), _T("a second upgraded copy was not put in place: ") + error);
         CheckCe(ReadBytes(live + _T(".backup_v52")) == damaged, _T("the earlier backup was written over"));
         CheckCe(second == live + _T(".backup_v52.2"), _T("with .backup_v52 taken, the 3.5 file was kept as ") + second);
         CheckCe(ReadBytes(second) == "the second 3.5 file", _T("the second backup is not the second file"));
      });

      // 6. A copy that is not sound is never put in place.
      runCase(L"an unsound copy", [&]()
      {
         const String live = scratch.File(_T("unsound.sdf"));
         const String upgraded = scratch.File(_T("unsound_upgraded.sdf"));
         const std::string before("the 3.5 file whose upgraded copy is not sound");
         WriteBytes(live, before);
         WriteBytes(upgraded, damaged);

         String backup, error;
         int code = 0;
         const bool installed = SQLCEConnection::InstallUpgradedCopy_(live, upgraded, password, backup, error, code);
         checkUntouched(_T("an unsound copy"), live, before, live + _T(".backup_v52"), upgraded);
         CheckCe(!installed, _T("an unsound upgraded copy was put in place"));
         CheckCe(code == 5124, _T("an unsound copy's refusal is not HM5124: ") + error);
      });

      if (!failures.IsEmpty())
         throw std::logic_error(std::string("SQLCEConnectionTester, the 3.5-to-4.0 upgrade:") + Unicode::ToUtf8(failures).c_str());
   }
}

#endif
