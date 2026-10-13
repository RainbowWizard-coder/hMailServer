// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "ADOConnection.h"
#include "ADORecordset.h"
#include "SQLCommand.h"
#include "DatabaseSettings.h"
#include "DatabaseTls.h"
#include "Macros/MSSQLMacroExpander.h"
#include "ADOInt64Helper.h"
#include "../Util/Registry.h"
#include "../Util/Unicode.h"
#include "../Application/IniFileSettings.h"

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
   ADOConnection::ADOConnection(std::shared_ptr<DatabaseSettings> pSettings) :
      DALConnection(pSettings)
   {
      HRESULT hr =cADOConnection.CreateInstance(__uuidof(Connection));
      
      cADOConnection->PutCursorLocation(adUseClient);

      if ( !SUCCEEDED( hr ) )
      {
         HM_ASSERT(0);
      }  

      bConnected = false;
   }


   ADOConnection::~ADOConnection()
   {
   
      try
      {
         cADOConnection = NULL;
      }
      catch(...)
      {
      
      } 
   
   
   }


   bool
   ADOConnection::Disconnect()
   {

      try
      {

         if (!bConnected)
            return false; // --- already disconnected.

         if (cADOConnection->State == 1)
            cADOConnection->Close();
      }
      catch ( ... )
      {
         HM_ASSERT(0);
      }

      bConnected = false;

      return true;
   }


   DALConnection::ConnectionResult
   ADOConnection::Connect(String &sErrorMessage)
   {
      String sUsername = database_settings_->GetUsername();
      String sPassword = database_settings_->GetPassword();
      String sServer = database_settings_->GetServer();
      String sDatabase = database_settings_->GetDatabaseName();
      String sServerFailoverPartner = database_settings_->GetDatabaseServerFailoverPartner();

      String sProvider = database_settings_->GetProvider();

      if (sProvider.IsEmpty())
      {
         if (sServerFailoverPartner.IsEmpty())
         {
            // If MSOLEDBSQL v18 or later is installed, prefer that one. It supports TLS 1.2.
            if (IsMSOLEDBSQL18OrLaterInstalled_())
            {
               sProvider = "MSOLEDBSQL";
            }
            else
            {
               sProvider = "sqloledb";
            }
         }
         else
         {
            sProvider = "SQLNCLI";
         }
      }

      if (bConnected)
      {
         HM_ASSERT(0); // --- Already connected!
         return Connected;
      }

      // TLS (DatabaseTls.h): the provider's own two keywords, "Use Encryption for
      // Data" and "Trust Server Certificate", chosen for [Database] TlsMode and the
      // provider. A mode this server does not know, or ConnectionStringOptions
      // naming encryption beside a TlsMode, is a configuration to correct: nothing
      // is attempted, and the connection manager does not try again. TlsCaFile is
      // not read here - the providers check against the Windows certificate store
      // - which FromConfiguration says once.
      DatabaseTls::Mode tlsMode = DatabaseTls::ModeDefault;
      String tlsCaFile;
      if (!DatabaseTls::FromConfiguration(DatabaseSettings::TypeMSSQLServer, tlsMode, tlsCaFile, sErrorMessage))
         return FatalError;

      String sOptions = IniFileSettings::Instance()->GetDatabaseConnectionStringOptions();

      String tlsKeywords;
      if (!DatabaseTls::SqlServerKeywords(tlsMode, sProvider, sOptions, tlsKeywords, sErrorMessage))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 7533, "ADOConnection::Connect", sErrorMessage);
         return FatalError;
      }

      // The server and the database each in the quotes ADO reads a value in, and the
      // user and the password not in the string at all: Open takes them as its own
      // arguments (OpenOnce_), where ADO reads nothing in them. Up to 6.3.5 all four
      // were written into the string as they were, so a semicolon in a password
      // ended it and began a keyword of its own, and a quote at its start made the
      // string one OLE DB does not parse ("Format of the initialization string does
      // not conform to the OLE DB specification") - which DBSetup had to refuse.
      String sConnectionString;
      if (!MakeConnectionString(sProvider, sServer, sDatabase, sUsername.IsEmpty(), sServerFailoverPartner, sConnectionString, sErrorMessage))
         return FatalError;

      // [Database] ConnectionStringOptions, verbatim and last. Anything the provider
      // understands can be written there; encryption is TlsMode's now, and
      // SqlServerKeywords has refused the two saying it at once.
      String sOptionsPart;
      if (!sOptions.IsEmpty())
      {
         sOptionsPart = sOptions;
         if (sOptions.Right(1) != _T(";"))
            sOptionsPart.append(_T(";"));
      }

      String errorDescription;
      String errorSource;
      int outcome = OpenOnce_(sConnectionString + tlsKeywords + sOptionsPart, sUsername, sPassword, errorDescription, errorSource);

      // prefer - the default, or said explicitly - takes the keywords away again when
      // what failed was the TLS: a SQL Server that cannot encrypt, which connected
      // before TlsMode existed, still does. Not for require or verify-full, and not
      // for a failure about anything else: a refused login is never tried twice.
      String tlsNotAgreed;
      if (outcome == OpenRefused && DatabaseTls::Effective(tlsMode) == DatabaseTls::ModePrefer && !tlsKeywords.IsEmpty() &&
          DatabaseTls::LooksLikeTlsFailure(errorDescription))
      {
         tlsNotAgreed = errorDescription;
         errorDescription.Empty();
         errorSource.Empty();

         outcome = OpenOnce_(sConnectionString + sOptionsPart, sUsername, sPassword, errorDescription, errorSource);
         if (outcome == OpenSucceeded)
            tlsKeywords.Empty();
      }

      if (outcome == OpenFailed)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5027, "ADOConnection::Connect", "Open failed");
         return TemporaryFailure;
      }

      if (outcome == OpenRefused)
      {
         sErrorMessage = "ADO: " + errorDescription;

         if (!tlsNotAgreed.IsEmpty())
            sErrorMessage = "ADO, with the connection encrypted: " + tlsNotAgreed + " Without: " + errorDescription;

         if (!tlsNotAgreed.IsEmpty() || DatabaseTls::LooksLikeTlsFailure(errorDescription))
            sErrorMessage += _T("\r\n\r\n") + DatabaseTls::Guidance(DatabaseSettings::TypeMSSQLServer, tlsMode);

         String sMessage = "Error when connecting to database. " + errorSource + " " + errorDescription + " Check your database settings in hMailServer.ini.";

         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5028, "ADOConnection::Connect", sMessage);

         return TemporaryFailure;
      }

      if (outcome != OpenSucceeded)
      {
         cADOConnection = 0;

         sErrorMessage = "ADO: Unknown database connection error";

         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5029, "ADOConnection::Connect", "An unknown error occurred when connecting to database. Check your database settings in hMailServer.ini.");

         return TemporaryFailure;
      }

      bConnected = true;

      // Change the format of dates. We always insert dates using the format yyyy-mm-dd.
      // We need to inform MSSQL about this.
      String sSetDateFormat = "SET DATEFORMAT ymd";
      String sErrorMsg;

      SQLCommand command1(sSetDateFormat);
      TryExecute(command1, sErrorMsg, 0, 0);

      // Set transaction isolation level to read uncommitted. This may give phantom
      // reads but that isn't a problem since we've implemented our own locking 
      // mechanism.
      String sIsolationLevel = "SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED";
      
      sErrorMsg = "";

      SQLCommand command2(sIsolationLevel);
      TryExecute(command2, sErrorMsg, 0, 0);

      // The row count TryExecute hands back is the RecordsAffected ADO reads from
      // the count in SQL Server's DONE token, which NOCOUNT leaves out: under it
      // every statement counts -1, and a claim judged by its count (ActiveActive.md)
      // would never be won. A session starts with NOCOUNT off unless the server's
      // 'user options' turn it on for every session, so it is turned off here, as a
      // session normally is - which is what every batch this server sends was
      // written against. ReadEncryptOption_ below turns it on for its own batch and
      // puts back what it found.
      sErrorMsg = "";

      SQLCommand command3(_T("SET NOCOUNT OFF"));
      TryExecute(command3, sErrorMsg, 0, 0);

      // What the session is. The provider has refused before the login whatever it
      // was asked to encrypt and could not, so a session opened with the keywords -
      // or by MSOLEDBSQL 19 left to its own default, which encrypts - is encrypted.
      // SQL Server's own answer is taken when there is one, and is what notices a
      // server that forces encryption on a session nobody asked for; but it comes
      // from sys.dm_exec_connections, which only a login with VIEW SERVER STATE reads
      // (measured 25 September 2026 on SQL Server 2022: CONNECTIONPROPERTY has no
      // encrypt_option and answers NULL to it), so an application's login usually
      // gets no answer. This is for the diagnostics and the log, and refuses only a
      // require or verify-full the server answers FALSE to.
      String encryptOption = ReadEncryptOption_();

      const DatabaseTls::ProviderKind providerKind = DatabaseTls::ClassifyProvider(sProvider);
      const bool optionsDecide = DatabaseTls::OptionsNameEncryption(sOptions);
      const bool providerDefaultEncrypts = providerKind == DatabaseTls::ProviderModern19 && tlsKeywords.IsEmpty() && tlsNotAgreed.IsEmpty() && !optionsDecide;
      const bool askedToEncrypt = tlsKeywords.Find(_T("Use Encryption for Data=True")) >= 0 || providerDefaultEncrypts;
      const bool answered = encryptOption.CompareNoCase(_T("TRUE")) == 0 || encryptOption.CompareNoCase(_T("FALSE")) == 0;
      const bool encrypted = answered ? encryptOption.CompareNoCase(_T("TRUE")) == 0 : askedToEncrypt;
      const DatabaseTls::Mode effectiveMode = DatabaseTls::Effective(tlsMode);

      if (answered && !encrypted && (effectiveMode == DatabaseTls::ModeRequire || effectiveMode == DatabaseTls::ModeVerifyFull))
      {
         sErrorMessage = Formatter::Format("SQL Server at {0} says the connection is not encrypted, and TlsMode is {1}: it was closed before it was used.",
            sServer, DatabaseTls::ModeName(tlsMode));
         Disconnect();
         return TemporaryFailure;
      }

      // Checked: verify-full; SQLOLEDB, which checks whenever it encrypts; and
      // MSOLEDBSQL19 left entirely to its own default, which checks - not when
      // ConnectionStringOptions has said something about it, which this does not
      // read to find out what.
      const bool verified = encrypted &&
         (effectiveMode == DatabaseTls::ModeVerifyFull ||
          (providerKind == DatabaseTls::ProviderLegacy && askedToEncrypt) ||
          providerDefaultEncrypts);

      String transportDetail = _T("provider ") + sProvider;
      if (!encrypted)
      {
         if (!tlsNotAgreed.IsEmpty())
            transportDetail = _T("TLS could not be agreed on (") + tlsNotAgreed + _T("), so the connection was made without it; provider ") + sProvider;
         else if (effectiveMode == DatabaseTls::ModeDisable)
            transportDetail = _T("TLS is switched off, and the provider still encrypts the login; provider ") + sProvider;
         else if (providerKind == DatabaseTls::ProviderLegacy)
            transportDetail = _T("SQLOLEDB encrypts only the login unless TlsMode is require or verify-full, and then checks the certificate; install the Microsoft OLE DB Driver for SQL Server to encrypt without that");
         else if (!answered && optionsDecide)
            transportDetail = _T("as far as this server can tell - ConnectionStringOptions sets the encryption, and this login may not read what it came to (VIEW SERVER STATE); provider ") + sProvider;
         else if (!answered)
            transportDetail = _T("the provider was not asked to encrypt, and this login may not read whether SQL Server forces it (VIEW SERVER STATE); provider ") + sProvider;
         else
            transportDetail = _T("the provider encrypts only the login; provider ") + sProvider;
      }
      else if (!answered)
      {
         transportDetail += _T("; the provider was set to encrypt, which it does or refuses the connection");
      }

      SetTransportSecurity_(encrypted, DatabaseTls::Describe(DatabaseSettings::TypeMSSQLServer, encrypted, transportDetail, verified, tlsMode));

      return Connected;
   }

   int
   ADOConnection::OpenOnce_(const String &connectionString, const String &username, const String &password, String &errorDescription, String &errorSource)
   {
      BSTR bsConnection = connectionString.AllocSysString();
      BSTR bsUser = username.AllocSysString();
      BSTR bsPassword = password.AllocSysString();

      int outcome = OpenSucceeded;

      try
      {
         HRESULT hr = cADOConnection->Open( bsConnection, bsUser, bsPassword, NULL );
         cADOConnection->PutCursorLocation(adUseClient);

         if ( !SUCCEEDED( hr ) )
            outcome = OpenFailed;
      }
      catch ( _com_error &err )
      {
         _bstr_t bstrSource( err.Source() );
         _bstr_t bstrDescription( err.Description() );

         LPCSTR lpcSource = bstrSource;
         errorSource = lpcSource;

         LPCSTR lpcDesc = bstrDescription;
         errorDescription = lpcDesc;

         outcome = OpenRefused;
      }
      catch( ... )
      {
         outcome = OpenUnknownError;
      }

      SysFreeString(bsConnection);
      SysFreeString(bsUser);
      SysFreeString(bsPassword);

      return outcome;
   }

   String
   ADOConnection::ReadEncryptOption_()
   {
      try
      {
         // Through sp_executesql, and only for a login that holds VIEW SERVER STATE:
         // a statement that names the view is refused as it is compiled, whether or
         // not the branch that reads it would run, and the refusal would be an error
         // at every connection. Without the permission the answer is NULL. NOCOUNT is
         // put back as it was, since the session is the pool's. Measured through
         // ADO and SQLOLEDB on SQL Server 2022 on 25 September 2026: NULL for a
         // login without the permission and FALSE for one with it, the session
         // left with NOCOUNT off; the view's TRUE for an encrypted session was read
         // through sqlcmd.
         const String query =
            _T("DECLARE @nocount int = @@OPTIONS & 512; SET NOCOUNT ON; DECLARE @e nvarchar(40); ")
            _T("IF HAS_PERMS_BY_NAME(NULL, NULL, 'VIEW SERVER STATE') = 1 ")
            _T("EXEC sp_executesql N'SELECT @e = encrypt_option FROM sys.dm_exec_connections WHERE session_id = @@SPID', N'@e nvarchar(40) OUTPUT', @e OUTPUT; ")
            _T("IF @nocount = 0 SET NOCOUNT OFF; ")
            _T("SELECT CONVERT(varchar(5), @e) AS encryptoption");

         ADORecordset recordset;
         String queryError;

         if (recordset.TryOpen(shared_from_this(), SQLCommand(query), queryError) != DALConnection::DALSuccess)
            return String();

         if (recordset.IsEOF())
            return String();

         String value = recordset.GetStringValue("encryptoption");
         value.Trim();
         return value;
      }
      catch (...)
      {
         return String();
      }
   }

   _ConnectionPtr
   ADOConnection::GetConnection() const
   {
      return cADOConnection;
   }

   __int64
   ADOConnection::ReadRecordsAffected(const VARIANT &recordsAffected)
   {
      __int64 count = -1;

      switch (recordsAffected.vt)
      {
      case VT_I4:
         count = recordsAffected.lVal;
         break;
      case VT_I8:
         count = recordsAffected.llVal;
         break;
      case VT_I2:
         count = recordsAffected.iVal;
         break;
      case VT_UI4:
         count = recordsAffected.ulVal;
         break;
      case VT_EMPTY:
      case VT_NULL:
         break;
      default:
         {
            _variant_t converted;
            if (SUCCEEDED(VariantChangeType(&converted, &recordsAffected, 0, VT_I8)))
               count = converted.llVal;
         }
         break;
      }

      return count < 0 ? -1 : count;
   }

   DALConnection::ExecutionResult
   ADOConnection::TryExecute(const SQLCommand &command, String &sErrorMessage, __int64 *iInsertID, int iIgnoreErrors, __int64 *rowsAffected)
   {
      if (rowsAffected != nullptr)
         *rowsAffected = -1;

      if (!bConnected)
         return DALConnectionProblem; // --- already disconnected.

      String queryString = command.GetQueryString();

      try
      {
         // Execute the SQL statement.
         _CommandPtr adoCommand;
         adoCommand.CreateInstance(__uuidof(Command));
         InitializeCommandParameters(adoCommand, command, queryString);

         adoCommand->CommandType = adCmdText;
         adoCommand->CommandText = _bstr_t(queryString);
         adoCommand->ActiveConnection = cADOConnection;

         // RecordsAffected: the rows the statement matched (DALConnection.h).
         _variant_t recordsAffected;
         adoCommand->Execute(&recordsAffected, NULL, adCmdText);

         if (rowsAffected != nullptr)
            *rowsAffected = ReadRecordsAffected(recordsAffected);

         // Check what unique ID we got back (if we're interested). Callers treat 0
         // as "no identity available" - which is what an empty identity recordset
         // or a NULL @@IDENTITY gives us - so it must be in place before we ask.
         if (iInsertID != nullptr)
         {
            *iInsertID = 0;

            _RecordsetPtr pIdentityRS;
            pIdentityRS.CreateInstance(__uuidof(Recordset));
            pIdentityRS->PutCursorLocation(adUseClient);
            pIdentityRS->PutRefActiveConnection(cADOConnection);

            // The _variant_t owns its BSTR, so the query string is released even
            // if Open throws.
            _variant_t vtIdentitySQL(_T("SELECT @@IDENTITY AS IDENT"));

            pIdentityRS->Open( vtIdentitySQL, vtMissing, adOpenKeyset, adLockOptimistic , adCmdText);
            pIdentityRS->PutRefActiveConnection(NULL);

            *iInsertID = GetIdentityFromRS_(pIdentityRS);
         }
      }
      catch ( _com_error &err )
      {
         // A failure, ignored or not, leaves no count of its own: a statement
         // that fails changes no row (DALConnection.h).
         if (rowsAffected != nullptr)
            *rowsAffected = -1;

         ExecutionResult dbErr = GetErrorType(_com_error::WCodeToHRESULT(err.WCode()));
         if (iIgnoreErrors > 0 && iIgnoreErrors & dbErr)
         {
            if (rowsAffected != nullptr)
               *rowsAffected = 0;

            return DALConnection::DALSuccess;
         }

         // The marker means "this object may already exist". It must not also
         // discard a lost connection - see DALConnection::HasIgnoreErrorsMarker.
         // A connection problem is returned so DALConnection::Execute reconnects
         // and tries again, and reports the statement as failed if it cannot.
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

         sErrorMessage = "Source: ADOConnection::Execute(), Code: HM10044, Description: Error while executing SQL statement: \n";
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

         sErrorMessage = "Source: ADOConnection::Execute(), Code: HM10045, Description: Unknown error when executing SQL statement: \n";
         sErrorMessage.append(queryString);

         return DALUnknown;
      }

      return DALSuccess;
   }

   bool
   ADOConnection::IsConnected() const
   { 
      return bConnected; 
   };

   __int64 
   ADOConnection::GetIdentityFromRS_(_RecordsetPtr pRS) const
   {
      try
      {
         if (pRS->GetRecordCount() == 0)
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
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5030, "ADOConnection::GetIdentityFromRS_", "Error while determening @@IDENTITY");
      }

      return 0;
   }

   void 
   ADOConnection::OnConnected()
   {
      // Nothing to do here. Only used my MySQL.
   }

   DALConnection::ExecutionResult 
   ADOConnection::GetErrorType(int iErrorCode)
   {
      switch (iErrorCode)
      {
      case 0:
         return DALSuccess;
      case 0x80040E2FL: // DB_E_INTEGRITYVIOLATION:
         return DALErrorInSQL;
      case 0x80040200L:
         return DALConnectionProblem;
      default:
         return DALUnknown;
      }
   }

   bool 
   ADOConnection::BeginTransaction(String &sErrorMessage)
   {
      return TryExecute(SQLCommand("BEGIN TRANSACTION"), sErrorMessage, 0, 0) == DALSuccess;
   }

   bool 
   ADOConnection::CommitTransaction(String &sErrorMessage)
   {
      return TryExecute(SQLCommand("COMMIT TRANSACTION"), sErrorMessage, 0, 0) == DALSuccess;
   }

   bool 
   ADOConnection::RollbackTransaction(String &sErrorMessage)
   {
      return TryExecute(SQLCommand("ROLLBACK TRANSACTION"), sErrorMessage, 0, 0) == DALSuccess;
   }

   void 
   ADOConnection::SetTimeout(int seconds)
   {
      cADOConnection->PutCommandTimeout(seconds);
   }

   bool 
   ADOConnection::CheckServerVersion(String &errorMessage)
   {
      std::shared_ptr<ADOConnection> connection = shared_from_this();

      ADORecordset recordset;
      if (recordset.TryOpen(connection, SQLCommand("SELECT SERVERPROPERTY('productversion') as ProductVersion"), errorMessage) != DALConnection::DALSuccess)
         return false;

      String version = recordset.GetStringValue("ProductVersion");

      std::vector<String> versionVector = StringParser::SplitString(version, ".");
      int majorVersion = _ttoi(versionVector[0]);
      
      if (majorVersion < RequiredVersion)
      {
         errorMessage = "hMailServer requires Microsoft SQL Server 2000 or newer.";       
         return false;
      }

      return true;
   }

   std::shared_ptr<DALRecordset> 
   ADOConnection::CreateRecordset()
   {
      std::shared_ptr<ADORecordset> recordset = std::shared_ptr<ADORecordset>(new ADORecordset());
      return recordset;
   }

   void
   ADOConnection::EscapeString(String &sInput)
   {
      sInput.Replace(_T("'"), _T("''"));
   }

   std::shared_ptr<IMacroExpander> 
   ADOConnection::CreateMacroExpander()
   {
      std::shared_ptr<MSSQLMacroExpander> expander = std::shared_ptr<MSSQLMacroExpander>(new MSSQLMacroExpander());
      return expander;
   }

   void
   ADOConnection::InitializeCommandParameters(_CommandPtr &adoCommand, const SQLCommand &sqlCommand, String &queryString) const
   {
      for(const SQLParameter &parameter : sqlCommand.GetParameters())
      {
         String parameterName = parameter.GetName();

         if (parameter.GetType() == SQLParameter::ParamTypeInt32)
         {
            VARIANT integerType;
            integerType.vt = VT_I4;
            integerType.intVal = parameter.GetInt32Value();

            adoCommand->Parameters->Append(adoCommand->CreateParameter(_bstr_t(),adInteger,adParamInput, 4, integerType));
         }
         else if (parameter.GetType() == SQLParameter::ParamTypeInt64)
         {
            // Windows 2000 backwards compatibility:
            ADO64Helper::AddInt64Parameter(adoCommand, parameterName, parameter.GetInt64Value());
         }
         else if (parameter.GetType() == SQLParameter::ParamTypeUnsignedInt32)
         {
            VARIANT integerType;
            integerType.vt = VT_UI4;
            integerType.intVal = parameter.GetUnsignedInt32Value();

            adoCommand->Parameters->Append(adoCommand->CreateParameter(_bstr_t(),adInteger,adParamInput, 4, integerType));
         }
         else if (parameter.GetType() == SQLParameter::ParamTypeString)
         {
            const String &value = parameter.GetStringValue();

            // _variant_t owns its BSTR for the lifetime of this scope. Building the
            // VARIANT from a _bstr_t temporary instead freed the string at the end of
            // the assignment, so the parameter was bound from released memory.
            _variant_t stringType(value.c_str());

            int length = 0;

            if (value.GetLength() < 4000)
               length = 4000;
            else
               length = value.GetLength();   

            if (!value.IsEmpty())
               adoCommand->Parameters->Append(adoCommand->CreateParameter(_bstr_t(), adVarWChar, adParamInput, length, stringType));
            else
               adoCommand->Parameters->Append(adoCommand->CreateParameter(_bstr_t(), adVarWChar, adParamInput, 1, stringType));
         }

         queryString.Replace(parameterName, _T("?"));
      }
   }

   bool 
   ADOConnection::IsMSOLEDBSQL18OrLaterInstalled_() const
   {
      Registry registry;
      String value;
      if (!registry.GetStringValue(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\MSOLEDBSQL", "InstalledVersion", value))
         return false;

      auto versionInfo = StringParser::SplitString(value, ".");

      if (versionInfo.size() != 4)
      {
         // Unsupported format
         return false;
      }

      auto majorVersionStr = versionInfo[0];
      if (!StringParser::IsNumeric(majorVersionStr))
      {
         // Unsupported format
         return false;
      }

      int majorVersion = 0;
      AnsiString str = majorVersionStr;
      if (!StringParser::TryParseInt(str, majorVersion))
      {
         // Unsupported format
         return false;
      }

      return majorVersion >= 18;
   }

   bool
   ADOConnection::QuoteValue(const String &value, String &quoted)
   {
      // ADO parses the string itself before OLE DB sees it, and more narrowly than
      // OLE DB's own parser (measured with ADO 6.1 and SQLOLEDB on Windows 11, 30
      // September 2026, by reading the properties ADO made of each string): in
      // double quotes it takes a single quote, a semicolon, an equals sign and
      // spaces as the value's; in single quotes a double quote, or a semicolon, but
      // not both ('a"b;c' is "Format of the initialization string does not conform
      // to the OLE DB specification"); and a quote doubled inside quotes it does not
      // read ("""a" fails the same way, where OLE DB takes it). So double quotes,
      // and single quotes for a value with a double quote and no semicolon; a value
      // with a double quote and a single quote or a semicolon has no form ADO
      // reads. Empty is written as nothing, as it always was.
      quoted.Empty();
      if (value.IsEmpty())
         return true;

      if (value.Find(_T("\"")) < 0)
      {
         quoted = _T("\"") + value + _T("\"");
         return true;
      }

      if (value.Find(_T("'")) < 0 && value.Find(_T(";")) < 0)
      {
         quoted = _T("'") + value + _T("'");
         return true;
      }

      return false;
   }

   bool
   ADOConnection::MakeConnectionString(const String &provider, const String &server, const String &database, bool integratedSecurity,
                                       const String &failoverPartner, String &connectionString, String &error)
   {
      connectionString.Empty();
      error.Empty();

      // A provider is a name - SQLOLEDB, SQLNCLI, MSOLEDBSQL, MSOLEDBSQL19 - and is
      // written as it is.
      for (int i = 0; i < provider.GetLength(); i++)
      {
         const wchar_t c = provider[i];
         if (!((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' || c == L'-'))
         {
            error = _T("The database provider in hMailServer.ini, ") + provider + _T(", is not a provider's name. The connection was not attempted.");
            return false;
         }
      }

      String quotedServer;
      if (!QuoteValue(server, quotedServer))
      {
         error = _T("The database server's name in hMailServer.ini holds a double quote beside a single quote or a semicolon, which no SQL Server connection string carries. The connection was not attempted.");
         return false;
      }

      String quotedDatabase;
      if (!QuoteValue(database, quotedDatabase))
      {
         error = _T("The database name in hMailServer.ini holds a double quote beside a single quote or a semicolon, which no SQL Server connection string carries. The connection was not attempted.");
         return false;
      }

      // The failover partner goes into the provider's own string (SQLNCLI's and
      // MSOLEDBSQL's FailoverPartner), which OLE DB passes on as it stands and the
      // provider reads without quotes: a host name, and nothing that would end it.
      for (int i = 0; i < failoverPartner.GetLength(); i++)
      {
         const wchar_t c = failoverPartner[i];
         if (c == L';' || c == L'"' || c == L'\'' || c == L'{' || c == L'}' || c == L'=' || c < 0x20)
         {
            error = _T("The failover partner in hMailServer.ini, ") + failoverPartner + _T(", holds a quote, a semicolon, a brace, an equals sign or a control character, which the provider's connection string does not carry. The connection was not attempted.");
            return false;
         }
      }

      connectionString = _T("Provider=") + provider + _T(";");
      connectionString += _T("Data Source=") + quotedServer + _T(";");
      connectionString += _T("Initial Catalog=") + quotedDatabase + _T(";");

      // Windows authentication; otherwise the user and the password go to Open.
      if (integratedSecurity)
         connectionString += _T("Integrated Security=SSPI;");

      if (!failoverPartner.IsEmpty())
         connectionString += _T("FailoverPartner=") + failoverPartner + _T(";");

      return true;
   }

   namespace
   {
      void CheckAdo(bool condition, const String &what)
      {
         if (!condition)
            throw std::logic_error(std::string("ADOConnectionTester: ") + Unicode::ToUtf8(what).c_str());
      }

      // What ADO itself reads for a property out of the string, without opening it.
      String AdoProperty(const String &connectionString, const TCHAR *name)
      {
         _ConnectionPtr connection;
         if (FAILED(connection.CreateInstance(__uuidof(Connection))))
            throw std::logic_error("ADOConnectionTester: no ADO Connection could be made");

         connection->PutConnectionString(_bstr_t(connectionString.c_str()));
         _variant_t value = connection->Properties->GetItem(_variant_t(name))->Value;
         if (value.vt == VT_EMPTY || value.vt == VT_NULL)
            return String();
         return String((const wchar_t *) _bstr_t(value));
      }
   }

   void
   ADOConnectionTester::Test()
   {
      // Names ADO must read back as they were given, each in the place of the
      // server and of the database: a semicolon, an equals sign, spaces, one kind
      // of quote or the other, a quote first, a keyword's text, an instance's
      // backslash and a port's comma.
      const wchar_t *hostile[] =
      {
         L"plain",
         L"HOST\\INSTANCE",
         L"tcp:db.example,14330",
         L"semi;colon",
         L"a=b",
         L" leading and trailing ",
         L"it's",
         L"'single first",
         L"double\"quote",
         L"\"double first",
         L"double\"quote=equals",
         L"x;Data Source=evil;Initial Catalog=evil",
         L"p\u00e4ssw\u00f6rd \u65e5\u672c",
      };

      for (const wchar_t *text : hostile)
      {
         const String value(text);
         String connectionString;
         String error;
         CheckAdo(ADOConnection::MakeConnectionString(_T("SQLOLEDB"), value, value, false, String(), connectionString, error),
            _T("MakeConnectionString refused a name it should write: ") + value + _T(" (") + error + _T(")"));

         CheckAdo(AdoProperty(connectionString, _T("Data Source")) == value,
            _T("ADO read the server as [") + AdoProperty(connectionString, _T("Data Source")) + _T("] and not [") + value + _T("] from ") + connectionString);
         CheckAdo(AdoProperty(connectionString, _T("Initial Catalog")) == value,
            _T("ADO read the database as [") + AdoProperty(connectionString, _T("Initial Catalog")) + _T("] and not [") + value + _T("] from ") + connectionString);
         CheckAdo(AdoProperty(connectionString, _T("Password")).IsEmpty() && AdoProperty(connectionString, _T("User ID")).IsEmpty(),
            _T("a name put a user or a password into the string: ") + connectionString);
      }

      // Windows authentication when there is no user; the credentials are never in
      // the string.
      String connectionString;
      String error;
      CheckAdo(ADOConnection::MakeConnectionString(_T("SQLOLEDB"), _T("db"), _T("hMailServer"), true, String(), connectionString, error) &&
               connectionString == _T("Provider=SQLOLEDB;Data Source=\"db\";Initial Catalog=\"hMailServer\";Integrated Security=SSPI;"),
         _T("the string for Windows authentication is ") + connectionString);
      CheckAdo(ADOConnection::MakeConnectionString(_T("SQLNCLI"), _T("db"), String(), false, _T("db2\\mirror"), connectionString, error) &&
               connectionString == _T("Provider=SQLNCLI;Data Source=\"db\";Initial Catalog=;FailoverPartner=db2\\mirror;"),
         _T("the string with a failover partner and no database is ") + connectionString);

      // What no connection string carries is refused, not written as something else.
      CheckAdo(!ADOConnection::MakeConnectionString(_T("SQLOLEDB"), _T("both ' and \""), _T("d"), false, String(), connectionString, error) && !error.IsEmpty(),
         _T("a server name with both kinds of quote was not refused"));
      CheckAdo(!ADOConnection::MakeConnectionString(_T("SQLOLEDB"), _T("x\";Data Source=evil;\""), _T("d"), false, String(), connectionString, error) && !error.IsEmpty(),
         _T("a server name with a double quote and a semicolon was not refused"));
      CheckAdo(!ADOConnection::MakeConnectionString(_T("SQLOLEDB"), _T("s"), _T("both ' and \""), false, String(), connectionString, error) && !error.IsEmpty(),
         _T("a database name with both kinds of quote was not refused"));
      CheckAdo(!ADOConnection::MakeConnectionString(_T("SQLOLEDB;Data Source=evil"), _T("s"), _T("d"), false, String(), connectionString, error),
         _T("a provider that is not a name was not refused"));
      CheckAdo(!ADOConnection::MakeConnectionString(_T("SQLNCLI"), _T("s"), _T("d"), false, _T("p;Data Source=evil"), connectionString, error),
         _T("a failover partner with a semicolon was not refused"));
   }
}


#endif
