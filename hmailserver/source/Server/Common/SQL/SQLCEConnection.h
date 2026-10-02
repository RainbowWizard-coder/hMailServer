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
   class SQLCEConnection : public DALConnection, public std::enable_shared_from_this<SQLCEConnection>
   {
   public:
      SQLCEConnection(std::shared_ptr<DatabaseSettings> pSettings);
      virtual ~SQLCEConnection();

      ConnectionResult Connect(String &sErrorMessage);
      bool Disconnect();
      ExecutionResult TryExecute(const SQLCommand &command, String &sErrorMessage, __int64 *iInsertID = 0, int iIgnoreErrors = 0);
      bool IsConnected() const;
  
      virtual bool GetSupportsCommandParameters() const {return true; }
      _ConnectionPtr GetConnection() const;

      virtual void OnConnected();

      static bool CreateDatabase(const String &sPath, const String &sFile, const String &sPassword,String &sErrorMessage);

      virtual bool BeginTransaction(String &sErrorMessage);
      virtual bool CommitTransaction(String &sErrorMessage);
      virtual bool RollbackTransaction(String &sErrorMessage);
      virtual void SetTimeout(int seconds);

      virtual bool CheckServerVersion(String &errorMessage);

      virtual std::shared_ptr<DALRecordset> CreateRecordset();

      virtual void EscapeString(String &sInput);

      virtual std::shared_ptr<IMacroExpander> CreateMacroExpander();

      void InitializeCommandParameters(_CommandPtr &adoCommand, const SQLCommand &sqlCommand, String &queryString) const;

      // The provider's connection string for a database file and its password,
      // each written as a value ADO reads back whole (ADOConnection::QuoteValue).
      // Until 6.3.6 both were written bare, so a data directory with a semicolon
      // in its name ended the Data Source there and gave the rest of the path to
      // the provider as keywords of its own - "Multiple-step OLE DB operation
      // generated errors" at every start. Windows allows no double quote in a
      // path, so a path always takes double quotes; a password QuoteValue cannot
      // carry is written as it always was.
      static String MakeConnectionString(const String &databaseFile, const String &password);

   private:

      bool GetRequiresUpgrade_(const String &sConnectionString);
      String GetConnectionString_(const String &sDatabase, const String &sPassword) const;
      String GetDatabaseFileName_(const String &sShortName) const;

      virtual bool UpgradeDatabase_();

      DALConnection::ExecutionResult GetErrorType_(int iErrorCode);

      __int64 GetIdentityFromRS_(_RecordsetPtr pRS) const;

      _ConnectionPtr cSQLCEConnection;  
      bool connected_;


   };

   // MakeConnectionString held to ADO's own reading of what it writes: a file
   // path with a semicolon, an equals sign, a quote or spaces in it, and
   // passwords of every quoting, each read back by ADO as given, and nothing
   // else moved. Needs SQL Server Compact 4.0's provider registered, which a
   // server that runs on it has; ClassTester runs it.
   class SQLCEConnectionTester
   {
   public:
      void Test();
   };
}

#endif
