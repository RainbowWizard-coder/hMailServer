// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "PGConnection.h"
#include <cstring>
#include "PGRecordset.h"
#include "DatabaseSettings.h"
#include "DatabaseTls.h"
#include "../Util/Unicode.h"
#include "../Application/IniFileSettings.h"
#include "Macros/PGSQLMacroExpander.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   PGConnection::PGConnection(std::shared_ptr<DatabaseSettings> pSettings) :
      DALConnection(pSettings),
      dbconn_(nullptr)
   {
      is_connected_ = false;
   }

   PGConnection::~PGConnection()
   {
      try
      {
         if (dbconn_)
         {
            PQfinish(dbconn_);
            dbconn_ = 0;
         }
      }
      catch (...)
      {

      }
        
   }

   DALConnection::ConnectionResult
   PGConnection::Connect(String &sErrorMessage)
   {
      try
      {

         String sUsername = database_settings_->GetUsername();
         String sPassword = database_settings_->GetPassword();
         String sServer = database_settings_->GetServer();
         String sDatabase = database_settings_->GetDatabaseName();
         long lDBPort = database_settings_->GetPort();

         // TLS to the server, from hMailServer.ini rather than from PGSSLMODE in the
         // service's environment. Two pairs of keys say it: PostgreSQLSslMode and
         // PostgreSQLSslRootCert, libpq's own words, which came first and win when
         // set; and TlsMode and TlsCaFile, the words every backend takes
         // (DatabaseTls.h). With neither set nothing is added, and libpq's own
         // default - prefer: encrypted when the server offers it, verified never, or
         // whatever PGSSLMODE says - stands as it always has. A mode this code does
         // not know is a refused connection with the reason in the log, not a
         // silent fall-back to that default - a typo in the one setting that is
         // supposed to require verification must not switch verification off.
         DatabaseTls::Mode tlsMode = DatabaseTls::ModeDefault;
         String tlsCaFile;
         if (!DatabaseTls::FromConfiguration(DatabaseSettings::TypePGServer, tlsMode, tlsCaFile, sErrorMessage))
            return FatalError;

         String sslMode = IniFileSettings::Instance()->GetDatabasePostgreSQLSslMode();
         if (!sslMode.IsEmpty())
         {
            static const wchar_t *knownModes[] = { L"disable", L"allow", L"prefer", L"require", L"verify-ca", L"verify-full" };
            bool known = false;
            for (const wchar_t *mode : knownModes)
            {
               if (sslMode.CompareNoCase(mode) == 0)
               {
                  sslMode = mode;
                  known = true;
               }
            }

            if (!known)
            {
               sErrorMessage = "PostgreSQLSslMode in hMailServer.ini is '" + sslMode + "'; it must be one of disable, allow, prefer, require, verify-ca or verify-full. The connection was not attempted.";
               ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5563, "PGConnection::Connect", sErrorMessage);
               return TemporaryFailure;
            }
         }
         else if (tlsMode != DatabaseTls::ModeDefault)
         {
            // libpq's sslmode has the same four words.
            sslMode = DatabaseTls::ModeName(tlsMode);
         }

         // A Windows path's backslashes are escaped with the rest (MakeConninfo).
         String sslRootCert = IniFileSettings::Instance()->GetDatabasePostgreSQLSslRootCert();
         const bool legacySslMode = !IniFileSettings::Instance()->GetDatabasePostgreSQLSslMode().IsEmpty();
         if (sslRootCert.IsEmpty() && !legacySslMode && tlsMode == DatabaseTls::ModeVerifyFull)
         {
            // FromConfiguration hands the file over for verify-full only, so that
            // libpq's require is never quietly made verify-ca by a root file beside it.
            sslRootCert = tlsCaFile;

#ifdef HM_PLATFORM_POSIX
            // No file named: the system's trust store, which libpq 16 and later read
            // for sslrootcert=system. On Windows OpenSSL's store is not the Windows
            // one, so there libpq is left to its own default and says what it looked
            // for when it is not there.
            if (sslRootCert.IsEmpty() && PQlibVersion() >= 160000)
               sslRootCert = _T("system");
#endif
         }

         // Every value quoted and escaped as libpq's conninfo syntax asks, so that a
         // password - or a user, a host, a database or a path - with a quote, a
         // backslash or a space in it reaches the server as it was written.
         AnsiString conninfo;
         if (!MakeConninfo(sServer, lDBPort, sUsername, sPassword, sDatabase, sslMode, sslRootCert, conninfo, sErrorMessage))
            return FatalError;

         dbconn_ = PQconnectdb(conninfo.c_str());


         if (PQstatus(dbconn_) != CONNECTION_OK)
         {
            sErrorMessage = PQerrorMessage(dbconn_);

            // libpq says what went wrong with SSL in its own words; which of the
            // settings to change is this server's to say. Not for a refusal by
            // pg_hba.conf, which names the encryption the attempt used ("SSL
            // encryption") and is the server's rule, not this server's setting.
            String lowerError = sErrorMessage;
            lowerError.MakeLower();
            if ((lowerError.Find(_T("ssl")) >= 0 || lowerError.Find(_T("certificate")) >= 0) && lowerError.Find(_T("pg_hba.conf")) < 0)
            {
               // libpq ends its message with a line break of its own.
               sErrorMessage.TrimRight();
               sErrorMessage += _T("\r\n\r\n") + DatabaseTls::Guidance(DatabaseSettings::TypePGServer, tlsMode);
            }

            return TemporaryFailure;
         }

         // What the session is, as libpq has it. libpq enforced require and the two
         // verify modes before the login; this reads the answer for the diagnostics
         // and the log, and refuses the connection only if a mode that requires TLS
         // somehow produced one without it.
         const bool encrypted = PQsslInUse(dbconn_) != 0;

         // The sslmode libpq used, as it has it: the one written above, or when
         // nothing was, PGSSLMODE from this process's environment, or its own
         // default, prefer.
         String usedSslMode;
         PQconninfoOption *usedOptions = PQconninfo(dbconn_);
         if (usedOptions != nullptr)
         {
            for (PQconninfoOption *option = usedOptions; option->keyword != nullptr; option++)
            {
               if (std::strcmp(option->keyword, "sslmode") == 0 && option->val != nullptr)
                  usedSslMode = String(option->val);
            }

            PQconninfoFree(usedOptions);
         }

         if (usedSslMode.IsEmpty())
            usedSslMode = sslMode.IsEmpty() ? String(_T("prefer")) : sslMode;

         String effectiveSslMode = usedSslMode;
         effectiveSslMode.MakeLower();
         const bool tlsRequired = effectiveSslMode == _T("require") || effectiveSslMode == _T("verify-ca") || effectiveSslMode == _T("verify-full");

         if (tlsRequired && !encrypted)
         {
            sErrorMessage = Formatter::Format("The connection to the PostgreSQL server at {0} is not encrypted, and sslmode is {1}: it was closed before it was used.",
               sServer, usedSslMode);
            PQfinish(dbconn_);
            dbconn_ = 0;
            return TemporaryFailure;
         }

         String transportDetail;
         if (encrypted)
         {
            const char *protocol = PQsslAttribute(dbconn_, "protocol");
            const char *cipher = PQsslAttribute(dbconn_, "cipher");
            transportDetail = String(protocol != nullptr ? protocol : "TLS");
            if (cipher != nullptr)
               transportDetail += _T(", ") + String(cipher);
         }
         else if (effectiveSslMode == _T("disable"))
            transportDetail = _T("TLS is switched off");
         else
            transportDetail = _T("the server offered no TLS");

         const bool verified = encrypted && (effectiveSslMode == _T("verify-ca") || effectiveSslMode == _T("verify-full"));
         String description = DatabaseTls::Describe(DatabaseSettings::TypePGServer, encrypted, transportDetail, verified, tlsMode);
         if (legacySslMode)
            description += _T(", sslmode ") + sslMode + _T(" from PostgreSQLSslMode");
         else if (sslMode.IsEmpty() && effectiveSslMode != _T("prefer"))
            description += _T(", sslmode ") + usedSslMode + _T(" from libpq's environment (PGSSLMODE)");

         SetTransportSecurity_(encrypted, description);

         is_connected_ = true;

         // The server says whether a backslash in a literal is an ordinary
         // character; every string escaped from here on follows its answer.
         // A server that does not report it (none since 8.2) is taken to
         // escape, the safe reading.
         NoteServerParameters(dbconn_);

         // statement_timeout is a session setting and this session has just been
         // created, so it starts at whatever postgresql.conf says - normally no
         // limit at all. Set after is_connected_, because SetTimeout runs a
         // statement and declines to run one on a connection that is not up yet.
         ApplyDefaultTimeout();
      }
      catch (...)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5085, "PGConnection::Connect", "An unhandled error occurred when connecting to the database");

         return TemporaryFailure;
      }
          
      return Connected;
   }

   bool
   PGConnection::Disconnect()
   {
      if (dbconn_)
      {
         PQfinish(dbconn_);
         dbconn_ = 0;
      }

      // Was left true, so IsConnected() went on claiming a connection that had
      // been finished. Connect() resets it on the way back up.
      is_connected_ = false;

      return true;
   }

   __int64
   PGConnection::ReadCommandTuples(const char *text)
   {
      if (text == nullptr || text[0] == 0)
         return -1;

      const __int64 largest = 0x7FFFFFFFFFFFFFFFLL;

      __int64 count = 0;
      for (const char *digit = text; *digit != 0; digit++)
      {
         if (*digit < '0' || *digit > '9')
            return -1;

         // No count reaches this; a text that would overflow is no count either.
         if (count > (largest - 9) / 10)
            return -1;

         count = count * 10 + (*digit - '0');
      }

      return count;
   }

   DALConnection::ExecutionResult
   PGConnection::TryExecute(const SQLCommand &command, String &sErrorMessage, __int64 *iInsertID, int iIgnoreErrors, __int64 *rowsAffected)
   {
      if (rowsAffected != nullptr)
         *rowsAffected = -1;

      NoteServerParameters(dbconn_);
      String SQL = command.GetQueryString();

      try
      {
         // PG_query-doc:
         // Zero if the query was successful. Non-zero if an error occurred.
         // 
         AnsiString sQuery;
         if (!Unicode::WideToMultiByte(SQL, sQuery))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5106, "PGConnection::TryExecute", "Could not convert string into multi-byte.");
            return DALConnection::DALUnknown;
         }

         // Cleared before the statement runs, the way ADOConnection does it. A
         // plain INSERT only comes back with a row when SQLStatement::GetCommand
         // appended a RETURNING clause, so without this the caller kept whatever
         // it happened to be holding - on the shared "insert then read the id"
         // path that is another row's identity.
         if (iInsertID != nullptr)
            *iInsertID = 0;

         PGresult *pResult = PQexec(dbconn_, sQuery);

         // Classified before the marker is consulted, because a marked statement
         // may discard "this object already exists" but never "the server never
         // heard the statement" - see DALConnection::HasIgnoreErrorsMarker. The
         // error text goes into a local so a discarded failure leaves nothing in
         // the caller's buffer.
         String checkErrorMessage;
         DALConnection::ExecutionResult result = CheckError(pResult, SQL, checkErrorMessage);

         bool failureIgnored = false;

         if (result != DALSuccess)
         {
            // A caller that passes DALErrorInSQL is saying that a row which already
            // exists is not a failure - PersistentKnownSender::Insert, which loses a
            // find-then-insert race on its unique index by design. The other three
            // backends honour it through their error-type mapping; this one ignored
            // the argument, so the race put HM5032 in the error log at High. It is
            // narrowed to unique_violation, so the flag never hides a statement
            // PostgreSQL refused for any other reason.
            const char *sqlState = pResult != 0 ? PQresultErrorField(pResult, PG_DIAG_SQLSTATE) : nullptr;

            bool ignoreByCaller = result == DALConnection::DALErrorInSQL &&
                                  (iIgnoreErrors & DALConnection::DALErrorInSQL) != 0 &&
                                  sqlState != nullptr && strcmp(sqlState, "23505") == 0;

            if (ignoreByCaller)
            {
               PQclear(pResult);

               // The refused insert changed no row (DALConnection.h).
               if (rowsAffected != nullptr)
                  *rowsAffected = 0;

               return DALConnection::DALSuccess;
            }

            bool ignoreByMarker = result != DALConnection::DALConnectionProblem &&
                                  HasIgnoreErrorsMarker(SQL);

            if (!ignoreByMarker)
            {
               sErrorMessage = checkErrorMessage;

               if (pResult != 0)
                  PQclear(pResult);

               return result;
            }

            failureIgnored = true;
         }

         // PQcmdTuples: the rows the command counted - for an UPDATE every row it
         // matched (DALConnection.h) - and empty for a command that counts none.
         if (rowsAffected != nullptr)
            *rowsAffected = failureIgnored ? 0 : ReadCommandTuples(PQcmdTuples(pResult));

         ExecStatusType iExecResult = PQresultStatus(pResult);

         // Check if a value has been returned. Will only occur if we've
         // inserted a value.
         if (iInsertID != nullptr && iExecResult == PGRES_TUPLES_OK)
         {
            // pick the ID from the first row.
            char *pRetVal = PQgetvalue(pResult, 0, 0);
            *iInsertID = pRetVal ? _atoi64(pRetVal) : 0;
         }

         if (pResult != 0)
            PQclear(pResult);
        
      }
      catch (...)
      {
         if (rowsAffected != nullptr)
            *rowsAffected = -1;

         sErrorMessage = "Source: PGConnection::TryExecute, Code: HM5084, Description: An unhanded error occurred while executing: " + SQL;
         return DALConnection::DALUnknown;
      }

      return DALConnection::DALSuccess;
   }

   bool
   PGConnection::IsConnected() const
   {
      return is_connected_;
   }

   PGconn*
   PGConnection::GetConnection() const
   {
      return dbconn_;
   }

   DALConnection::ExecutionResult
   PGConnection::CheckError(PGresult *pResult, const String &sAdditionalInfo, String &sOutputErrorMessage) const
   {
      try
      {
         String sErrorMsg = "";

         DALConnection::ExecutionResult result = DALConnection::DALUnknown;
         
         if (pResult)
         {  
            ExecStatusType iExecResult = PQresultStatus(pResult);

            if (iExecResult == PGRES_COMMAND_OK || iExecResult == PGRES_TUPLES_OK) 
            {
               last_sql_state_.clear();
               result = DALConnection::DALSuccess;
               return result;
            }
            else if (iExecResult == PGRES_FATAL_ERROR)
            {
               result = DALConnection::DALErrorInSQL;
            }

            // What the failure was, by its SQLSTATE, for ClassifyLastFailure: a
            // deadlock or a serialisation failure, a row that is there already, or
            // a connection that went (DatabaseTransaction acts on each).
            const char *sqlState = PQresultErrorField(pResult, PG_DIAG_SQLSTATE);
            last_sql_state_ = sqlState != nullptr ? sqlState : "";

            // Retrieve error message
            sErrorMsg  = PQresultErrorMessage(pResult);

            /*
               A connection that died while the statement was in flight does not
               come back as a null result: libpq builds a PGRES_FATAL_ERROR
               result ("server closed the connection unexpectedly") and marks the
               connection bad. Classified on the result status alone that is
               DALErrorInSQL, which DALConnection::Execute and DALRecordset::Open
               both treat as final - so the statement was reported failed and
               never retried, and it took a *second* statement (which libpq
               refuses outright, giving the null result below) before anything
               reconnected. On MySQL the same event is recognised, through error
               codes 2006/2013, and the statement is retried; PostgreSQL was the
               one backend that dropped it.

               PQstatus is what separates the two: a statement the server
               rejected leaves the connection CONNECTION_OK, a statement that
               never got an answer does not.
            */
            if (result != DALConnection::DALSuccess && PQstatus(dbconn_) != CONNECTION_OK)
               result = DALConnection::DALConnectionProblem;
         }
         else
         {
            last_sql_state_.clear();

            sErrorMsg  = PQerrorMessage(dbconn_);

            if (sErrorMsg.IsEmpty())
               sErrorMsg = "Unknown error. Error structure not initialized.";

            result = DALConnection::DALConnectionProblem;
         }

         String sErrorMessage;
         sErrorMessage.Format(_T("Postgres: %s (Additional info: %s)"), sErrorMsg.c_str(), sAdditionalInfo);

         sOutputErrorMessage = sErrorMessage;

         return result;
      }
      catch (...)
      {
         ErrorManager::Instance()->ReportError(HM::ErrorManager::High, 5083, "PGConnection::CheckError", "An unhandled error occurred while checking for errors.");

         return DALConnection::DALUnknown;
      }
   }

   void 
   PGConnection::OnConnected()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Not much going on here...
   //---------------------------------------------------------------------------()
   {
      
   }

   bool 
   PGConnection::BeginTransaction(String &sErrorMessage)
   {
      return TryExecute(SQLCommand("BEGIN TRANSACTION"), sErrorMessage, 0, 0) == DALSuccess;
   }

   bool 
   PGConnection::CommitTransaction(String &sErrorMessage)
   {
      // A transaction in which a statement has failed is answered, when it is
      // asked to COMMIT, with success and the command tag ROLLBACK: PostgreSQL
      // rolls it back. TryExecute looks at the status alone, so this said
      // committed, and every caller - SchemaUpgrader, COM's
      // Database.CommitTransaction - reads true as saved. It is rolled back here
      // instead, and said to be.
      if (IsTransactionFailed())
      {
         String rollbackError;
         TryExecute(SQLCommand("ROLLBACK TRANSACTION"), rollbackError, 0, 0);

         sErrorMessage = "Postgres: the transaction was rolled back, not committed: a statement in it had failed, and PostgreSQL does not commit a transaction after that.";
         return false;
      }

      return TryExecute(SQLCommand("COMMIT TRANSACTION"), sErrorMessage, 0, 0) == DALSuccess;
   }

   bool 
   PGConnection::RollbackTransaction(String &sErrorMessage)
   {
      return TryExecute(SQLCommand("ROLLBACK TRANSACTION"), sErrorMessage, 0, 0) == DALSuccess;
   }

   DALConnection::LastFailure
   PGConnection::ClassifyLastFailure(ExecutionResult result)
   {
      if (result == DALConnection::DALConnectionProblem || dbconn_ == 0 || PQstatus(dbconn_) != CONNECTION_OK)
         return LastFailureConnection;

      // Class 08, connection exception.
      if (last_sql_state_.compare(0, 2, "08") == 0)
         return LastFailureConnection;

      // serialization_failure and deadlock_detected: the transaction is lost, and
      // may be run again from the start.
      if (last_sql_state_ == "40001" || last_sql_state_ == "40P01")
         return LastFailureRetryable;

      // unique_violation, the one failure TryExecute lets a caller ignore.
      if (last_sql_state_ == "23505")
         return LastFailureDuplicate;

      return LastFailureOther;
   }

   void
   PGConnection::ForgetLastFailure()
   {
      last_sql_state_.clear();
   }

   bool
   PGConnection::IsTransactionFailed() const
   {
      return dbconn_ != 0 && PQtransactionStatus(dbconn_) == PQTRANS_INERROR;
   }

   void
   PGConnection::SetTimeout(int seconds)
   {
      // See the comment on the declaration. Nothing to do before the session
      // exists; Connect() calls this again once it does.
      if (dbconn_ == nullptr || !is_connected_)
         return;

      // statement_timeout is in milliseconds and its maximum is INT_MAX, so the
      // multiplication is done in 64 bits and clamped rather than being allowed to
      // wrap a caller's large "seconds" into a small - or negative - number of
      // milliseconds. SQLScriptRunner asks for 1800 seconds, which is nowhere near
      // the ceiling; an administrator who writes a nonsense value into the ini is
      // the case this guards.
      __int64 milliseconds = 0;

      if (seconds > 0)
      {
         const __int64 maximum_milliseconds = 2147483647;

         milliseconds = (__int64) seconds * 1000;

         if (milliseconds > maximum_milliseconds)
            milliseconds = maximum_milliseconds;
      }

      String sql;
      sql.Format(_T("SET statement_timeout = %I64d"), milliseconds);

      String errorMessage;

      if (TryExecute(SQLCommand(sql), errorMessage, 0, 0) != DALConnection::DALSuccess)
      {
         // Logged rather than reported: the connection is usable, this is the one
         // statement on it that failed, and a role that may not SET statement_timeout
         // would otherwise report an error on every connection in the pool at every
         // start. What it costs is the timeout, and the line below says so.
         LOG_APPLICATION("The PostgreSQL statement_timeout could not be set on this connection, so "
                         "statements on it are not time-limited. " + errorMessage);
      }
   }

   bool
   PGConnection::CheckServerVersion(String &errorMessage)
   {

      return true;
   }

   std::shared_ptr<DALRecordset> 
   PGConnection::CreateRecordset()
   {
      std::shared_ptr<PGRecordset> recordset = std::shared_ptr<PGRecordset>(new PGRecordset());
      return recordset;
   }

   std::atomic<bool> PGConnection::standard_conforming_strings_(false);

   bool
   PGConnection::StandardConformingStrings()
   {
      return standard_conforming_strings_;
   }

   void
   PGConnection::NoteServerParameters(PGconn *connection)
   {
      if (connection == nullptr)
         return;
      const char *conforming = PQparameterStatus(connection, "standard_conforming_strings");
      standard_conforming_strings_ = (conforming != nullptr && strcmp(conforming, "on") == 0);
   }

   void
   PGConnection::EscapeString(String &sInput)
   {
      sInput.Replace(_T("'"), _T("''"));

      // A backslash is doubled only where the server would otherwise read it
      // as an escape; with standard_conforming_strings on, doubling it stored
      // it doubled - every Windows path in a setting, every backslash in a
      // signature or a rule, on PostgreSQL, until 14 September 2026.
      if (!StandardConformingStrings())
         sInput.Replace(_T("\\"), _T("\\\\"));
   }

   std::shared_ptr<IMacroExpander> 
   PGConnection::CreateMacroExpander()
   {
      std::shared_ptr<PGSQLMacroExpander> expander = std::shared_ptr<PGSQLMacroExpander>(new PGSQLMacroExpander());
      return expander;
   }

   AnsiString
   PGConnection::QuoteConninfoValue(const AnsiString &bytes)
   {
      // libpq, "Connection Strings": "To write an empty value, or a value
      // containing spaces, surround it with single quotes... Single quotes and
      // backslashes within a value must be escaped with a backslash, i.e., \' and
      // \\." Every value is quoted, so that none depends on what it holds.
      AnsiString quoted = "'";
      const char *text = bytes.c_str();
      const int length = bytes.GetLength();
      for (int i = 0; i < length; i++)
      {
         if (text[i] == '\'' || text[i] == '\\')
            quoted += '\\';
         quoted += text[i];
      }
      quoted += '\'';
      return quoted;
   }

   namespace
   {
      // A path as libpq's fopen reads it: on Windows the ANSI code page, refusing a
      // character it does not have rather than writing a best fit that names
      // another file; elsewhere UTF-8, which is what a POSIX file name is here.
      bool PathBytes(const String &path, AnsiString &bytes)
      {
#ifdef HM_PLATFORM_POSIX
         return Unicode::ToUtf8Exact(path, bytes);
#else
         bytes.Empty();
         if (path.IsEmpty())
            return true;

         BOOL defaulted = FALSE;
         const int size = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, path.c_str(), path.GetLength(), nullptr, 0, nullptr, &defaulted);
         if (size <= 0 || defaulted)
            return false;

         std::string narrow((size_t) size, '\0');
         if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, path.c_str(), path.GetLength(), &narrow[0], size, nullptr, &defaulted) != size || defaulted)
            return false;

         bytes = narrow.c_str();
         return bytes.GetLength() == size;
#endif
      }
   }

   bool
   PGConnection::MakeConninfo(const String &server, long port, const String &user, const String &password,
                              const String &database, const String &sslMode, const String &sslRootCert,
                              AnsiString &conninfo, String &error)
   {
      conninfo.Empty();
      error.Empty();

      struct Value
      {
         const char *keyword;
         String text;
         const TCHAR *what;
      };

      String portText;
      portText.Format(_T("%d"), (int) port);

      const Value values[] =
      {
         { "host", server, _T("The database server's name") },
         { "port", portText, _T("The database server's port") },
         { "user", user, _T("The database user") },
         { "password", password, _T("The database password") },
         { "dbname", database.IsEmpty() ? String(_T("postgres")) : database, _T("The database name") },
      };

      for (const Value &value : values)
      {
         AnsiString bytes;
         if (!ClientBytes(value.text, bytes))
         {
            error = String(value.what) + _T(" in hMailServer.ini holds a character UTF-8 cannot carry (half of a surrogate pair), so it cannot be passed to PostgreSQL. The connection was not attempted.");
            conninfo.Empty();
            return false;
         }

         if (!conninfo.IsEmpty())
            conninfo += " ";
         conninfo += AnsiString(value.keyword) + "=" + QuoteConninfoValue(bytes);
      }

      if (!sslMode.IsEmpty())
      {
         AnsiString bytes;
         if (!ClientBytes(sslMode, bytes))
         {
            error = _T("The PostgreSQL sslmode cannot be written for libpq. The connection was not attempted.");
            conninfo.Empty();
            return false;
         }
         conninfo += " sslmode=" + QuoteConninfoValue(bytes);
      }

      if (!sslRootCert.IsEmpty())
      {
         AnsiString bytes;
         if (!PathBytes(sslRootCert, bytes))
         {
            error = _T("The PostgreSQL root certificate's path, ") + sslRootCert + _T(", holds a character this system's code page does not have, and libpq opens the file by that name. Move the file to a path without it. The connection was not attempted.");
            conninfo.Empty();
            return false;
         }
         conninfo += " sslrootcert=" + QuoteConninfoValue(bytes);
      }

      return true;
   }

   namespace
   {
      void CheckConninfo(bool condition, const std::string &what)
      {
         if (!condition)
            throw std::logic_error("PGConnectionTester: " + what);
      }

      // The value libpq reads for keyword out of conninfo, by its own parser.
      bool ParsedValue(const AnsiString &conninfo, const char *keyword, std::string &value, std::string &error)
      {
         char *message = nullptr;
         PQconninfoOption *options = PQconninfoParse(conninfo.c_str(), &message);
         if (options == nullptr)
         {
            error = message != nullptr ? message : "PQconninfoParse refused it";
            if (message != nullptr)
               PQfreemem(message);
            return false;
         }

         bool found = false;
         for (PQconninfoOption *option = options; option->keyword != nullptr; option++)
         {
            if (std::strcmp(option->keyword, keyword) == 0 && option->val != nullptr)
            {
               value = option->val;
               found = true;
            }
         }

         PQconninfoFree(options);
         if (!found)
            error = std::string("no ") + keyword + " in it";
         return found;
      }
   }

   void
   PGConnectionTester::Test()
   {
      // Every value MakeConninfo writes read back by libpq's own parser as exactly
      // the value given - the password above all, in every position a value takes:
      // quotes, backslashes, spaces, an equals sign, a keyword's text, a value that
      // is all escape, an empty one, and characters outside ASCII in the bytes the
      // client is handed them in (DALConnection::ClientBytes, which
      // MySQLConnectionTester holds to its bytes).
      const wchar_t *hostile[] =
      {
         L"plain",
         L"",
         L"'",
         L"\\",
         L"\\'",
         L"x\\",
         L"it's",
         L"back\\slash",
         L"two words",
         L" leading and trailing ",
         L"a=b",
         L"semi;colon",
         L"double\"quote",
         L"x' host='evil.example' password='y",
         L"x\\' host=evil.example",
         L"''''",
         L"\\\\\\",
         L"p\u00e4ssw\u00f6rd",
         L"\u65e5\u672c\u8a9e\u306e\u30d1\u30b9\u30ef\u30fc\u30c9",
         L"tab\there",
      };

      for (const wchar_t *text : hostile)
      {
         const String value(text);
         AnsiString expected;
         CheckConninfo(DALConnection::ClientBytes(value, expected), "a test value could not be written as the client is handed it");

         AnsiString conninfo;
         String error;
         CheckConninfo(PGConnection::MakeConninfo(_T("db.example"), 5432, value, value, value, _T("verify-full"), String(), conninfo, error),
            "MakeConninfo refused a value it should write: " + std::string(Unicode::ToUtf8(error).c_str()));

         for (const char *keyword : { "user", "password", "dbname" })
         {
            // An empty database is postgres, by design (below).
            const std::string wanted = std::strcmp(keyword, "dbname") == 0 && expected.IsEmpty()
               ? std::string("postgres") : std::string(expected.c_str(), (size_t) expected.GetLength());

            std::string parsed;
            std::string why;
            CheckConninfo(ParsedValue(conninfo, keyword, parsed, why),
               std::string("libpq did not read ") + keyword + " back from " + conninfo.c_str() + ": " + why);
            CheckConninfo(parsed == wanted,
               std::string("libpq read ") + keyword + " as [" + parsed + "] and not [" + wanted + "] from " + conninfo.c_str());
         }

         // And nothing else: the host, the port and the mode are the ones given,
         // whatever the other values held.
         std::string parsed;
         std::string why;
         CheckConninfo(ParsedValue(conninfo, "host", parsed, why) && parsed == "db.example", std::string("a value moved the host: ") + conninfo.c_str());
         CheckConninfo(ParsedValue(conninfo, "port", parsed, why) && parsed == "5432", std::string("a value moved the port: ") + conninfo.c_str());
         CheckConninfo(ParsedValue(conninfo, "sslmode", parsed, why) && parsed == "verify-full", std::string("a value moved sslmode: ") + conninfo.c_str());
      }

      // An empty database is postgres; the mode and the root certificate are left
      // out when not given, and a path's backslashes are the path's.
      AnsiString conninfo;
      String error;
      CheckConninfo(PGConnection::MakeConninfo(_T("h"), 1, _T("u"), _T("p"), String(), String(), _T("C:\\Program Files\\hMailServer\\it's ca.pem"), conninfo, error),
         "MakeConninfo refused a root certificate's path");
      std::string parsed;
      std::string why;
      CheckConninfo(ParsedValue(conninfo, "dbname", parsed, why) && parsed == "postgres", std::string("an empty database is not postgres: ") + conninfo.c_str());
      CheckConninfo(ParsedValue(conninfo, "sslrootcert", parsed, why) && parsed == "C:\\Program Files\\hMailServer\\it's ca.pem",
         std::string("the root certificate's path was not read back as given: ") + conninfo.c_str());
      CheckConninfo(!ParsedValue(conninfo, "sslmode", parsed, why) || parsed.empty(), std::string("sslmode was written when none was given: ") + conninfo.c_str());

      // A value UTF-8 cannot carry is refused, not written as something else.
      String lone(L"half");
      lone += (wchar_t) 0xD800;
      CheckConninfo(!PGConnection::MakeConninfo(_T("h"), 1, _T("u"), lone, _T("d"), String(), String(), conninfo, error) && conninfo.IsEmpty() && !error.IsEmpty(),
         "a password with half a surrogate pair was not refused");
   }
}