// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "./DatabaseConnectionManager.h"

#include "DALConnection.h"
#include "DALConnectionFactory.h"
#include "DatabaseSettings.h"
#include "DatabaseTransaction.h"
#include "DatabaseUnavailableMarker.h"

#include "../Util/AuditTrail.h"

// ADORecordset names the ADO smart pointers the Windows precompiled header
// #imports; the roadmap section "Linux and AArch64" leaves that backend out of
// the POSIX build. Nothing below uses the type, only the include had to go.
#ifdef _MSC_VER
#include "ADORecordset.h"
#endif
#include "MySQLRecordset.h"
#include "PGRecordset.h"
// SQLCERecordset holds an ADO _RecordsetPtr, the same reason ADORecordset.h is
// guarded above; SQL Server Compact is the other backend the POSIX build leaves
// out. Nothing below uses the type.
#ifdef _MSC_VER
#include "SQLCERecordset.h"
#endif
#include "MySQLInterface.h"

#include "SQLCommand.h"

#include "Prerequisites/PrerequisiteList.h"
#include "SQLScriptRunner.h"

#include "../Util/ServerStatus.h"
#include "../Util/OtelTracer.h"

#include <boost/chrono.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif


#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // Connect_'s answer when the close before the process ends began while
      // it made its connections (Pool_, CheckConnectionMade_).
      const char *const ClosedBeforeExitMessage = "The database was closed for the end of the process while its connections were being made.";
   }

   DatabaseConnectionManager::DatabaseConnectionManager(void)
   {
   }

   DatabaseConnectionManager::~DatabaseConnectionManager(void)
   {
   }

   bool
   DatabaseConnectionManager::CreateConnections(std::shared_ptr<DatabaseSettings> settings, int connections, String &sErrorMessage)
   {
      if (!settings || connections < 1)
      {
         sErrorMessage = "A pool needs the database's settings and at least one connection.";
         return false;
      }

      if (Connect_(settings, connections, sErrorMessage) != DALConnection::Connected)
         return false;

      sErrorMessage = "";
      return true;
   }

   bool
   DatabaseConnectionManager::CreateConnections(String &sErrorMessage)
   {
      int iNoOfConnectionAttempts = IniFileSettings::Instance()->GetNumberOfDatabaseConnectionAttempts();
      int iDelayBetweenConnectionAttempt = IniFileSettings::Instance()->GetDBConnectionAttemptsDelay() * 1000;

      bool bConnectionOK = false;
      for (int iTry = 1; iTry <= iNoOfConnectionAttempts ; iTry++)
      {
         // [Database] read at every attempt, as it always was.
         IniFileSettings *pIniFileSettings = IniFileSettings::Instance();

         std::shared_ptr<DatabaseSettings> pSettings = std::shared_ptr<DatabaseSettings>
            (new DatabaseSettings(pIniFileSettings->GetDatabaseProvider(), pIniFileSettings->GetDatabaseServer(), pIniFileSettings->GetDatabaseName(),
                                  pIniFileSettings->GetUsername(), pIniFileSettings->GetPassword(), pIniFileSettings->GetDatabaseDirectory(),
                                  pIniFileSettings->GetDatabaseServerFailoverPartner(), pIniFileSettings->GetDatabaseType(), pIniFileSettings->GetDatabasePort()));

         DALConnection::ConnectionResult iResult = Connect_(pSettings, pIniFileSettings->GetNumberOfDatabaseConnections(), sErrorMessage);

         switch (iResult)
         {
         case DALConnection::Connected:
            // Reset the error message. If we failed to connect the first time,
            // but succeeded the second, it's not really an error to care about.
            sErrorMessage = "";
            return true;
         case DALConnection::FatalError:
            // Skip out and report error.
            return false;
         case DALConnection::TemporaryFailure:
            // We failed to connect to the database server.
            // Pause a few seconds and then try again.
            Sleep(iDelayBetweenConnectionAttempt);
            break;
         }




      }  

      return false;
   }

   DALConnection::ConnectionResult
   DatabaseConnectionManager::Connect_(std::shared_ptr<DatabaseSettings> pSettings, int iNoOfConnections, String &sErrorMessage)
   {
      for (int i = 0; i < iNoOfConnections; i++)
      {
         std::shared_ptr<DALConnection> pConnection = DALConnectionFactory::CreateConnection(pSettings);

#ifdef HM_PLATFORM_POSIX
         // The factory has just refused a backend this build does not carry - SQL
         // Server or SQL Server Compact - and has said which in the error log.
         // Without this the empty pointer would be dereferenced on the next line
         // and the server would die instead of reporting why it cannot start. The
         // test is POSIX-only deliberately: the Windows switch has a case for every
         // type it is given, so the pointer cannot be empty there, and this file is
         // under a running regression gate that must compile the same bytes it did
         // before.
         if (!pConnection)
         {
            sErrorMessage = "The database type configured in hMailServer.INI is not available in this build. See the error log for which one was asked for.";
            return DALConnection::FatalError;
         }
#endif

         DALConnection::ConnectionResult result = pConnection->Connect(sErrorMessage);

         if (result != DALConnection::Connected)
            return result;

         if (!Pool_(pConnection))
         {
            sErrorMessage = ClosedBeforeExitMessage;
            return DALConnection::FatalError;
         }
      }

      return CheckConnectionMade_(sErrorMessage);
   }

   bool
   DatabaseConnectionManager::Pool_(const std::shared_ptr<DALConnection> &pConnection)
   {
      {
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);

         if (!closing_)
         {
            available_connections_.insert(pConnection);

            // A caller waiting in GetConnection_ takes it now.
            connection_released_.notify_one();
            return true;
         }
      }

      // Made after the close before the process ends, which nothing will
      // close again: closed now, as one handed back after it is.
      CloseBeforeExit_(pConnection);
      return false;
   }

   DALConnection::ConnectionResult
   DatabaseConnectionManager::CheckConnectionMade_(String &sErrorMessage)
   {
      // Checked out as a statement's connection is - the first in the pool,
      // as it was, or another one free when a caller has taken that one.
      std::shared_ptr<DALConnection> pConnection = GetConnection_();
      if (!pConnection)
      {
         if (GetIsClosingBeforeExit())
            sErrorMessage = ClosedBeforeExitMessage;
         else
            sErrorMessage = "No database connection is available.";

         return DALConnection::FatalError;
      }

      const bool versionAccepted = pConnection->CheckServerVersion(sErrorMessage);
      if (versionAccepted)
         pConnection->OnConnected();

      // Back in the pool - or closed, when the close before the process ends
      // began meanwhile.
      ReleaseConnection_(pConnection);

      if (!versionAccepted)
         return DALConnection::FatalError;

      if (GetIsClosingBeforeExit())
      {
         sErrorMessage = ClosedBeforeExitMessage;
         return DALConnection::FatalError;
      }

      return DALConnection::Connected;
   }

   bool
   DatabaseConnectionManager::GetIsClosingBeforeExit()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);
      return closing_;
   }

   void
   DatabaseConnectionManager::Disconnect()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      //auto iterConnection = available_connections_.begin();
      for(std::shared_ptr<DALConnection> pConnection : available_connections_)
      {
         pConnection->Disconnect();
      }
      available_connections_.clear();

      for(std::shared_ptr<DALConnection> pConnection : busy_connections_)
      {
         pConnection->Disconnect();
      }

      busy_connections_.clear();
   }

   int
   DatabaseConnectionManager::CloseWhenIdle(int maxWaitSeconds, const std::function<void()> &progress)
   {
      boost::unique_lock<boost::recursive_mutex> guard(mutex_);

      closing_ = true;

      for (std::shared_ptr<DALConnection> pConnection : available_connections_)
         CloseBeforeExit_(pConnection);

      available_connections_.clear();

      // A caller waiting for a connection is answered now (GetConnection_).
      connection_released_.notify_all();

      // Those in use come back through ReleaseConnection_, which closes each.
      const boost::chrono::steady_clock::time_point deadline =
         boost::chrono::steady_clock::now() + boost::chrono::seconds(maxWaitSeconds > 0 ? maxWaitSeconds : 0);
      boost::chrono::steady_clock::time_point nextProgress = boost::chrono::steady_clock::now() + boost::chrono::seconds(1);

      while (!busy_connections_.empty())
      {
         const boost::chrono::steady_clock::time_point now = boost::chrono::steady_clock::now();
         if (now >= deadline)
            break;

         if (now >= nextProgress)
         {
            nextProgress = now + boost::chrono::seconds(1);

            // Told without the lock, which a caller handing a connection back
            // takes.
            if (progress)
            {
               guard.unlock();
               progress();
               guard.lock();
               continue;
            }
         }

         connection_released_.wait_until(guard, deadline < nextProgress ? deadline : nextProgress);
      }

      return (int) busy_connections_.size();
   }

   void
   DatabaseConnectionManager::CloseBeforeExit_(const std::shared_ptr<DALConnection> &pConnection)
   {
      try
      {
         pConnection->Disconnect();
      }
      catch (...)
      {
         // Left as it is, to end with the process as one still in use does;
         // said, since on SQL Server Compact what was committed since its last
         // write then waits for the flush interval, or is lost.
         LOG_APPLICATION("A database connection could not be closed before the process ends, and ends with the process as it is.");
      }
   }

   int
   DatabaseConnectionManager::SecondsToWaitForConnectionsInUse(DatabaseSettings::SQLDBType type)
   {
      // By value: the constant is declared in its class.
      const int flushIntervalSeconds = DatabaseSettings::SqlCeFlushIntervalSeconds;

      if (type == DatabaseSettings::TypeMSSQLCompactEdition)
         return flushIntervalSeconds + 2;

      return 0;
   }

   bool
   DatabaseConnectionManager::Execute(const SQLStatement &statement, __int64 *iInsertID, int iIgnoreErrors)
   {
      // The caller does not want the message. It is still produced and logged by
      // the layer below; this simply has nowhere to put it, which is what the
      // defaulted temporary did before.
      String discarded;
      return Execute(statement.GetCommand(), iInsertID, iIgnoreErrors, discarded);
   }

   bool
   DatabaseConnectionManager::Execute(const SQLCommand &command, __int64 *iInsertID, int iIgnoreErrors)
   {
      String discarded;
      return Execute(command, iInsertID, iIgnoreErrors, discarded);
   }

   bool
   DatabaseConnectionManager::Execute(const SQLStatement &statement, __int64 *iInsertID, int iIgnoreErrors, String &sErrorMessage)
   {
      return Execute(statement.GetCommand(), iInsertID, iIgnoreErrors, sErrorMessage);
   }

   bool
   DatabaseConnectionManager::Execute(const SQLCommand &command, __int64 *iInsertID, int iIgnoreErrors, String &sErrorMessage)
   {
      return Execute_(command, iInsertID, iIgnoreErrors, sErrorMessage, nullptr, nullptr);
   }

   __int64
   DatabaseConnectionManager::ExecuteCount(const SQLCommand &command, bool *reconnected, int iIgnoreErrors)
   {
      String discarded;
      __int64 rows = -1;

      if (!Execute_(command, nullptr, iIgnoreErrors, discarded, &rows, reconnected))
         return -1;

      return rows;
   }

   bool
   DatabaseConnectionManager::Execute_(const SQLCommand &command, __int64 *iInsertID, int iIgnoreErrors, String &sErrorMessage,
                                       __int64 *rowsAffected, bool *reconnected)
   {
      if (rowsAffected != nullptr)
         *rowsAffected = -1;

      if (reconnected != nullptr)
         *reconnected = false;

      // A statement made on a thread inside a DatabaseTransaction of this pool
      // belongs to that transaction: it runs on the transaction's connection, once,
      // and not on a connection of its own. This is what puts the persistence
      // layer's existing saves inside a transaction without each being rewritten
      // (DatabaseTransaction.h).
      DatabaseTransaction *transaction = DatabaseTransaction::Current_(this);
      if (transaction != nullptr && transaction->Routes_())
         return ExecuteInTransaction_(*transaction, command, iInsertID, iIgnoreErrors, sErrorMessage);

      if (SimulateFailureFor_(command))
      {
         sErrorMessage = "Simulated database failure ([Settings] SimulateDatabaseFailureFor).";
         return false;
      }

      std::shared_ptr<DALConnection> pDALConn = GetConnection_();

      if (!pDALConn)
      {
         // Acquisition can legitimately fail (pool exhausted past its deadline).
         // GetConnection_ has already reported it.
         LOG_DEBUG("Aborting statement since no database connection could be acquired.");
         return false;
      }

      boost::chrono::steady_clock::time_point queryStart = boost::chrono::steady_clock::now();

      bool bResult = pDALConn->Execute(command, sErrorMessage, iInsertID, iIgnoreErrors, rowsAffected, reconnected);

      unsigned __int64 elapsedMicros = (unsigned __int64) boost::chrono::duration_cast<boost::chrono::microseconds>(
         boost::chrono::steady_clock::now() - queryStart).count();
      MeasureQuery_(command, elapsedMicros);

      ReleaseConnection_(pDALConn);

      // The audit trail's one chokepoint for object changes. It is AFTER the
      // release on purpose: the row it may write is itself a statement through
      // this function, and taking a second connection while still holding the
      // first is how a pool of N deadlocks under N concurrent writers. It does
      // nothing at all unless an interface has installed an actor on this
      // thread, which the server's own writes never do - see AuditTrail.h.
      AuditTrail::Instance()->RecordStatement(command, bResult);

      return bResult;
   }

   bool
   DatabaseConnectionManager::ExecuteInTransaction_(DatabaseTransaction &transaction, const SQLCommand &command, __int64 *iInsertID, int iIgnoreErrors, String &sErrorMessage)
   {
      bool bResult = transaction.Execute_(command, iInsertID, iIgnoreErrors, sErrorMessage);

      // The audit trail's record, as in Execute. Its row is written through
      // Execute on this thread, so it joins the transaction, commits with the
      // change it records and is rolled back with it - and takes no second
      // connection from the pool while this one is held.
      AuditTrail::Instance()->RecordStatement(command, bResult);

      return bResult;
   }

   std::shared_ptr<DALRecordset>
   DatabaseConnectionManager::OpenRecordset(const SQLStatement &statement)
   {
      return OpenRecordset(statement.GetCommand());
   }

   /*
      Test-only fault injection. True when [Settings] SimulateDatabaseFailureFor is set
      and this statement contains it, in which case the caller is answered exactly as it
      would be by a database that refused the statement.

      Why it exists: three sweeps in August 2026 fixed 51 places where the result of a
      database operation was discarded, and every defect found was in error handling
      that had never once executed - a `return true` where the delete had failed, a
      String passed through a variadic %s in the line that reports a failed restore, a
      UID counter read back after an unchecked increment. Those 51 fixes added 51 more
      branches that also never execute. Without a way to run them, the fixes are
      untested code of exactly the kind that was just found to be wrong.

      Why a substring of the statement and not a mode: the server has delivery threads,
      scheduled tasks and protocol sessions all issuing statements at once, so "fail the
      next statement" would fail whatever happened to be next. Naming the statement -
      "update hm_imapfolders set foldercurrentuid" - fails precisely one write and
      leaves everything else working, which is the only thing that makes this usable
      against a running server.

      Why it is safe: empty out of the box, so the cost is one IsEmpty() per statement
      and the behaviour is unchanged. It cannot be set remotely or over COM - only by
      editing hMailServer.INI and reinitialising - and Application::Reinitialize reports
      HM6119 for as long as it is set, because the failure mode of a facility like this
      is being quietly left on, not being turned on. SimulateSpoolWriteFailure and
      CrashSimulationMode are the same idea for the disk and the crash handler.
   */
   bool
   DatabaseConnectionManager::SimulateFailureFor_(const SQLCommand &command)
   {
      // The bool first, and it is the whole reason this is affordable: this runs before
      // every statement the server issues, and the String getters in IniFileSettings
      // return by value, so asking for the pattern here would put a heap copy on the
      // path of every query to support a facility that is off on every real server.
      if (!IniFileSettings::Instance()->GetSimulateDatabaseFailureEnabled())
         return false;

      const String pattern = IniFileSettings::Instance()->GetSimulateDatabaseFailureFor();

      if (pattern.IsEmpty())
         return false;

      return command.GetQueryString().Find(pattern) >= 0;
   }

   std::shared_ptr<DALRecordset>
   DatabaseConnectionManager::OpenRecordset(const SQLCommand &command)
   {
      // As in Execute: a query made on a thread inside a DatabaseTransaction of
      // this pool runs on the transaction's connection, once, and reads what the
      // transaction has written.
      DatabaseTransaction *transaction = DatabaseTransaction::Current_(this);
      if (transaction != nullptr && transaction->Routes_())
         return transaction->OpenRecordset_(command);

      std::shared_ptr<DALRecordset> pRecordset;

      // Reads as well as writes: several of the defects this exists to test are on the
      // read side - ReadRecipients_ failing made a queued message look recipient-less,
      // and the delivery manager then deleted it.
      if (SimulateFailureFor_(command))
         return pRecordset;

      std::shared_ptr<DALConnection> pDALConn = GetConnection_();

      if (!pDALConn)
      {
         // Acquisition can legitimately fail (pool exhausted past its deadline).
         // GetConnection_ has already reported it.
         LOG_DEBUG("Aborting statement since no database connection could be acquired.");
         return pRecordset;
      }

      pRecordset = pDALConn->CreateRecordset();

      boost::chrono::steady_clock::time_point queryStart = boost::chrono::steady_clock::now();

      bool bOpened = pRecordset->Open(pDALConn, command);

      unsigned __int64 elapsedMicros = (unsigned __int64) boost::chrono::duration_cast<boost::chrono::microseconds>(
         boost::chrono::steady_clock::now() - queryStart).count();
      MeasureQuery_(command, elapsedMicros);

      if (!bOpened)
      {
         // A query that could not be run is also "the database did not answer",
         // not "the database answered with nothing" - a dropped connection or a
         // locked table reaches here rather than the acquisition failure above.
         DatabaseUnavailableMarker::Mark();

         pRecordset.reset();
      }

      ReleaseConnection_(pDALConn);

      return pRecordset;


   }

   void
   DatabaseConnectionManager::MeasureQuery_(const SQLCommand &command, unsigned __int64 microseconds)
   {
      int slowThresholdMs = IniFileSettings::Instance()->GetSlowQueryLogMilliseconds();
      bool wasSlow = slowThresholdMs > 0 && (microseconds / 1000) >= (unsigned __int64) slowThresholdMs;

      ServerStatus::Instance()->OnDatabaseQuery(microseconds, wasSlow);

      if (wasSlow)
      {
         String logLine;
         logLine.Format(_T("Slow database query (%I64u ms): %s"),
            microseconds / 1000, RedactSqlLiterals_(command.GetQueryString()).c_str());
         LOG_APPLICATION(logLine);
      }

      // Emit an OpenTelemetry span for the statement (redacted) when tracing is on.
      // Parents to the active protocol-command span on this thread when present.
      if (OtelTracer::Instance()->IsEnabled())
      {
         std::vector<OtelAttribute> attributes;

         OtelAttribute system;
         system.key = "db.system";
         switch (IniFileSettings::Instance()->GetDatabaseType())
         {
         case HM::DatabaseSettings::TypeMYSQLServer: system.value = "mysql"; break;
         case HM::DatabaseSettings::TypeMSSQLServer: system.value = "mssql"; break;
         case HM::DatabaseSettings::TypePGServer: system.value = "postgresql"; break;
         case HM::DatabaseSettings::TypeMSSQLCompactEdition: system.value = "sqlce"; break;
         default: system.value = "unknown"; break;
         }
         attributes.push_back(system);

         OtelAttribute statement;
         statement.key = "db.statement";
         statement.value = RedactSqlLiterals_(command.GetQueryString());
         attributes.push_back(statement);

         OtelTracer::Instance()->RecordCompletedSpan("db.query", OtelSpanKindClient, microseconds, attributes);
      }
   }

   String
   DatabaseConnectionManager::RedactSqlLiterals_(const String &sql)
   {
      // Replace the contents of every single-quoted string literal with a single
      // '?' so any secret inlined into the statement text is never logged. Handles
      // the SQL '' escaped quote and the MySQL \' backslash escape so a literal is
      // not closed prematurely. Over-masking on a malformed/unbalanced quote is
      // acceptable (it only ever removes more, never less).
      String result;
      int len = sql.GetLength();
      bool inString = false;
      bool maskedCurrent = false;

      for (int i = 0; i < len; i++)
      {
         TCHAR c = sql[i];

         if (!inString)
         {
            result += c;
            if (c == _T('\''))
            {
               inString = true;
               maskedCurrent = false;
            }
            continue;
         }

         // Inside a string literal.
         if (c == _T('\\') && i + 1 < len)
         {
            // Backslash escape (MySQL default): consume the escaped character too.
            i++;
            if (!maskedCurrent) { result += _T('?'); maskedCurrent = true; }
            continue;
         }

         if (c == _T('\''))
         {
            if (i + 1 < len && sql[i + 1] == _T('\''))
            {
               // Doubled '' escaped quote: still inside the literal.
               i++;
               if (!maskedCurrent) { result += _T('?'); maskedCurrent = true; }
               continue;
            }

            // Closing quote.
            if (!maskedCurrent)
               result += _T('?');
            result += _T('\'');
            inString = false;
            continue;
         }

         // Ordinary character inside the literal: emit one '?' for the whole run.
         if (!maskedCurrent) { result += _T('?'); maskedCurrent = true; }
      }

      return result;
   }

   void
   DatabaseConnectionManager::ReleaseConnection_(std::shared_ptr<DALConnection> pConnection)
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      auto iterConnection = busy_connections_.find(pConnection);
      if (iterConnection == busy_connections_.end())
      {
         HM_ASSERT(0);
         return;
      }

      busy_connections_.erase(iterConnection);

      if (closing_)
      {
         // The close before the process ends waits for this one (CloseWhenIdle):
         // closed now that nothing uses it, rather than under its statement.
         CloseBeforeExit_(pConnection);

         connection_released_.notify_all();
         return;
      }

      // Locate an available connection
      available_connections_.insert(pConnection);

      // Wake one waiter (if any) in GetConnection_ now that a connection is free.
      connection_released_.notify_one();
   }

   int 
   DatabaseConnectionManager::GetCurrentDatabaseVersion()
   {
      SQLCommand command("select * from hm_dbversion");
      std::shared_ptr<DALRecordset> pRS = OpenRecordset(command);
      if (!pRS)
         return 0;

      int iRetVal = pRS->GetLongValue("value");

      return iRetVal;
   }

   std::shared_ptr<DALConnection>
   DatabaseConnectionManager::GetConnection_()
   {
      boost::unique_lock<boost::recursive_mutex> guard(mutex_);

      // The deadline bounds the whole acquisition, not each individual wait.
      // Without it, a pool exhausted by one stalled subsystem blocks every
      // unrelated caller - message saving, greylisting, authentication - for as
      // long as the stall lasts. 0 keeps the unbounded wait for anyone who wants
      // it.
      const int acquire_timeout_seconds = IniFileSettings::Instance()->GetDBConnectionAcquireTimeout();
      const bool bounded_wait = acquire_timeout_seconds > 0;

      boost::chrono::steady_clock::time_point deadline;
      if (bounded_wait)
      {
         deadline = boost::chrono::steady_clock::now() + boost::chrono::seconds(acquire_timeout_seconds);
      }

      // Loop until we find a free connection (re-checking after each wakeup).
      while (1)
      {
         // The process is ending (CloseWhenIdle): nothing more is handed out, and
         // the caller is answered as the pool answers when it has no connection
         // at all, below - a statement after the close is not one that found
         // nothing.
         if (closing_)
         {
            DatabaseUnavailableMarker::Mark();

            std::shared_ptr<DALConnection> pEmpty;
            return pEmpty;
         }

         // Locate an available connection
         auto iterConnection = available_connections_.begin();

         if (iterConnection != available_connections_.end())
         {
            // Remove the connection from free and add to busy
            std::shared_ptr<DALConnection> pConn = (*iterConnection);

            // Remove it from the list of available connections
            available_connections_.erase(iterConnection);

            busy_connections_.insert(pConn);
            return pConn;
         }

         if (busy_connections_.size() == 0 &&
            available_connections_.size() == 0)
         {
            // There's no available connections at all. Nothing to wait for.
            //
            // Marked for the same reason the timeout below is: this returns the
            // empty result that a caller cannot tell from "the query found
            // nothing", and a pool holding no connections whatsoever is about as
            // unavailable as a database gets. Without the mark, a recipient
            // lookup during the window between Disconnect() and the next
            // successful Connect() answers "no such user" and the sender is told
            // permanently that a valid mailbox does not exist. No error is
            // reported here - the failure to connect has already been reported
            // by whoever tried.
            DatabaseUnavailableMarker::Mark();

            std::shared_ptr<DALConnection> pEmpty;
            return pEmpty;
         }

         // All connections are busy. Wait until one is released instead of
         // polling. A timeout bounds the wait as a defensive backstop so a lost
         // notification can never wedge the caller indefinitely.
         boost::chrono::milliseconds wait_time(100);

         if (bounded_wait)
         {
            boost::chrono::steady_clock::time_point now = boost::chrono::steady_clock::now();

            if (now >= deadline)
            {
               // Hand back the same empty result the pool produces when it has no
               // connections at all, so callers take their existing failure path.
               String message;
               message.Format(_T("Timed out after %d seconds while waiting for a database connection. All %d pooled connections are in use. Increase [Database] NumberOfConnections, or investigate what is holding connections open."),
                  acquire_timeout_seconds, (int) busy_connections_.size());

               // Released before reporting. ErrorManager::ReportError writes to the
               // error log and, when an OnError event script is installed, runs it
               // synchronously with full COM access - and those COM objects read the
               // database, re-entering this function on this thread. The mutex is
               // recursive so the re-entry is admitted, but the pool is empty (that
               // is why we are here), so the inner call would wait for a connection
               // while still holding the outer lock, blocking every thread trying to
               // return one.
               guard.unlock();

               // Marked before reporting, and before returning the empty result:
               // callers see the same "nothing came back" they would see from a
               // query that legitimately found nothing, and this is the only thing
               // that lets them tell the two apart. Without it a recipient lookup
               // that timed out would answer "no such user" and the sender would
               // get a permanent rejection for a valid address.
               DatabaseUnavailableMarker::Mark();

               ErrorManager::Instance()->ReportError(ErrorManager::High, 5180, "DatabaseConnectionManager::GetConnection_", message);

               std::shared_ptr<DALConnection> pEmpty;
               return pEmpty;
            }

            boost::chrono::milliseconds remaining =
               boost::chrono::duration_cast<boost::chrono::milliseconds>(deadline - now);

            if (remaining < wait_time)
               wait_time = remaining;
         }

         connection_released_.wait_for(guard, wait_time);
      }
   }

   /*
      What a transaction opened here does and does not cover. Measured against
      the code on 13 August 2026; every claim below is about this tree, not about
      what the backends are capable of.

      SCOPE. The connection is taken out of the pool and handed back to the
      caller, and it stays checked out until Commit or Rollback returns it. Only
      statements run *on that object* are inside the transaction: Execute() and
      OpenRecordset() on this class borrow a different pooled connection per
      statement, so anything routed through them during the window is outside it.
      The COM API uses it so: InterfaceDatabase keeps the connection in conn_ and
      sends ExecuteSQL / ExecuteSQLScript / EnsurePrerequisites straight to it
      (SchemaUpgrader begins its own transactions on the connection UpgradeSchema
      lends it). The server's own units of work use DatabaseTransaction instead,
      which holds its connection for the thread that opened it, so that Execute()
      and OpenRecordset() made on that thread run inside it, and which never runs
      a statement again after a reconnect.

      PER BACKEND:

      MS SQL Server (ADOConnection) - real. BEGIN/COMMIT/ROLLBACK TRANSACTION are
      issued as statements on the session, and MSSQL rolls back DDL as well as
      DML. Note that Connect() puts the session at READ UNCOMMITTED, so the
      transaction buys atomicity of its own writes and no read isolation
      whatsoever; a concurrent reader on another pooled connection sees the
      uncommitted rows.

      PostgreSQL (PGConnection) - real, and the strongest of the four: DDL is
      transactional, and the session is at the server default (READ COMMITTED)
      because nothing overrides it. A statement that fails inside the transaction
      aborts it: everything after it is refused until the rollback, and a COMMIT
      is answered with a rollback, which CommitTransaction reports as a failure.

      MySQL/MariaDB (MySQLConnection) - conditional. BEGIN is issued only if
      every table in the database reported InnoDB when the connection was opened
      (LoadSupportsTransactions_); on a MyISAM database BeginTransaction returns
      true having done nothing, and it is Rollback that finally says so, with
      error 5104 telling the administrator to restore a backup. DDL is never
      transactional on MySQL - it commits implicitly - so a multi-statement
      upgrade step that fails half way leaves a half-changed schema whatever this
      returns.

      SQL Server Compact (SQLCEConnection) - real, DDL included, and it is the
      installer default. See the comment on SQLCEConnection::BeginTransaction.

      So: a transaction here is worth having on MSSQL, PostgreSQL and SQL CE, is
      worth having on MySQL only for an all-InnoDB database, and is worth nothing
      for DDL on MySQL. Anything that wraps a cascade in one has to keep working
      when it silently buys nothing - and file deletions cannot be rolled back on
      any of them.
   */
   std::shared_ptr<DALConnection>
   DatabaseConnectionManager::BeginTransaction(String &sErrorMessage)
   {
      std::shared_ptr<DALConnection> pDALConnection = GetConnection_();

      // GetConnection_ returns empty when the pool could not hand one out.
      if (!pDALConnection)
      {
         sErrorMessage = "No database connection is available.";
         return pDALConnection;
      }

      if (!pDALConnection->BeginTransaction(sErrorMessage))
      {
         // Could not start the transaction. The connection must go back to the
         // pool - leaking it here permanently shrank the pool, and once it was
         // exhausted every SMTP, IMAP and POP3 operation blocked forever.
         ReleaseConnection_(pDALConnection);

         std::shared_ptr<DALConnection> pEmpty;
         return pEmpty;
      }

      return pDALConnection;
   }

   bool
   DatabaseConnectionManager::CommitTransaction(std::shared_ptr<DALConnection> pConnection, String &sErrorMessage)
   {
      bool bResult = pConnection->CommitTransaction(sErrorMessage);
      ReleaseConnection_(pConnection);

      return bResult;
   }

   bool
   DatabaseConnectionManager::RollbackTransaction(std::shared_ptr<DALConnection> pConnection, String &sErrorMessage)
   {
      bool bResult = pConnection->RollbackTransaction(sErrorMessage);
      ReleaseConnection_(pConnection);

      return bResult;
   }

   bool
   DatabaseConnectionManager::GetIsConnected()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      // Closing before the process ends: a connection still out is its
      // caller's, and nothing else runs.
      if (closing_)
         return false;

      size_t iNoOfConnections = busy_connections_.size() + available_connections_.size();

      if (iNoOfConnections == 0)
         return false;

      return true;
   }

   int
   DatabaseConnectionManager::GetBusyConnectionCount()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);
      return (int) busy_connections_.size();
   }

   int
   DatabaseConnectionManager::GetAvailableConnectionCount()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);
      return (int) available_connections_.size();
   }

   bool
   DatabaseConnectionManager::ExecuteScript(const String &sFile, String &sErrorMessage)
   {
      std::shared_ptr<DALConnection> pConnection = GetConnection_();

      // GetConnection_ returns empty when the pool could not hand one out.
      if (!pConnection)
      {
         sErrorMessage = "No database connection is available.";
         return false;
      }

      SQLScriptRunner scriptRunner;
      bool result = scriptRunner.ExecuteScript(pConnection, sFile, sErrorMessage);

      ReleaseConnection_(pConnection);

      return result;
   }

   SchemaUpgrader::Outcome
   DatabaseConnectionManager::UpgradeSchema(int required, const String &scriptDirectory, SchemaUpgrader::Report report)
   {
      std::shared_ptr<DALConnection> pConnection = GetConnection_();

      // GetConnection_ returns empty when the pool could not hand one out.
      if (!pConnection)
      {
         SchemaUpgrader::Outcome outcome;
         outcome.failed_step = "borrowing a connection";
         outcome.error = "No database connection is available.";
         return outcome;
      }

      SchemaUpgrader::Outcome outcome = SchemaUpgrader::Upgrade(pConnection, scriptDirectory, required, report);

      ReleaseConnection_(pConnection);

      return outcome;
   }

   bool
   DatabaseConnectionManager::EnsuresPrerequisites(long DBVersion, String &sErrorMessage)
   {
      PrerequisiteList prerequisites;

      std::shared_ptr<DALConnection> pConnection = GetConnection_();

      // GetConnection_ returns empty when the pool could not hand one out.
      if (!pConnection)
      {
         sErrorMessage = "No database connection is available.";
         return false;
      }

      bool result = prerequisites.Ensure(pConnection, DBVersion, sErrorMessage);

      ReleaseConnection_(pConnection);

      return result;
   }

   namespace
   {
      // A connection with no database behind it: it answers every call as one
      // that worked and records when it is closed.
      class StandInConnection : public DALConnection
      {
      public:
         StandInConnection() :
            DALConnection(std::make_shared<DatabaseSettings>(String(), String(), String(), String(), String(), String(), String(),
               DatabaseSettings::TypeUnknown, 0))
         {
         }

         ConnectionResult Connect(String &/*sErrorMessage*/) override { return Connected; }
         bool Disconnect() override { closed_.store(true); return true; }
         ExecutionResult TryExecute(const SQLCommand &/*command*/, String &/*sErrorMessage*/, __int64 * /*iInsertID*/, int /*iIgnoreErrors*/) override { return DALSuccess; }
         bool IsConnected() const override { return !closed_.load(); }
         void OnConnected() override { connected_open_.store(!closed_.load()); }
         bool BeginTransaction(String &/*sErrorMessage*/) override { return true; }
         bool CommitTransaction(String &/*sErrorMessage*/) override { return true; }
         bool RollbackTransaction(String &/*sErrorMessage*/) override { return true; }
         void SetTimeout(int /*seconds*/) override {}
         bool GetSupportsCommandParameters() const override { return true; }
         bool CheckServerVersion(String &/*errorMessage*/) override
         {
            if (during_check_)
               during_check_();

            return true;
         }
         void EscapeString(String &/*sInput*/) override {}
         std::shared_ptr<DALRecordset> CreateRecordset() override { return std::shared_ptr<DALRecordset>(); }
         std::shared_ptr<IMacroExpander> CreateMacroExpander() override { return std::shared_ptr<IMacroExpander>(); }

         bool IsClosed() const { return closed_.load(); }

         // Run while the server's version is asked, as the close before the
         // process ends may run on another thread meanwhile.
         void RunDuringTheCheck(const std::function<void()> &duringCheck) { during_check_ = duringCheck; }

         // OnConnected ran, on a connection that was still open.
         bool WasConnectedOpen() const { return connected_open_.load(); }

      private:
         std::atomic<bool> closed_{false};
         std::atomic<bool> connected_open_{false};
         std::function<void()> during_check_;
      };

      void ExpectOfTheClose(bool condition, const char *failure)
      {
         if (!condition)
            throw std::logic_error(std::string("DatabaseConnectionManagerTester: ") + failure);
      }
   }

   void
   DatabaseConnectionManagerTester::Test()
   {
      TestTheConnectionInUseIsClosedOnlyWhenHandedBack_();
      TestTheWaitEndsWithTheLastConnectionBack_();
      TestTheWaitForAConnectionInUse_();
      TestAConnectionMadeAfterTheCloseIsClosed_();
      TestTheChecksOnAConnectionMadeHoldIt_();
   }

   std::shared_ptr<DatabaseConnectionManager>
   DatabaseConnectionManagerTester::MakeManagerOfStandIns(int count, std::vector<std::shared_ptr<DALConnection>> &connections)
   {
      std::shared_ptr<DatabaseConnectionManager> manager = std::make_shared<DatabaseConnectionManager>();
      for (int i = 0; i < count; i++)
      {
         std::shared_ptr<DALConnection> connection = std::make_shared<StandInConnection>();
         connections.push_back(connection);
         manager->available_connections_.insert(connection);
      }

      return manager;
   }

   void
   DatabaseConnectionManagerTester::TestTheConnectionInUseIsClosedOnlyWhenHandedBack_()
   {
      DatabaseConnectionManager manager;
      std::vector<std::shared_ptr<StandInConnection>> connections;
      for (int i = 0; i < 3; i++)
      {
         connections.push_back(std::make_shared<StandInConnection>());
         manager.available_connections_.insert(connections.back());
      }

      // In use, as a transaction holds it - or a statement that has not ended.
      String error;
      std::shared_ptr<DALConnection> inUse = manager.BeginTransaction(error);
      ExpectOfTheClose(inUse != nullptr, "no connection was handed out before the close.");

      const int left = manager.CloseWhenIdle(0, std::function<void()>());

      int idleClosed = 0;
      bool inUseClosed = false;
      for (const std::shared_ptr<StandInConnection> &connection : connections)
      {
         if (connection == inUse)
            inUseClosed = connection->IsClosed();
         else if (connection->IsClosed())
            idleClosed++;
      }

      ExpectOfTheClose(left == 1, "the close did not answer that one connection was still in use.");
      ExpectOfTheClose(!inUseClosed, "the close closed the connection in use under its caller, as Disconnect does - until 10 October 2026 the stop before the process ends closed a statement's connection under it (PQfinish, mysql_close).");
      ExpectOfTheClose(idleClosed == 2, "the close left an idle connection open.");
      ExpectOfTheClose(!manager.GetIsConnected(), "the manager answered that it was connected once the close had begun.");

      // Nothing handed out from now on, and at once: with every connection
      // that is left in use, the pool would otherwise wait for one - on a
      // thread of its own, so that such a wait fails the test rather than
      // hangs it, and ends when the connection in use is handed back below.
      std::atomic<bool> returned(false);
      std::atomic<bool> handedOut(false);
      std::atomic<bool> marked(false);
      std::thread asker([&manager, &returned, &handedOut, &marked]()
      {
         DatabaseUnavailableMarker::Clear();
         String askError;
         std::shared_ptr<DALConnection> after = manager.BeginTransaction(askError);
         handedOut.store(after != nullptr);
         marked.store(DatabaseUnavailableMarker::IsMarked());
         DatabaseUnavailableMarker::Clear();
         if (after)
            manager.RollbackTransaction(after, askError);
         returned.store(true);
      });

      const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!returned.load() && std::chrono::steady_clock::now() < until)
         std::this_thread::sleep_for(std::chrono::milliseconds(10));
      const bool atOnce = returned.load();

      // Closed as its caller hands it back.
      manager.CommitTransaction(inUse, error);
      asker.join();

      ExpectOfTheClose(atOnce, "a connection asked for after the close had begun was waited for.");
      ExpectOfTheClose(!handedOut.load(), "a connection was handed out after the close had begun.");
      ExpectOfTheClose(marked.load(), "a connection refused after the close had begun was not marked as the database unavailable.");

      ExpectOfTheClose(std::static_pointer_cast<StandInConnection>(inUse)->IsClosed(), "the connection in use was not closed when its caller handed it back after the close.");
      ExpectOfTheClose(manager.GetBusyConnectionCount() == 0 && manager.GetAvailableConnectionCount() == 0, "a connection handed back after the close went back to the pool.");
   }

   void
   DatabaseConnectionManagerTester::TestTheWaitEndsWithTheLastConnectionBack_()
   {
      DatabaseConnectionManager manager;
      std::shared_ptr<StandInConnection> connection = std::make_shared<StandInConnection>();
      manager.available_connections_.insert(connection);

      String error;
      std::shared_ptr<DALConnection> inUse = manager.BeginTransaction(error);
      ExpectOfTheClose(inUse != nullptr, "no connection was handed out before the close.");

      // Handed back a moment into the wait, as a statement that ends.
      std::thread caller([&manager, inUse]()
      {
         std::this_thread::sleep_for(std::chrono::milliseconds(300));
         String commitError;
         manager.CommitTransaction(inUse, commitError);
      });

      const std::chrono::steady_clock::time_point before = std::chrono::steady_clock::now();
      const int left = manager.CloseWhenIdle(30, std::function<void()>());
      const std::chrono::steady_clock::duration took = std::chrono::steady_clock::now() - before;
      caller.join();

      ExpectOfTheClose(left == 0, "the close answered that a connection was still in use after its caller had handed it back.");
      ExpectOfTheClose(took < std::chrono::seconds(20), "the close waited on after the last connection in use had come back.");
      ExpectOfTheClose(connection->IsClosed(), "the connection handed back during the close's wait was not closed.");
   }

   void
   DatabaseConnectionManagerTester::TestTheWaitForAConnectionInUse_()
   {
      // SQL Server Compact's wait covers its flush interval, the servers' is none.
      const int flushIntervalSeconds = DatabaseSettings::SqlCeFlushIntervalSeconds;
      ExpectOfTheClose(DatabaseConnectionManager::SecondsToWaitForConnectionsInUse(DatabaseSettings::TypeMSSQLCompactEdition) > flushIntervalSeconds,
         "the close before the process ends does not wait out SQL Server Compact's flush interval for a connection in use, and the process's end would lose what was committed since its last write.");
      ExpectOfTheClose(DatabaseConnectionManager::SecondsToWaitForConnectionsInUse(DatabaseSettings::TypePGServer) == 0 &&
         DatabaseConnectionManager::SecondsToWaitForConnectionsInUse(DatabaseSettings::TypeMYSQLServer) == 0 &&
         DatabaseConnectionManager::SecondsToWaitForConnectionsInUse(DatabaseSettings::TypeMSSQLServer) == 0,
         "the close before the process ends waits for a connection in use on a database server, where nothing committed is lost.");

      DatabaseConnectionManager manager;
      std::shared_ptr<StandInConnection> connection = std::make_shared<StandInConnection>();
      manager.available_connections_.insert(connection);

      String error;
      std::shared_ptr<DALConnection> inUse = manager.BeginTransaction(error);
      ExpectOfTheClose(inUse != nullptr, "no connection was handed out before the close.");

      std::atomic<int> told(0);
      const std::chrono::steady_clock::time_point before = std::chrono::steady_clock::now();
      const int left = manager.CloseWhenIdle(2, [&told]() { told++; });
      const std::chrono::steady_clock::duration took = std::chrono::steady_clock::now() - before;

      ExpectOfTheClose(left == 1, "the close did not answer that the connection was still in use at the end of its wait.");
      ExpectOfTheClose(took >= std::chrono::milliseconds(1900), "the close did not wait its limit for the connection in use.");
      ExpectOfTheClose(told.load() >= 1, "the close's wait did not tell its progress, which the service controller is given.");
      ExpectOfTheClose(!connection->IsClosed(), "the close closed the connection in use when its wait ended.");

      manager.CommitTransaction(inUse, error);
      ExpectOfTheClose(connection->IsClosed(), "the connection in use was not closed when its caller handed it back after the close's wait.");
   }

   void
   DatabaseConnectionManagerTester::TestAConnectionMadeAfterTheCloseIsClosed_()
   {
      // A start or a reinitialise that the stop before the process ends gave
      // up waiting for, still making its connections.
      DatabaseConnectionManager manager;
      std::shared_ptr<StandInConnection> before = std::make_shared<StandInConnection>();
      ExpectOfTheClose(manager.Pool_(before), "a connection made before the close was not put in the pool.");
      ExpectOfTheClose(manager.GetAvailableConnectionCount() == 1 && !before->IsClosed(), "a connection made before the close was not left open in the pool.");
      ExpectOfTheClose(!manager.GetIsClosingBeforeExit(), "the manager answered that it was closing before the close had begun.");

      manager.CloseWhenIdle(0, std::function<void()>());
      ExpectOfTheClose(manager.GetIsClosingBeforeExit(), "the manager did not answer that it was closing once the close had begun.");

      std::shared_ptr<StandInConnection> after = std::make_shared<StandInConnection>();
      const bool pooled = manager.Pool_(after);

      ExpectOfTheClose(!pooled, "a connection made after the close was put in the pool - until 11 October 2026 it went in, without the pool's lock, and stayed open.");
      ExpectOfTheClose(after->IsClosed(), "a connection made after the close was left open.");
      ExpectOfTheClose(manager.GetAvailableConnectionCount() == 0 && manager.GetBusyConnectionCount() == 0, "a connection made after the close is still in the pool.");

      // The refusal marks this thread's statement as one the database did
      // not take, which is put back as it was.
      const bool markedBefore = DatabaseUnavailableMarker::IsMarked();
      String error;
      const DALConnection::ConnectionResult result = manager.CheckConnectionMade_(error);
      if (!markedBefore)
         DatabaseUnavailableMarker::Clear();

      ExpectOfTheClose(result == DALConnection::FatalError && !error.IsEmpty(),
         "the checks on the connections made answered that they had connected, with nothing said, after the close.");
   }

   void
   DatabaseConnectionManagerTester::TestTheChecksOnAConnectionMadeHoldIt_()
   {
      // As the start makes them, with nothing in the way: the connection goes
      // back to the pool.
      {
         DatabaseConnectionManager manager;
         std::shared_ptr<StandInConnection> connection = std::make_shared<StandInConnection>();
         manager.Pool_(connection);

         String error;
         const DALConnection::ConnectionResult result = manager.CheckConnectionMade_(error);

         ExpectOfTheClose(result == DALConnection::Connected, "the checks on a connection made did not answer that it had connected.");
         ExpectOfTheClose(connection->WasConnectedOpen(), "the checks on a connection made did not run OnConnected on it.");
         ExpectOfTheClose(manager.GetAvailableConnectionCount() == 1 && manager.GetBusyConnectionCount() == 0, "the connection the checks ran on did not go back to the pool.");
      }

      // The close before the process ends during the checks, as on another
      // thread: it finds the connection in use, leaves it open for the rest
      // of them, and it is closed once they end.
      DatabaseConnectionManager manager;
      std::shared_ptr<StandInConnection> connection = std::make_shared<StandInConnection>();
      manager.Pool_(connection);

      int leftInUse = -1;
      connection->RunDuringTheCheck([&manager, &leftInUse]()
      {
         leftInUse = manager.CloseWhenIdle(0, std::function<void()>());
      });

      String error;
      const DALConnection::ConnectionResult result = manager.CheckConnectionMade_(error);

      ExpectOfTheClose(leftInUse == 1, "the close during the checks on a connection made did not find it in use - until 11 October 2026 they ran on it in the pool.");
      ExpectOfTheClose(connection->WasConnectedOpen(), "the close during the checks on a connection made closed it under them.");
      ExpectOfTheClose(connection->IsClosed(), "the connection the checks ran on was not closed when they ended after the close.");
      ExpectOfTheClose(manager.GetAvailableConnectionCount() == 0 && manager.GetBusyConnectionCount() == 0, "the connection the checks ran on went back to the pool after the close.");
      ExpectOfTheClose(result == DALConnection::FatalError && !error.IsEmpty(), "the checks answered that the connection made had connected, with nothing said, after the close had begun during them.");
   }
}