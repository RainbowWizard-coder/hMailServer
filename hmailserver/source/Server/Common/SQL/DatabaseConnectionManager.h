// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DALConnection.h"
#include "DatabaseSettings.h"
#include "../Persistence/SchemaUpgrader.h"

#include <functional>
#include <memory>
#include <vector>

#include <boost/thread/recursive_mutex.hpp>
#include <boost/thread/condition_variable.hpp>

namespace HM
{
   class SQLCommand;
   class DatabaseTransaction;

   class DatabaseConnectionManager
   {
   public:
      DatabaseConnectionManager(void);
      ~DatabaseConnectionManager(void);

      bool CreateConnections(String &sErrorMessage);

      // A pool of its own over the database the settings name, with this many
      // connections and one attempt, for a tester that needs a pool beside the
      // server's - on a database of its own, or as a second node would be, since two
      // pools in one process race on the database as two nodes do (ActiveActive.md,
      // D18). The server's own pool is CreateConnections(String &), from the
      // configuration file.
      bool CreateConnections(std::shared_ptr<DatabaseSettings> settings, int connections, String &sErrorMessage);

      // Every connection closed, those in use included: a reinitialise's,
      // which opens the database again at once, and a command-line verb's.
      void Disconnect();

      // The close before the process ends (Application::ShutDown): the idle
      // connections closed now, each one in use as its caller hands it back,
      // and none handed out from now on - a statement asked for meanwhile is
      // answered as one with no connection to run on, and DatabaseUnavailable-
      // Marker says so. Waits maxWaitSeconds at most for those in use to come
      // back, calling progress about once a second while it waits, and answers
      // how many are still out. Until 10 October 2026 the stop before exit
      // closed the database with Disconnect, under a statement still running
      // on one of them (PQfinish, mysql_close).
      int CloseWhenIdle(int maxWaitSeconds, const std::function<void()> &progress);

      // How long CloseWhenIdle waits for a connection in use before the
      // process ends without it. SQL Server Compact: its flush interval and
      // two seconds more. It writes what was committed to its file at that
      // interval, and when any of its connections to the file closes - on
      // server1 on 11 October 2026 the close of a connection that had made no
      // statement of its own wrote what another, left open, had committed -
      // so with its one connection (IniFileSettings) still in use nothing is
      // closed, and only the interval writes it: the wait covers what was
      // committed before CloseWhenIdle began. A connection opened and closed
      // for the purpose would write the same at once; none is, since that
      // would open the file beside a statement still running on it, which was
      // not measured. The database servers: not at all, since what they took
      // is theirs, and a statement the process's end cuts off is theirs to
      // roll back.
      static int SecondsToWaitForConnectionsInUse(DatabaseSettings::SQLDBType type);

      // CloseWhenIdle has begun: no connection is handed out or pooled from
      // now on, and a start or a reinitialise still making them is answered
      // that the database was closed (Connect_), not that it could not be
      // reached (Application::OpenDatabase).
      bool GetIsClosingBeforeExit();

      // Four overloads rather than two with a defaulted reference. The default
      // used to be String(_T("")) - a temporary bound to a non-const reference,
      // which MSVC accepts as an extension and every conforming compiler
      // refuses. The pair without the parameter discards the message exactly as
      // the temporary did, and every existing call site still resolves.
      bool Execute(const SQLStatement &statement, __int64 *iInsertID = 0, int iIgnoreErrors = 0);
      bool Execute(const SQLStatement &statement, __int64 *iInsertID, int iIgnoreErrors, String &sErrorMessage);
      bool Execute(const SQLCommand &command, __int64 *iInsertID = 0, int iIgnoreErrors = 0);
      bool Execute(const SQLCommand &command, __int64 *iInsertID, int iIgnoreErrors, String &sErrorMessage);

      // The number of rows the statement matched, or -1 when it failed - for an
      // UPDATE, every row its WHERE clause matched, whether or not the new values
      // differ from the old, on all four backends (DALConnection::TryExecute) - so
      // that a conditional UPDATE can be judged by its count: won when it matched
      // one row (ActiveActive.md, decision D1). For one INSERT, UPDATE or DELETE;
      // what an engine answers for anything else is not to be read. iIgnoreErrors
      // DALConnection::DALErrorInSQL forgives a row the database's own rules
      // refused, and that only: a duplicate key on all four backends, and on SQL
      // Server also a NULL in a NOT NULL column and a foreign key's or a CHECK's
      // refusal, on SQL Server Compact also a foreign key's
      // (DALConnection::TryExecute). Forgiven, it counts 0, since it changed no
      // row; any other failure counts -1 and is reported as one, and a lost
      // connection is reconnected and the statement run again, as without it.
      //
      // The same statement as Execute, with the same fault injection, measuring and
      // audit, and the same error reported when it fails. Execute runs a statement
      // again after a reconnect (DALConnection::Execute); reconnected, when given,
      // says whether that happened while this one ran. A first run may then have
      // been committed with only its answer lost: an UPDATE that recognises itself
      // (a claim matching its own epoch, D1) counts 1 either way, and any other
      // caller that relies on the count reads its row back before it believes a 0
      // or a failure - an insert-or-claim's INSERT among them, whose second run
      // finds the row its first run wrote and is forgiven as a duplicate, 0.
      //
      // Made on a thread inside a DatabaseTransaction of this pool, it runs in that
      // transaction as Execute does, and the count is the transaction's one run of
      // it (DatabaseTransaction::Execute): a row the caller forgives as a duplicate
      // counts 0 there as here, and reconnected stays false, since a scope runs no
      // statement again - except on a MySQL database that is not transactional,
      // where the scope runs it as the pool does.
      __int64 ExecuteCount(const SQLCommand &command, bool *reconnected = nullptr, int iIgnoreErrors = 0);

      std::shared_ptr<DALRecordset> OpenRecordset(const SQLStatement &statement);
      std::shared_ptr<DALRecordset> OpenRecordset(const SQLCommand &command);

      int GetCurrentDatabaseVersion();

      bool GetIsConnected();

      // Connection-pool observability (used by the metrics endpoint). Busy =
      // currently checked out by a caller, available = idle in the pool.
      int GetBusyConnectionCount();
      int GetAvailableConnectionCount();

      std::shared_ptr<DALConnection> BeginTransaction(String &sErrorMessage);
      bool CommitTransaction(std::shared_ptr<DALConnection> pConnection, String &sErrorMessage);
      bool RollbackTransaction(std::shared_ptr<DALConnection> pConnection, String &sErrorMessage);
      bool ExecuteScript(const String &sFile, String &sErrorMessage);

      // The upgrade chain on one of the pool's connections, for the start-up
      // schema step ([Database] UpgradeOnStart): SchemaUpgrader::Upgrade with
      // a connection borrowed and returned here, since the walk needs one
      // connection for its transactions and the pool's members are private.
      SchemaUpgrader::Outcome UpgradeSchema(int required, const String &scriptDirectory, SchemaUpgrader::Report report);

      bool EnsuresPrerequisites(long DBVersion, String &sErrorMessage);
   private:

      friend class DatabaseConnectionManagerTester;

      DALConnection::ConnectionResult Connect_(std::shared_ptr<DatabaseSettings> settings, int connections, String &sErrorMessage);

      // The one path Execute and ExecuteCount share: the fault injection, a pooled
      // connection, the statement (DALConnection::Execute), its measuring, and the
      // audit trail's record of it.
      bool Execute_(const SQLCommand &command, __int64 *iInsertID, int iIgnoreErrors, String &sErrorMessage,
                    __int64 *rowsAffected, bool *reconnected);

      // A connection Connect_ has made, put in the pool under mutex_ - or,
      // once CloseWhenIdle has begun, closed instead, and false answered. A
      // start or a reinitialise that the stop before the process ends gave up
      // waiting for may still be making them (Application::ShutDown). Until
      // 11 October 2026 they went into the pool without the lock, while that
      // close read the pool under it and cleared it, and one made after the
      // close stayed open.
      bool Pool_(const std::shared_ptr<DALConnection> &pConnection);

      // Connect_'s checks on one of the connections it has made - the
      // server's version, and what the backend does once connected - with
      // that connection checked out as a statement's is: no other caller is
      // handed it meanwhile, and CloseWhenIdle waits for it rather than
      // closing it under them. FatalError, the connection closed, once
      // CloseWhenIdle has begun. Until 11 October 2026 they ran on the first
      // connection in the pool, read from it without the lock.
      DALConnection::ConnectionResult CheckConnectionMade_(String &sErrorMessage);

      std::shared_ptr<DALConnection> GetConnection_();
      void ReleaseConnection_(std::shared_ptr<DALConnection> pConn);

      // One connection closed for CloseWhenIdle, and said when it cannot be.
      static void CloseBeforeExit_(const std::shared_ptr<DALConnection> &pConnection);

      // Test-only fault injection; see the comment above the definition. Off unless
      // [Settings] SimulateDatabaseFailureFor names a substring of the statement.
      static bool SimulateFailureFor_(const SQLCommand &command);

      // Records a statement's execution latency into the observability metrics and,
      // when it exceeds [Settings] SlowQueryLogMilliseconds, logs it (literals
      // redacted). Backend-agnostic: called from the single Execute/OpenRecordset
      // chokepoint so every query is measured regardless of database engine.
      void MeasureQuery_(const SQLCommand &command, unsigned __int64 microseconds);

      // Returns the statement text with the contents of every single-quoted string
      // literal replaced by '?', so any value inlined into the SQL is never written
      // to the slow-query log.
      static String RedactSqlLiterals_(const String &sql);
 
      boost::recursive_mutex mutex_;
      
      std::set<std::shared_ptr<DALConnection> > busy_connections_;
      std::set<std::shared_ptr<DALConnection> > available_connections_;
      
      // Signalled whenever a connection is returned to the pool, so a waiter in
      // GetConnection_ can wake immediately instead of polling.
      boost::condition_variable_any connection_released_;

      // CloseWhenIdle has begun: nothing is handed out, and a connection
      // handed back is closed. Under mutex_; never cleared - a reinitialise
      // makes a new manager.
      bool closing_ = false;

      // A DatabaseTransaction takes its connection from the pool and gives it
      // back, measures its statements and runs the fault injection as the pool's
      // own statements are, and has its statements recorded by the audit trail
      // through ExecuteInTransaction_. Its tester fills a pool of its own with
      // connections to a database of its own.
      friend class DatabaseTransaction;
      friend class DatabaseTransactionTester;

      // A statement made inside a DatabaseTransaction - through it, or through
      // Execute on the thread that opened it - run on its connection, once (but on
      // a MySQL database that is not transactional as the pool runs it), and
      // recorded by the audit trail as every statement through Execute is: here,
      // in the one file the audit trail's chokepoint lives in
      // (build/check-audit-choke-point.py). rowsAffected and reconnected as
      // ExecuteCount says them, of the transaction's run of the statement.
      bool ExecuteInTransaction_(DatabaseTransaction &transaction, const SQLCommand &command, __int64 *iInsertID, int iIgnoreErrors, String &sErrorMessage,
                                 __int64 *rowsAffected = nullptr, bool *reconnected = nullptr);
   };

   // CloseWhenIdle, over stand-in connections that need no database: the idle
   // ones closed at once and the one in use only once it is handed back,
   // nothing handed out meanwhile, and the wait ended by the last one coming
   // back or by its limit. Runs wherever ClassTester runs, and in
   // business-object-tests: it touches no manager but its own.
   class DatabaseConnectionManagerTester
   {
   public:
      void Test();

      // A manager over count stand-in connections, all idle, for the stop
      // before the process ends (ServersLockTester): they are put in
      // connections too, each to be asked whether it has been closed
      // (DALConnection::IsConnected).
      static std::shared_ptr<DatabaseConnectionManager> MakeManagerOfStandIns(int count, std::vector<std::shared_ptr<DALConnection>> &connections);

   private:
      void TestTheConnectionInUseIsClosedOnlyWhenHandedBack_();
      void TestTheWaitEndsWithTheLastConnectionBack_();
      void TestTheWaitForAConnectionInUse_();
      void TestAConnectionMadeAfterTheCloseIsClosed_();
      void TestTheChecksOnAConnectionMadeHoldIt_();
   };
}