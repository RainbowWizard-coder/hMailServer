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
      ExecutionResult TryExecute(const SQLCommand &command, String &sErrorMessage, __int64 *iInsertID = 0, int iIgnoreErrors = 0, __int64 *rowsAffected = nullptr);
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

      friend class SQLCEConnectionTester;

      bool GetRequiresUpgrade_(const String &sConnectionString);
      String GetConnectionString_(const String &sDatabase, const String &sPassword) const;
      String GetDatabaseFileName_(const String &sShortName) const;

      // A 3.5 database file upgraded to 4.0 in place, as Connect asks when the
      // provider says it needs to be: the engine writes the upgraded copy beside
      // it, the copy is proven, and only then is the 3.5 file moved aside - to
      // .backup_v52, or the first .backup_v52.<n> that holds nothing - and the
      // copy put in its place. Any failure leaves the live file where it was
      // (or, if the copy could not take its place, puts it back), says so in
      // the error log (HM5122 to HM5125) and answers false, and the service
      // does not start on it; the next start tries again. Until 6.3.6 the
      // engine's own answer was not read, so a 3.5 file the engine could not
      // upgrade was moved aside all the same and the server was left without
      // its database. error is the sentence reported, which the start's own
      // failure (HM4354) carries as well.
      virtual bool UpgradeDatabase_(String &error);

      // The whole of that, without the error log: what went wrong is the error,
      // and backupFile where the 3.5 file went. The tester's way in.
      static bool UpgradeDatabaseFile_(const String &liveFile, const String &upgradedFile, const String &password,
                                       String &backupFile, String &error, int &errorCode);

      // The upgraded copy's two proofs: the engine reads every page of it
      // (Verify), and the provider the server opens it with opens it with the
      // password.
      static bool ProveDatabaseFile_(const String &file, const String &password, String &error);

      // The upgraded copy in the live file's place, once it is proven; the 3.5
      // file moved aside first, and put back if the copy cannot take its place.
      static bool InstallUpgradedCopy_(const String &liveFile, const String &upgradedFile, const String &password,
                                       String &backupFile, String &error, int &errorCode);

      DALConnection::ExecutionResult GetErrorType_(int iErrorCode);

      __int64 GetIdentityFromRS_(_RecordsetPtr pRS) const;

      _ConnectionPtr cSQLCEConnection;  
      bool connected_;


   };

   // MakeConnectionString held to ADO's own reading of what it writes: a file
   // path with a semicolon, an equals sign, a quote or spaces in it, and
   // passwords of every quoting, each read back by ADO as given, and nothing
   // else moved. And the 3.5-to-4.0 upgrade held to leaving the live file in
   // place whenever the engine cannot upgrade it - a damaged file, a 4.0 file,
   // a file another process holds - and to putting a proven copy in its place
   // only once the 3.5 file is safely aside. Needs SQL Server Compact 4.0's
   // provider and engine registered, which a server that runs on it has;
   // ClassTester runs it.
   class SQLCEConnectionTester
   {
   public:
      void Test();

   private:
      void TestUpgrade_();
   };
}

#endif
