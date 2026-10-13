// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DALConnection.h"

// ADO and SQL Server Compact are COM. This class is declared only on Windows,
// because every part of it is written in terms of the ADO smart pointers the
// Windows precompiled header #imports and those have no POSIX shape; the
// roadmap section "Linux and AArch64" - the row "Database backends that
// survive" - leaves both backends out of the POSIX build, where
// DALConnectionFactory refuses them by name.
#ifndef HM_PLATFORM_POSIX

namespace HM
{
   class ADOConnection : public DALConnection, public std::enable_shared_from_this<ADOConnection>
   {
   public:
      enum ServerInfo
      {
         RequiredVersion = 8
      };

      ADOConnection(std::shared_ptr<DatabaseSettings> pSettings);
      virtual ~ADOConnection();

      ConnectionResult Connect(String &sErrorMessage);
      bool Disconnect();
      ExecutionResult TryExecute(const SQLCommand &command, String &sErrorMessage, __int64 *iInsertID = 0, int iIgnoreErrors = 0, __int64 *rowsAffected = nullptr);
      bool IsConnected() const;

      virtual bool GetSupportsCommandParameters() const {return true; }
      _ConnectionPtr GetConnection() const;

      // What Command.Execute handed back as RecordsAffected, as TryExecute's
      // rowsAffected: the count, or -1 when there is none - a negative count, which
      // is ADO's own "unknown" (a statement that counts nothing, or SQL Server under
      // NOCOUNT), or a value that is no number. A long in practice (VT_I4, measured
      // on SQL Server Compact 4.0 on 12 October 2026); any integer type is read.
      // SQLCEConnection, which calls ADO the same way, reads it here too.
      static __int64 ReadRecordsAffected(const VARIANT &recordsAffected);

      virtual void OnConnected();

      virtual bool BeginTransaction(String &sErrorMessage);
      virtual bool CommitTransaction(String &sErrorMessage);
      virtual bool RollbackTransaction(String &sErrorMessage);
      virtual void SetTimeout(int seconds);

      static DALConnection::ExecutionResult GetErrorType(int iErrorCode);
      virtual bool CheckServerVersion(String &errorMessage);
      virtual std::shared_ptr<DALRecordset> CreateRecordset();
      virtual void EscapeString(String &sInput);
      virtual std::shared_ptr<IMacroExpander> CreateMacroExpander();

      void InitializeCommandParameters(_CommandPtr &adoCommand, const SQLCommand &sqlCommand, String &queryString) const;

      // The connection string Connect hands ADO, less the TLS keywords and
      // ConnectionStringOptions it appends: Provider, then Data Source (the server,
      // as HOST, HOST\INSTANCE or HOST,PORT) and Initial Catalog each in the quotes
      // ADO reads a value in, then Integrated Security=SSPI when there is no user,
      // and FailoverPartner when there is one. The user and the password are never
      // in it: Open takes them as arguments, which ADO reads nothing in, so either
      // may hold any character. False, with the reason, for a value no connection
      // string carries: a server or database name holding a double quote beside a
      // single quote or a semicolon (QuoteValue), a provider that is not a plain
      // name, or a failover partner - written into the provider's own string,
      // which reads no quotes - with a quote, a semicolon, a brace or an equals
      // sign in it.
      static bool MakeConnectionString(const String &provider, const String &server, const String &database, bool integratedSecurity,
                                       const String &failoverPartner, String &connectionString, String &error);

      // One value as MakeConnectionString writes it: empty as nothing, otherwise in
      // double quotes when it holds none, else in single quotes when it holds
      // neither a single quote nor a semicolon - the forms ADO's own parser reads.
      // False for any other value.
      static bool QuoteValue(const String &value, String &quoted);

   private:

      // What one Open came to: opened; refused by the provider with a COM error,
      // whose description and source OpenOnce_ hands back; an HRESULT that was not
      // success without an exception; anything else.
      enum OpenOutcome
      {
         OpenSucceeded = 0,
         OpenRefused = 1,
         OpenFailed = 2,
         OpenUnknownError = 3
      };

      int OpenOnce_(const String &connectionString, const String &username, const String &password, String &errorDescription, String &errorSource);

      // sys.dm_exec_connections.encrypt_option of this session - TRUE or FALSE - or
      // empty when the login may not read it (VIEW SERVER STATE), which is the
      // usual case for an application's login.
      String ReadEncryptOption_();

      __int64 GetIdentityFromRS_(_RecordsetPtr pRS) const;

      bool IsMSOLEDBSQL18OrLaterInstalled_() const;

      _ConnectionPtr cADOConnection;  
      bool bConnected;

   };

   // ADOConnection::MakeConnectionString held to ADO's own reading of what it
   // writes (an ADO Connection given the string, its Data Source and Initial
   // Catalog properties read back): hostile server and database names read back as
   // given, none moving another value, and what cannot be written refused. Throws
   // std::logic_error with a sentence. ClassTester runs it inside the service.
   class ADOConnectionTester
   {
   public:
      void Test();
   };
}

#endif
