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
      ExecutionResult TryExecute(const SQLCommand &command, String &sErrorMessage, __int64 *iInsertID = 0, int iIgnoreErrors = 0);
      bool IsConnected() const;
   
      virtual bool GetSupportsCommandParameters() const {return true; }
      _ConnectionPtr GetConnection() const;

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
}

#endif
