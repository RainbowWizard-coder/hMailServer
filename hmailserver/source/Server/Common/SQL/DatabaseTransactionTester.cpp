// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "DatabaseTransaction.h"

#include "DALConnection.h"
#include "DALConnectionFactory.h"
#include "DALRecordset.h"
#include "DatabaseConnectionManager.h"
#include "DatabaseSettings.h"
#include "DatabaseUnavailableMarker.h"
#include "SQLCommand.h"
#include "SQLScriptRunner.h"
#include "ScriptTransaction.h"

#include "../Application/IniFileSettings.h"
#include "../Util/FileUtilities.h"
#include "../Util/GUIDCreator.h"

#ifndef HM_PLATFORM_POSIX
#include "SQLCEConnection.h"
#include "../Util/PasswordGenerator.h"
#endif

#include "../Util/Strings/Formatter.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      void Check(bool condition, const String &what)
      {
         if (!condition)
            throw std::logic_error(std::string("DatabaseTransactionTester: ") + AnsiString(what).c_str());
      }

      // Two threads that meet once: each waits at Arrive until the other has come,
      // for at most fifteen seconds, so that a unit of work that never arrives
      // fails the check rather than hanging the tester.
      class Meeting
      {
      public:
         Meeting() : arrived_(0) {}

         bool Arrive()
         {
            std::unique_lock<std::mutex> lock(mutex_);
            arrived_++;
            condition_.notify_all();
            return condition_.wait_for(lock, std::chrono::seconds(15), [this] { return arrived_ >= 2; });
         }

      private:
         std::mutex mutex_;
         std::condition_variable condition_;
         int arrived_;
      };

      // The tables and the statements, on the database the settings name.
      class Probe
      {
      public:
         Probe(std::shared_ptr<DatabaseSettings> settings, DatabaseTransactionTester::Report report) :
            settings_(settings),
            report_(report)
         {
            type_ = settings_->GetType();
         }

         // The tester's tables removed and its connections closed: at the end, and
         // when a check failed, before the failure goes on (Test). Whatever is left
         // is the tester's own tables, which the next run drops first.
         void Finish()
         {
            DropTables();

            if (pool_)
               pool_->Disconnect();

            if (raw_)
               raw_->Disconnect();
         }

         // A connection of the tester's own, outside every pool: the tables are
         // made, read and emptied through it, and sessions are ended from it.
         std::shared_ptr<DALConnection> Connect(const String &what)
         {
            std::shared_ptr<DALConnection> connection = DALConnectionFactory::CreateConnection(settings_);
            String error;
            Check(connection && connection->Connect(error) == DALConnection::Connected,
               _T("the tester could not connect for ") + what + _T(": ") + error);
            return connection;
         }

         void Start()
         {
            raw_ = Connect(_T("its own statements"));

            DropTables();

            const String engine = type_ == DatabaseSettings::TypeMYSQLServer ? _T(" ENGINE=InnoDB") : _T("");
            Raw(_T("create table aatxnprobe (id int not null primary key, v int not null)") + engine);
            Raw(_T("create table aatxnprobeb (id int not null primary key, v int not null)") + engine);

            // A row of aatxnprobec refers to a row of aatxnprobe by its v: an insert
            // naming a row that is not there is refused by the foreign key, which on
            // SQL Server and Compact the provider gives the HRESULT of a duplicate
            // key (DB_E_INTEGRITYVIOLATION).
            Raw(_T("create table aatxnprobec (id int not null primary key, v int not null, ")
                _T("constraint fk_aatxnprobec_v foreign key (v) references aatxnprobe (id))") + engine);

            // The pool after the tables: a MySQL connection decides when it opens
            // whether it has transactions, from the tables there are then
            // (MySQLConnection::LoadSupportsTransactions_), and a database with no
            // table yet has none.
            pool_ = DatabaseTransactionTester::OpenPool(settings_, 4);
         }

         void DropTables()
         {
            if (!raw_)
               return;

            // TryExecute: nothing reported when there is nothing to drop. The table
            // with the foreign key first, before the table it refers to.
            String ignored;
            raw_->TryExecute(SQLCommand(_T("drop table aatxnprobec")), ignored, 0, 0);
            raw_->TryExecute(SQLCommand(_T("drop table aatxnprobe")), ignored, 0, 0);
            raw_->TryExecute(SQLCommand(_T("drop table aatxnprobeb")), ignored, 0, 0);
            raw_->TryExecute(SQLCommand(_T("drop table aatxnprobem")), ignored, 0, 0);
         }

         void Raw(const String &sql)
         {
            String error;
            Check(raw_->TryExecute(SQLCommand(sql), error, 0, 0) == DALConnection::DALSuccess,
               _T("the tester's own statement failed: ") + sql + _T(": ") + error);
         }

         // The row's value as committed, read on the tester's own connection, or -1
         // when there is no such row.
         int Value(const String &table, int id)
         {
            std::shared_ptr<DALRecordset> recordset = raw_->CreateRecordset();
            String error;
            Check(recordset->TryOpen(raw_, SQLCommand(Formatter::Format("select v from {0} where id = {1}", table, id)), error) == DALConnection::DALSuccess,
               _T("the tester could not read its table: ") + error);
            if (recordset->IsEOF())
               return -1;
            return (int) recordset->GetLongValue("v");
         }

         void Reset()
         {
            Raw(_T("delete from aatxnprobec"));
            Raw(_T("delete from aatxnprobe"));
            Raw(_T("delete from aatxnprobeb"));
            Raw(_T("insert into aatxnprobe (id, v) values (1, 0)"));
            Raw(_T("insert into aatxnprobeb (id, v) values (1, 0)"));
         }

         // The session the transaction's connection holds, ended as a database that
         // fails over or restarts ends it: on PostgreSQL, MySQL and SQL Server by the
         // database, from the tester's own connection (on SQL Server KILL needs
         // ALTER ANY CONNECTION); on SQL Server Compact, which is a file in this
         // process, by closing the connection under the transaction. Not by ADO's
         // Close on SQL Server: the provider keeps the session in its pool as it is,
         // a transaction left open still open and holding its locks (measured with
         // SQLOLEDB on SQL Server 2022, 13 October 2026).
         void EndSessionOf(const std::shared_ptr<DALConnection> &connection)
         {
            if (type_ == DatabaseSettings::TypeMSSQLCompactEdition)
            {
               connection->Disconnect();
               return;
            }

            const bool postgres = type_ == DatabaseSettings::TypePGServer;
            const bool mssql = type_ == DatabaseSettings::TypeMSSQLServer;

            std::shared_ptr<DALRecordset> recordset = connection->CreateRecordset();
            String error;
            const String own = postgres ? _T("select pg_backend_pid() as session") : mssql ? _T("select @@SPID as session") : _T("select connection_id() as session");
            Check(recordset->TryOpen(connection, SQLCommand(own), error) == DALConnection::DALSuccess && !recordset->IsEOF(),
               _T("the tester could not read the session it is to end: ") + error);
            const __int64 session = recordset->GetInt64Value("session");

            Raw(postgres ? Formatter::Format("select pg_terminate_backend({0})", session) : Formatter::Format("kill {0}", session));

            // Gone from the server's own list before the transaction's next
            // statement, so that it meets a session that has ended rather than one
            // that is ending.
            const String gone = postgres ? Formatter::Format("select count(*) as sessions from pg_stat_activity where pid = {0}", session)
                              : mssql    ? Formatter::Format("select count(*) as sessions from sys.dm_exec_sessions where session_id = {0}", session)
                                         : Formatter::Format("select count(*) as sessions from information_schema.processlist where id = {0}", session);
            for (int i = 0; i < 100; i++)
            {
               std::shared_ptr<DALRecordset> left = raw_->CreateRecordset();
               Check(left->TryOpen(raw_, SQLCommand(gone), error) == DALConnection::DALSuccess, _T("the tester could not look for the ended session: ") + error);
               if (left->GetLongValue("sessions") == 0)
                  return;
               std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            Check(false, _T("the session the tester ended was still there five seconds later"));
         }

         void Say(const String &line)
         {
            if (report_)
               report_(line);
         }

         std::shared_ptr<DatabaseSettings> settings_;
         DatabaseTransactionTester::Report report_;
         DatabaseSettings::SQLDBType type_;
         std::shared_ptr<DALConnection> raw_;
         std::shared_ptr<DatabaseConnectionManager> pool_;
      };

      SQLCommand Update(const String &table, int add)
      {
         return SQLCommand(Formatter::Format("update {0} set v = v + {1} where id = 1", table, add));
      }

      SQLCommand Insert(int id, int value)
      {
         return SQLCommand(Formatter::Format("insert into aatxnprobe (id, v) values ({0}, {1})", id, value));
      }

      // A row of aatxnprobec that refers to a row of aatxnprobe that is not there
      // (Reset leaves row 1 alone in aatxnprobe), which the foreign key refuses.
      SQLCommand Orphan(int id)
      {
         return SQLCommand(Formatter::Format("insert into aatxnprobec (id, v) values ({0}, 99)", id));
      }

      // A transaction the pool hands out (DatabaseConnectionManager::BeginTransaction),
      // held as the COM Database object holds its script's: by ScriptTransaction, the
      // code the Database object runs - InterfaceDatabase turns its answers into the
      // object's errors and does nothing more. Begun here; rolled back, and its
      // connection given back to the pool, if it is still open when it ends, so that
      // a check that fails half-way leaves no transaction holding the tester's rows,
      // which the tables' drop at the end would wait for.
      void BeginOn(ScriptTransaction &transaction, std::shared_ptr<DatabaseConnectionManager> pool)
      {
         String error;
         Check(transaction.Begin(pool, error), _T("the pool's BeginTransaction failed: ") + error);
      }

      // A statement of the script's, run as the Database object runs one
      // (Database.ExecuteSQL is ScriptTransaction::Execute).
      ScriptTransaction::Outcome Script(ScriptTransaction &transaction, const SQLCommand &command, String &error)
      {
         return transaction.Execute(command.GetQueryString(), nullptr, error);
      }

      const TCHAR *OutcomeName(ScriptTransaction::Outcome outcome)
      {
         switch (outcome)
         {
         case ScriptTransaction::Ran:
            return _T("run");
         case ScriptTransaction::Failed:
            return _T("failed");
         default:
            return _T("refused");
         }
      }

      // Whether the Database object's error begins with what it says first: what
      // became of the statement, or of the commit.
      bool Says(const String &error, const String &start)
      {
         return error.Find(start) == 0;
      }
   }

   void
   DatabaseTransactionTester::Fail_(const String &what)
   {
      Check(false, what);
   }

   std::shared_ptr<DatabaseConnectionManager>
   DatabaseTransactionTester::OpenPool(std::shared_ptr<DatabaseSettings> settings, int connections)
   {
      std::shared_ptr<DatabaseConnectionManager> pool(new DatabaseConnectionManager);

      for (int i = 0; i < connections; i++)
      {
         std::shared_ptr<DALConnection> connection = DALConnectionFactory::CreateConnection(settings);
         String error;
         Check(connection && connection->Connect(error) == DALConnection::Connected, _T("the tester could not connect a pool's connection: ") + error);

         pool->available_connections_.insert(connection);
      }

      return pool;
   }

   void
   DatabaseTransactionTester::Test(std::shared_ptr<DatabaseSettings> settings, Report report, bool provokeErrors)
   {
      // The thread's mark of a database that did not answer, which several checks
      // set on purpose, cleared when they end: inside the server this is a COM
      // call's thread, which serves other calls after it.
      DatabaseUnavailableMarker::Scope unavailable;

      Probe probe(settings, report);

      // Every check, between the probe's Start and its Finish, which removes the
      // tester's tables and closes its connections whether the checks passed or
      // not; a failure of the checks is the one said, and Finish's otherwise.
      auto checks = [&]()
      {
         std::shared_ptr<DatabaseConnectionManager> pool = probe.pool_;
         const bool postgres = probe.type_ == DatabaseSettings::TypePGServer;
         const bool mysql = probe.type_ == DatabaseSettings::TypeMYSQLServer;

         // A rollback discards; the manager's own Execute, made on the transaction's
         // thread, runs in the transaction and is discarded with it; a statement made
         // on another thread meanwhile, or under a Suspension, is not in it.
         {
            probe.Reset();

            std::exception_ptr otherThread;
            {
               DatabaseTransaction transaction(pool);
               Check(transaction.IsOpen() && !transaction.IsJoined() && transaction.IsTransactional(), _T("a transaction did not open on the tester's pool"));
               Check(transaction.Execute(Insert(2, 20)), _T("an insert through the transaction failed"));
               Check(pool->Execute(Update(_T("aatxnprobe"), 5)), _T("an update through the manager inside the transaction failed"));

               // What the transaction wrote, read through the manager on its thread:
               // on its connection, so the uncommitted row is there.
               std::shared_ptr<DALRecordset> inside = pool->OpenRecordset(SQLCommand(_T("select v from aatxnprobe where id = 2")));
               Check(inside && !inside->IsEOF() && inside->GetLongValue("v") == 20, _T("a query through the manager inside the transaction did not read the transaction's own uncommitted row"));

               std::thread other([&]()
               {
                  try
                  {
                     Check(pool->Execute(Update(_T("aatxnprobeb"), 7)), _T("an update through the manager on another thread failed"));
                  }
                  catch (...)
                  {
                     otherThread = std::current_exception();
                  }
               });
               other.join();

               {
                  DatabaseTransaction::Suspension outside;
                  Check(pool->Execute(SQLCommand(_T("insert into aatxnprobeb (id, v) values (2, 2)"))), _T("an insert under a Suspension failed"));
               }

               // No Commit: the scope rolls back when it ends.
            }

            if (otherThread)
               std::rethrow_exception(otherThread);

            Check(probe.Value(_T("aatxnprobe"), 2) == -1, _T("a row inserted through a transaction that was not committed is there"));
            Check(probe.Value(_T("aatxnprobe"), 1) == 0, _T("an update made through DatabaseConnectionManager::Execute on the transaction's thread was not rolled back with it: it ran on a connection of its own, as on 74aed9ab8"));
            Check(probe.Value(_T("aatxnprobeb"), 1) == 7, _T("an update made on another thread during the transaction was rolled back with it"));
            Check(probe.Value(_T("aatxnprobeb"), 2) == 2, _T("a statement made under a Suspension was rolled back with the transaction (the control: it runs as every statement ran on 74aed9ab8)"));
            probe.Say(_T("a rollback discards, the manager's Execute on the thread joins, another thread's and a Suspension's do not"));
         }

         // The row counts inside a scope: the manager's ExecuteCount made on the
         // transaction's thread runs in the transaction, as its Execute does, and
         // counts the transaction's one run of the statement; the transaction's own
         // Execute counts the same way. The control is the rollback: an update
         // ExecuteCount ran on a connection of its own would have committed there.
         {
            probe.Reset();
            {
               DatabaseTransaction transaction(pool);
               Check(transaction.IsOpen(), _T("a transaction did not open on the tester's pool"));

               bool reconnected = true;
               const __int64 counted = pool->ExecuteCount(Update(_T("aatxnprobeb"), 3), &reconnected);
               Check(counted == 1 && !reconnected,
                  Formatter::Format("an update of one row through the manager's ExecuteCount inside a transaction counted {0}, not 1, or said it had reconnected ({1})", counted, reconnected ? 1 : 0));

               const __int64 none = pool->ExecuteCount(SQLCommand(_T("update aatxnprobeb set v = v + 1 where id = 99")));
               Check(none == 0, Formatter::Format("an update of no row through the manager's ExecuteCount inside a transaction counted {0}, not 0", none));

               __int64 own = -2;
               const bool updated = transaction.Execute(Update(_T("aatxnprobe"), 4), 0, 0, &own);
               Check(updated && own == 1, Formatter::Format("an update of one row through the transaction counted {0}, not 1", own));

               // No Commit: the scope rolls back when it ends.
            }
            Check(probe.Value(_T("aatxnprobeb"), 1) == 0,
               _T("an update made through the manager's ExecuteCount on the transaction's thread was not rolled back with it: it ran on a connection of its own"));
            Check(probe.Value(_T("aatxnprobe"), 1) == 0, _T("an update made through the transaction was not rolled back with it"));
            probe.Say(_T("ExecuteCount on the transaction's thread runs in it and counts its run, 1 for a row and 0 for none, as the transaction's Execute counts"));
         }

         // A commit commits what the transaction and the manager wrote; a scope
         // opened inside joins, and its Commit commits nothing of its own; a joined
         // scope that ends without its Commit takes the whole transaction back.
         {
            probe.Reset();
            {
               DatabaseTransaction outer(pool);
               Check(outer.Execute(Update(_T("aatxnprobe"), 1)), _T("the outer scope's update failed"));
               {
                  DatabaseTransaction inner(pool);
                  Check(inner.IsJoined() && inner.IsOpen(), _T("a scope opened inside another of the same pool did not join it"));
                  Check(pool->Execute(Update(_T("aatxnprobe"), 10)), _T("the joined scope's update failed"));
                  Check(inner.Commit(), _T("a joined scope's Commit failed"));
               }
               Check(outer.Commit(), _T("the outer scope did not commit"));
            }
            Check(probe.Value(_T("aatxnprobe"), 1) == 11, Formatter::Format("a committed transaction with a joined scope left {0}, not 11", probe.Value(_T("aatxnprobe"), 1)));

            probe.Reset();
            {
               DatabaseTransaction outer(pool);
               Check(outer.Execute(Update(_T("aatxnprobe"), 1)), _T("the outer scope's update failed"));
               {
                  DatabaseTransaction inner(pool);
                  Check(inner.Execute(Update(_T("aatxnprobe"), 10)), _T("the joined scope's update failed"));
               }
               Check(!outer.Commit() && outer.GetFailure() == DatabaseTransaction::FailureAbandoned,
                  _T("a transaction was committed after a scope joined to it ended without its Commit"));
            }
            Check(probe.Value(_T("aatxnprobe"), 1) == 0, _T("a transaction a joined scope abandoned left its writes"));

            // Run inside Run joins, and is run once.
            probe.Reset();
            int innerRuns = 0;
            DatabaseTransaction::RunResult result = DatabaseTransaction::Run(pool, [&](DatabaseTransaction &)
            {
               if (!pool->Execute(Update(_T("aatxnprobe"), 1)))
                  return false;

               DatabaseTransaction::RunResult inner = DatabaseTransaction::Run(pool, [&](DatabaseTransaction &joined)
               {
                  innerRuns++;
                  return joined.IsJoined() && joined.Execute(Update(_T("aatxnprobe"), 10));
               });

               return inner.outcome == DatabaseTransaction::Committed;
            });
            Check(result.outcome == DatabaseTransaction::Committed && result.attempts == 1 && innerRuns == 1,
               _T("a unit of work run inside another did not join it and commit with it once"));
            Check(probe.Value(_T("aatxnprobe"), 1) == 11, _T("a unit of work run inside another did not commit with it"));

            probe.Say(_T("a commit commits; a scope inside joins, its Commit commits nothing, and abandoned it takes the whole back"));
         }

         // The first failure fails the scope: what comes after it is not run, and the
         // commit is refused. Provoked by the fault the tester injects (Fail_, the
         // path every failure takes) and, where errors may be reported, by a
         // statement the database refuses.
         {
            probe.Reset();
            {
               DatabaseTransaction transaction(pool);
               Check(transaction.Execute(Update(_T("aatxnprobe"), 1)), _T("the first update failed"));
               transaction.root_->Fail_(DatabaseTransaction::FailureStatement, _T("the tester's failure"));
               Check(!transaction.IsOpen(), _T("a failed transaction says it is open"));
               Check(!pool->Execute(Update(_T("aatxnprobe"), 10)), _T("a statement after the transaction failed was run"));
               Check(!pool->OpenRecordset(SQLCommand(_T("select v from aatxnprobe where id = 1"))), _T("a query after the transaction failed was run"));
               Check(!transaction.Commit() && transaction.GetFailure() == DatabaseTransaction::FailureStatement, _T("a failed transaction was committed"));
            }
            Check(probe.Value(_T("aatxnprobe"), 1) == 0, _T("a write made before the transaction failed was committed"));

            if (provokeErrors)
            {
               probe.Reset();
               {
                  DatabaseTransaction transaction(pool);
                  Check(transaction.Execute(Update(_T("aatxnprobe"), 1)), _T("the first update failed"));
                  Check(!transaction.Execute(SQLCommand(_T("insert into aatxnprobe_none (id) values (1)"))), _T("an insert into a table that does not exist succeeded"));
                  Check(transaction.GetFailure() == DatabaseTransaction::FailureStatement, _T("a statement the database refused was not taken as one"));
                  Check(!transaction.Execute(Update(_T("aatxnprobe"), 10)) && !transaction.Commit(), _T("a statement or a commit was run after a statement the database refused"));
               }
               Check(probe.Value(_T("aatxnprobe"), 1) == 0, _T("a write made before a statement the database refused was committed"));
            }

            // A pool with no connection to give: the scope does not begin and says
            // why, and Run does not run the unit again, since the pool has waited
            // for a connection already (DBConnectionAcquireTimeout) or holds none.
            // As a lost connection, which it was taken for, it was tried five
            // times, a pool's wait each time.
            {
               std::shared_ptr<DatabaseConnectionManager> none(new DatabaseConnectionManager);
               {
                  DatabaseTransaction transaction(none);
                  Check(!transaction.IsOpen() && transaction.GetFailure() == DatabaseTransaction::FailureUnavailable,
                     _T("a scope on a pool with no connection to give did not say that it had none"));
               }

               int calls = 0;
               DatabaseTransaction::RunResult result = DatabaseTransaction::Run(none, [&](DatabaseTransaction &)
               {
                  calls++;
                  return true;
               });
               Check(result.outcome == DatabaseTransaction::Failed && result.attempts == 1 && calls == 0,
                  Formatter::Format("Run on a pool with no connection to give made {0} attempts, where the pool has waited for one already", result.attempts));
            }

            probe.Say(_T("the first failure fails the transaction: nothing after it is run, and nothing is committed; a pool with no connection to give is not tried again"));
         }

         // On a pool of one connection - SQL Server Compact's always is - a scope
         // holds the only connection there is. A scope that failed gives it back at
         // the failure, before the failure is reported, so that what then runs
         // outside the transaction under a Suspension (ErrorManager's OnError
         // script) finds it; and a statement made outside an open scope by the
         // scope's own thread, which could only wait for the connection that
         // thread holds, is refused at once (HM8503) rather than after
         // DBConnectionAcquireTimeout - where the server's other threads waited
         // with it, measured on the bench's Compact database.
         {
            probe.Reset();
            std::shared_ptr<DatabaseConnectionManager> one = OpenPool(settings, 1);
            {
               DatabaseTransaction transaction(one);
               Check(transaction.IsOpen() && transaction.Execute(Update(_T("aatxnprobe"), 1)), _T("a transaction on a pool of one connection did not open, or its update failed"));
               transaction.root_->Fail_(DatabaseTransaction::FailureStatement, _T("the tester's failure"));

               DatabaseTransaction::Suspension outside;
               const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
               const bool ran = one->Execute(Update(_T("aatxnprobeb"), 5));
               const long long waited = (long long) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
               Check(ran, Formatter::Format("a statement under a Suspension, made after a transaction on a pool of one connection had failed, found no connection after {0} ms: the failed scope kept the only one", (__int64) waited));
            }
            Check(probe.Value(_T("aatxnprobe"), 1) == 0 && probe.Value(_T("aatxnprobeb"), 1) == 5,
               _T("on a pool of one connection, the failed transaction's update stayed, or the statement made outside it did not"));

            if (provokeErrors)
            {
               DatabaseTransaction transaction(one);
               Check(transaction.IsOpen(), _T("a transaction on a pool of one connection did not open"));
               {
                  DatabaseTransaction::Suspension outside;
                  const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
                  const bool ran = one->Execute(Update(_T("aatxnprobeb"), 5));
                  const long long waited = (long long) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
                  Check(!ran, _T("a statement under a Suspension ran on a pool of one connection whose only connection an open transaction of the same thread held"));
                  Check(waited < 10000, Formatter::Format("a statement under a Suspension waited {0} ms for a connection only its own thread's open transaction could give back", (__int64) waited));
               }
               Check(transaction.Commit(), _T("the open transaction did not commit after a statement outside it was refused"));
            }

            one->Disconnect();
            probe.Say(_T("on a pool of one connection, a failed transaction gives it back before the failure is reported, and a statement outside an open one on its own thread is refused at once"));
         }

         // On a pool of two connections, one held by an open scope and the other by
         // a transaction of another thread, a statement the scope's own thread makes
         // outside it (under a Suspension) waits for the connection the other thread
         // gives back, and runs. The refusal above is for a pool whose every
         // connection lent out is held by the thread's own transactions, which only
         // that thread could give back; one that refused whenever the thread held a
         // scope and no connection was free would refuse here, and an OnError script
         // or a second transaction with it on a busy server engine, where the pool
         // waits. The wait it measures says that no connection was free when the
         // statement was made, so that the guard was asked.
         {
            probe.Reset();
            std::shared_ptr<DatabaseConnectionManager> two = OpenPool(settings, 2);
            {
               DatabaseTransaction transaction(two);
               Check(transaction.IsOpen() && transaction.Execute(Update(_T("aatxnprobe"), 1)), _T("a transaction on a pool of two connections did not open, or its update failed"));

               std::atomic<bool> holding(false);
               std::exception_ptr otherFailed;
               std::thread other([&]()
               {
                  try
                  {
                     DatabaseTransaction held(two);
                     Check(held.IsOpen(), _T("another thread's transaction on a pool of two connections did not open"));
                     holding = true;
                     std::this_thread::sleep_for(std::chrono::milliseconds(1200));

                     // Ended without a commit: its connection goes back to the pool.
                  }
                  catch (...)
                  {
                     otherFailed = std::current_exception();
                     holding = true;
                  }
               });

               bool ran = false;
               long long waited = 0;
               std::exception_ptr outsideFailed;
               try
               {
                  while (!holding)
                     std::this_thread::sleep_for(std::chrono::milliseconds(5));

                  DatabaseTransaction::Suspension outside;
                  const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
                  ran = two->Execute(Update(_T("aatxnprobeb"), 5));
                  waited = (long long) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
               }
               catch (...)
               {
                  // Joined first: a thread destroyed while it can still be joined
                  // ends the process.
                  outsideFailed = std::current_exception();
               }

               other.join();
               if (otherFailed)
                  std::rethrow_exception(otherFailed);
               if (outsideFailed)
                  std::rethrow_exception(outsideFailed);

               Check(ran, Formatter::Format("a statement under a Suspension, made while an open transaction of its thread held one connection of a pool of two and another thread's the other, was refused after {0} ms where it was to wait for the other thread's", (__int64) waited));
               Check(waited >= 300, Formatter::Format("a statement under a Suspension on a pool of two connections both held found one free after {0} ms, so the check did not see it wait", (__int64) waited));
               Check(transaction.Commit(), _T("the open transaction on a pool of two connections did not commit after a statement outside it waited and ran"));
            }
            Check(probe.Value(_T("aatxnprobe"), 1) == 1 && probe.Value(_T("aatxnprobeb"), 1) == 5,
               _T("on a pool of two connections, the transaction's update or the statement made outside it while it was open did not stay"));

            two->Disconnect();
            probe.Say(_T("on a pool of two connections, a statement outside an open transaction on its own thread waits for the connection another thread gives back, and runs"));
         }

         // A row that is there already, with DALErrorInSQL, is not a failure and
         // leaves the transaction as it was - on PostgreSQL by a savepoint, without
         // which the failed insert aborts the transaction and the COMMIT rolls back.
         {
            probe.Reset();
            {
               DatabaseTransaction transaction(pool);
               Check(transaction.Execute(Update(_T("aatxnprobe"), 6)), _T("the first update failed"));
               __int64 insertId = 99;
               Check(transaction.Execute(Insert(1, 50), &insertId, DALConnection::DALErrorInSQL) && insertId == 0,
                  _T("an insert of a row that is there, with DALErrorInSQL, was a failure, or gave an insert id"));
               Check(transaction.IsOpen(), _T("an ignored duplicate failed the transaction"));
               Check(transaction.Execute(Update(_T("aatxnprobe"), 1)), _T("a statement after an ignored duplicate failed"));
               Check(transaction.Commit(), _T("a transaction with an ignored duplicate did not commit"));
            }
            Check(probe.Value(_T("aatxnprobe"), 1) == 7, Formatter::Format("a transaction with an ignored duplicate left {0}, not 7", probe.Value(_T("aatxnprobe"), 1)));

            if (postgres)
            {
               // The control, through a transaction the pool hands out, which has no
               // savepoint: PostgreSQL rolls the transaction back, and the COMMIT is
               // answered with success and the tag ROLLBACK. CommitTransaction now
               // says so; on 74aed9ab8 it said committed.
               probe.Reset();
               String error;
               std::shared_ptr<DALConnection> connection = pool->BeginTransaction(error);
               Check(connection != nullptr, _T("the pool's BeginTransaction failed: ") + error);
               Check(connection->TryExecute(Update(_T("aatxnprobe"), 6), error, 0, 0) == DALConnection::DALSuccess, _T("the control's update failed"));
               Check(connection->TryExecute(Insert(1, 50), error, 0, DALConnection::DALErrorInSQL) == DALConnection::DALSuccess, _T("the control's duplicate was not ignored"));
               Check(connection->IsTransactionFailed(), _T("PostgreSQL did not abort the transaction at the ignored duplicate"));
               Check(!pool->CommitTransaction(connection, error), _T("a transaction PostgreSQL rolled back at its COMMIT was reported as committed"));
               Check(probe.Value(_T("aatxnprobe"), 1) == 0, _T("the control's transaction was committed"));
            }

            probe.Say(_T("a row that is there already, ignored, leaves the transaction to commit"));
         }

         // A row that is there already is the one failure DALErrorInSQL may hide. On
         // SQL Server and SQL Server Compact the provider gives an insert a foreign
         // key refused, and on SQL Server a NULL into a NOT NULL column, the HRESULT
         // of a duplicate key (DB_E_INTEGRITYVIOLATION), which those backends call
         // DALErrorInSQL; a unit of work that went on past such an insert would
         // commit without the row the database refused. First what the connection
         // says each failure was, on the tester's own connection outside any
         // transaction, where nothing is reported; then, where errors may be
         // reported, the scope itself with DALErrorInSQL.
         {
            probe.Reset();

            auto classified = [&](const SQLCommand &command) -> DALConnection::LastFailure
            {
               probe.raw_->ForgetLastFailure();
               String error;
               const DALConnection::ExecutionResult result = probe.raw_->TryExecute(command, error, 0, 0);
               Check(result != DALConnection::DALSuccess, _T("a statement the database was to refuse succeeded: ") + command.GetQueryString());
               return probe.raw_->ClassifyLastFailure(result);
            };

            const DALConnection::LastFailure duplicate = classified(Insert(1, 50));
            Check(duplicate == DALConnection::LastFailureDuplicate,
               Formatter::Format("an insert of a row whose primary key is there already was classified {0}, not as a row that is there already ({1})",
                  (int) duplicate, (int) DALConnection::LastFailureDuplicate));

            const DALConnection::LastFailure orphan = classified(Orphan(1));
            Check(orphan == DALConnection::LastFailureOther,
               Formatter::Format("an insert a foreign key refused was classified {0}, not as a failed statement ({1}): with DALErrorInSQL a unit of work would go on and commit without the row",
                  (int) orphan, (int) DALConnection::LastFailureOther));

            const DALConnection::LastFailure null = classified(SQLCommand(_T("insert into aatxnprobe (id, v) values (5, null)")));
            Check(null == DALConnection::LastFailureOther,
               Formatter::Format("an insert of a NULL into a NOT NULL column was classified {0}, not as a failed statement ({1})",
                  (int) null, (int) DALConnection::LastFailureOther));

            Check(probe.Value(_T("aatxnprobec"), 1) == -1 && probe.Value(_T("aatxnprobe"), 5) == -1, _T("a row the database refused is there"));

            if (provokeErrors)
            {
               probe.Reset();
               {
                  DatabaseTransaction transaction(pool);
                  Check(transaction.Execute(Update(_T("aatxnprobe"), 1)), _T("the first update failed"));
                  __int64 insertId = 99;
                  Check(!transaction.Execute(Orphan(2), &insertId, DALConnection::DALErrorInSQL),
                     _T("an insert a foreign key refused, made with DALErrorInSQL, was taken for a row that is there already"));
                  Check(transaction.GetFailure() == DatabaseTransaction::FailureStatement, _T("an insert a foreign key refused did not fail the transaction as a statement the database refused"));
                  Check(!transaction.Execute(Update(_T("aatxnprobe"), 10)) && !transaction.Commit(), _T("a statement or a commit was run after an insert a foreign key refused"));
               }
               Check(probe.Value(_T("aatxnprobe"), 1) == 0 && probe.Value(_T("aatxnprobec"), 2) == -1,
                  _T("a unit of work with an insert a foreign key refused committed part of itself"));
            }

            probe.Say(_T("an insert a foreign key refused and a NULL into a NOT NULL column are failures, not rows that are there already, and DALErrorInSQL does not hide them"));
         }

         // A statement marked [IGNORE-ERRORS], whose failure the backend answers as a
         // success: the scope goes on only while the transaction does. Here a value
         // that is not a number into an int column. SQL Server rolls the whole
         // transaction back at that conversion error (245), so there the scope
         // fails and nothing of the unit is committed; PostgreSQL goes back to the
         // savepoint the scope set, MySQL, MariaDB and SQL Server Compact take back
         // the statement alone (or MySQL outside strict mode stores 0), and the unit
         // commits whole. All of it or none of it: where the scope went on after
         // SQL Server had rolled back, the update after the statement committed on
         // its own and the COMMIT was refused.
         {
            probe.Reset();
            bool committed = false;
            DatabaseTransaction::Failure failure = DatabaseTransaction::FailureNone;
            {
               DatabaseTransaction transaction(pool);
               Check(transaction.Execute(Update(_T("aatxnprobe"), 1)), _T("the first update failed"));
               const bool past = transaction.Execute(SQLCommand(_T("insert into aatxnprobe (id, v) values (6, 'not a number') /* [IGNORE-ERRORS] */")));
               committed = past && transaction.Execute(Update(_T("aatxnprobe"), 10)) && transaction.Commit();
               failure = transaction.GetFailure();
            }

            const int value = probe.Value(_T("aatxnprobe"), 1);
            if (committed)
            {
               Check(value == 11, Formatter::Format("a unit of work that went on past a statement marked [IGNORE-ERRORS] committed and left {0}, not 11", value));
               probe.Say(_T("a statement marked [IGNORE-ERRORS] that failed: the transaction went on, and the unit committed whole"));
            }
            else
            {
               Check(value == 0 && failure == DatabaseTransaction::FailureStatement,
                  Formatter::Format("a unit of work ended at a statement marked [IGNORE-ERRORS] left {0}, not 0, and failed as {1}: part of it was committed on its own after the database had ended its transaction",
                     value, (int) failure));
               probe.Say(_T("a statement marked [IGNORE-ERRORS] that failed: the database ended the transaction, and the scope failed with nothing committed"));
            }

            // And a marked statement whose failure leaves the transaction open on
            // every engine - a row that is there already, which is what the marker
            // is for - after which the unit commits whole, wherever it is run. The
            // conversion error above cannot tell a scope that reads whether the
            // transaction is still open from one that takes every swallowed failure
            // for its end, since that error does end it on SQL Server: there the
            // scope goes on because ADOConnection::IsTransactionFailed reads
            // @@TRANCOUNT 1 after the duplicate (2627), and one that answered
            // "ended" would roll back every unit whose marked statement found its
            // row in place. PostgreSQL takes the failure back to the savepoint;
            // MySQL, MariaDB and SQL Server Compact take back the statement alone.
            probe.Reset();
            committed = false;
            failure = DatabaseTransaction::FailureNone;
            String error;
            {
               DatabaseTransaction transaction(pool);
               committed = transaction.Execute(Update(_T("aatxnprobe"), 1)) &&
                           transaction.Execute(SQLCommand(_T("insert into aatxnprobe (id, v) values (1, 50) /* [IGNORE-ERRORS] */"))) &&
                           transaction.Execute(Update(_T("aatxnprobe"), 10)) &&
                           transaction.Commit();
               failure = transaction.GetFailure();
               error = transaction.GetErrorMessage();
            }

            const int whole = probe.Value(_T("aatxnprobe"), 1);
            Check(committed && whole == 11,
               Formatter::Format("a unit of work whose statement marked [IGNORE-ERRORS] met a row that is there already, which leaves the transaction open, was not committed whole: it left {0}, not 11, and failed as {1}: {2}",
                  whole, (int) failure, error));
            probe.Say(_T("a statement marked [IGNORE-ERRORS] that met a row there already: the transaction went on, and the unit committed whole"));
         }

         // Two transactions on one row: the second waits for the first's commit, and
         // both writes are kept. Without transactions (the control) the update is
         // committed as it runs, and the second does not wait.
         {
            for (int pass = 0; pass < 2; pass++)
            {
               const bool scoped = pass == 0;
               probe.Reset();

               std::atomic<bool> holding(false);
               std::exception_ptr firstFailed;

               std::thread first([&]()
               {
                  try
                  {
                     if (scoped)
                     {
                        DatabaseTransaction transaction(pool);
                        Check(transaction.Execute(Update(_T("aatxnprobe"), 1)), _T("the first transaction's update failed"));
                        holding = true;
                        std::this_thread::sleep_for(std::chrono::milliseconds(800));
                        Check(transaction.Commit(), _T("the first transaction did not commit"));
                     }
                     else
                     {
                        Check(pool->Execute(Update(_T("aatxnprobe"), 1)), _T("the control's first update failed"));
                        holding = true;
                        std::this_thread::sleep_for(std::chrono::milliseconds(800));
                     }
                  }
                  catch (...)
                  {
                     holding = true;
                     firstFailed = std::current_exception();
                  }
               });

               while (!holding)
                  std::this_thread::sleep_for(std::chrono::milliseconds(5));

               std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
               bool written = false;
               std::exception_ptr secondFailed;
               try
               {
                  if (scoped)
                  {
                     DatabaseTransaction transaction(pool);
                     written = transaction.Execute(Update(_T("aatxnprobe"), 10)) && transaction.Commit();
                  }
                  else
                  {
                     written = pool->Execute(Update(_T("aatxnprobe"), 10));
                  }
               }
               catch (...)
               {
                  // Joined first: a thread destroyed while it can still be joined
                  // ends the process.
                  secondFailed = std::current_exception();
               }
               const long long waited = (long long) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

               first.join();
               if (firstFailed)
                  std::rethrow_exception(firstFailed);
               if (secondFailed)
                  std::rethrow_exception(secondFailed);

               Check(written, _T("the second update failed"));
               Check(probe.Value(_T("aatxnprobe"), 1) == 11, _T("two updates of one row did not both stay"));

               if (scoped)
                  Check(waited >= 500, Formatter::Format("a transaction's update of a row another transaction had changed did not wait for its commit (it took {0} ms)", (__int64) waited));
               else
                  Check(waited < 500, Formatter::Format("the control's update waited {0} ms for an update that had been committed", (__int64) waited));
            }

            probe.Say(_T("two transactions on one row wait for each other, where statements on their own do not"));
         }

         // Two units of work that take two rows in opposite orders: one is chosen as
         // the deadlock's victim (on SQL Server Compact, which detects none, the lock
         // wait times out). Run runs it again, and both commit once each. With the
         // same units in scopes of their own (the control) the victim's unit is lost.
         {
            for (int pass = 0; pass < 2; pass++)
            {
               const bool run = pass == 0;
               probe.Reset();

               Meeting meeting;
               DatabaseTransaction::RunResult results[2];
               bool failedRetryable[2] = { false, false };
               int calls[2] = { 0, 0 };
               std::exception_ptr failures[2];

               auto unit = [&](int which) -> bool
               {
                  // Unit 0 takes aatxnprobe then aatxnprobeb, adding 1; unit 1 the
                  // other way round, adding 10.
                  const String firstTable = which == 0 ? _T("aatxnprobe") : _T("aatxnprobeb");
                  const String secondTable = which == 0 ? _T("aatxnprobeb") : _T("aatxnprobe");
                  const int add = which == 0 ? 1 : 10;

                  if (!run)
                  {
                     DatabaseTransaction transaction(pool);
                     bool done = transaction.Execute(Update(firstTable, add));
                     meeting.Arrive();
                     done = done && transaction.Execute(Update(secondTable, add)) && transaction.Commit();
                     failedRetryable[which] = !done && transaction.GetFailure() == DatabaseTransaction::FailureRetryable;
                     return done;
                  }

                  results[which] = DatabaseTransaction::Run(pool, [&](DatabaseTransaction &transaction)
                  {
                     calls[which]++;
                     if (!transaction.Execute(Update(firstTable, add)))
                        return false;
                     if (calls[which] == 1)
                        meeting.Arrive();
                     return transaction.Execute(Update(secondTable, add));
                  });

                  return results[which].outcome == DatabaseTransaction::Committed;
               };

               bool done[2] = { false, false };
               std::thread other([&]()
               {
                  try
                  {
                     done[1] = unit(1);
                  }
                  catch (...)
                  {
                     failures[1] = std::current_exception();
                  }
               });

               try
               {
                  done[0] = unit(0);
               }
               catch (...)
               {
                  failures[0] = std::current_exception();
               }

               other.join();

               for (int i = 0; i < 2; i++)
               {
                  if (failures[i])
                     std::rethrow_exception(failures[i]);
               }

               const int a = probe.Value(_T("aatxnprobe"), 1);
               const int b = probe.Value(_T("aatxnprobeb"), 1);

               if (run)
               {
                  Check(done[0] && done[1], Formatter::Format("two units of work that met in a deadlock were not both committed by Run: outcomes {0} and {1}, after {2} and {3} attempts: {4}",
                     (int) results[0].outcome, (int) results[1].outcome, results[0].attempts, results[1].attempts, results[0].error + _T(" / ") + results[1].error));
                  Check(results[0].attempts + results[1].attempts >= 3, _T("neither unit of work met the deadlock, so nothing was retried"));
                  Check(a == 11 && b == 11, Formatter::Format("two units of work Run retried left {0} and {1}, not 11 and 11", a, b));
               }
               else
               {
                  Check(!(done[0] && done[1]), _T("the control's two units of work both committed, so they never met in a deadlock"));
                  Check(failedRetryable[0] || failedRetryable[1], _T("the deadlock's victim did not fail as a deadlock, a serialisation failure or a lock wait"));
                  const int expected = (done[0] ? 1 : 0) + (done[1] ? 10 : 0);
                  Check(a == expected && b == expected, Formatter::Format("the control left {0} and {1} where its committed units make {2}", a, b, expected));
               }
            }

            probe.Say(_T("a deadlock between two units of work: Run runs the victim again and both commit once, where scopes alone lose it"));
         }

         // A transaction the pool hands out (DatabaseConnectionManager::BeginTransaction),
         // held by ScriptTransaction - the code the COM Database object runs its script's
         // transaction with - whose statement a deadlock chooses as its victim. MySQL,
         // MariaDB and SQL Server roll the whole transaction back there and go on in
         // autocommit, PostgreSQL refuses everything until the rollback, and SQL Server
         // Compact, which detects no deadlock, times the lock wait out and leaves the
         // transaction as it was. Once the database has ended the transaction the
         // script's next statement is refused - by ScriptTransaction, and on PostgreSQL
         // by the database - and its commit fails with nothing of the transaction
         // committed, each saying so. On 6e1a336c5 the Database object sent the next
         // statement, which MySQL and MariaDB committed on its own, and the commit - a
         // COMMIT with nothing open - reported success. The control: the same statement
         // sent past the refusal is committed on its own on MySQL, MariaDB and SQL
         // Server. Then the victim a statement marked [IGNORE-ERRORS], sent as
         // Database.ExecuteSQL sends one; a script and the prerequisites asked for
         // after the deadlock (Database.ExecuteSQLScript and
         // Database.EnsurePrerequisites), each refused as a statement is; a script
         // run in such a transaction (Database.ExecuteSQLScript,
         // ScriptTransaction::ExecuteScript), whose statement marked [IGNORE-ERRORS] is
         // the victim, and, on MySQL and MariaDB, the other failures that end the
         // transaction there: MariaDB's 1020 and, where innodb_rollback_on_timeout is on,
         // a lock wait that timed out - and, where it is off, the same lock wait, which
         // takes back its own statement alone. Last, a second Begin while a transaction
         // is open (Database.BeginTransaction called twice).
         {
            const bool compact = probe.type_ == DatabaseSettings::TypeMSSQLCompactEdition;
            const bool mssql = probe.type_ == DatabaseSettings::TypeMSSQLServer;

            // The script's transaction holds row 1 of aatxnprobe, which it updates,
            // and row 2, which it inserts. Another transaction, made the heavier by
            // forty rows of aatxnprobeb so that InnoDB and SQL Server choose the
            // script's as the victim, holds row 1 of aatxnprobeb. ask, on a thread of
            // its own, makes the script ask for that row, and waits; the other
            // transaction then asks for the script's row 1. On PostgreSQL the script
            // waits first, so that its deadlock_timeout runs out first and its
            // transaction is the one aborted. On SQL Server Compact both lock waits
            // time out. The other transaction is rolled back before this returns, and
            // what ask answered is returned.
            //
            // PostgreSQL looks for a deadlock once, when a waiter's deadlock_timeout
            // runs out (1 s by default), and not again: the script's transaction is the
            // victim only when the other is waiting for it by then. On a busy machine
            // the other may start waiting later, its own look then finds the cycle,
            // and the other is aborted (2 runs in 11 at a load of about 40 on 32
            // threads, 16 October 2026). So, where the tester's user may set it (a
            // superuser, as the CI's and the lab's are), the script's deadlock_timeout
            // is made 3 s and the other's 10 s, inside each transaction: the other never
            // looks first, and has 3 s to start waiting. And on every engine a deadlock
            // whose victim was the other transaction is set up again, the script's
            // transaction rolled back and begun again on the pool and the rows put back,
            // three times in all before the case is said not to have been made.
            auto deadlock = [&](ScriptTransaction &script, const std::function<ScriptTransaction::Outcome()> &ask) -> ScriptTransaction::Outcome
            {
               String error;

               bool timeoutsSet = false;
               if (postgres)
               {
                  std::shared_ptr<DALRecordset> role = probe.raw_->CreateRecordset();
                  Check(role->TryOpen(probe.raw_, SQLCommand(_T("select case when current_setting('is_superuser') = 'on' then 1 else 0 end as superuser")), error) == DALConnection::DALSuccess && !role->IsEOF(),
                     _T("the tester could not read whether its user is a superuser: ") + error);
                  timeoutsSet = role->GetLongValue("superuser") == 1;
               }

               for (int attempt = 1; ; attempt++)
               {
                  if (attempt > 1)
                  {
                     String ignored;
                     script.Rollback(ignored);
                     probe.Reset();
                     BeginOn(script, pool);
                  }

                  if (timeoutsSet)
                     Check(script.GetConnection()->TryExecute(SQLCommand(_T("set local deadlock_timeout = '3s'")), error, 0, 0) == DALConnection::DALSuccess, _T("the tester could not set the script's deadlock_timeout: ") + error);

                  Check(Script(script, Update(_T("aatxnprobe"), 1), error) == ScriptTransaction::Ran, _T("the script's first update failed: ") + error);
                  Check(Script(script, Insert(2, 20), error) == ScriptTransaction::Ran, _T("the script's insert failed: ") + error);

                  __int64 session = 0;
                  if (postgres)
                  {
                     std::shared_ptr<DALRecordset> own = script.GetConnection()->CreateRecordset();
                     Check(own->TryOpen(script.GetConnection(), SQLCommand(_T("select pg_backend_pid() as session")), error) == DALConnection::DALSuccess && !own->IsEOF(),
                        _T("the tester could not read the script's session: ") + error);
                     session = own->GetInt64Value("session");
                  }

                  ScriptTransaction other;
                  BeginOn(other, pool);
                  if (timeoutsSet)
                     Check(other.GetConnection()->TryExecute(SQLCommand(_T("set local deadlock_timeout = '10s'")), error, 0, 0) == DALConnection::DALSuccess, _T("the tester could not set the other transaction's deadlock_timeout: ") + error);
                  for (int id = 100; id < 140; id++)
                  {
                     Check(other.GetConnection()->TryExecute(SQLCommand(Formatter::Format("insert into aatxnprobeb (id, v) values ({0}, 0)", id)), error, 0, 0) == DALConnection::DALSuccess,
                        _T("the other transaction's insert failed: ") + error);
                  }
                  Check(other.GetConnection()->TryExecute(Update(_T("aatxnprobeb"), 5), error, 0, 0) == DALConnection::DALSuccess, _T("the other transaction's update failed: ") + error);

                  ScriptTransaction::Outcome answer = ScriptTransaction::Ran;
                  std::exception_ptr askFailed;
                  std::thread asker([&]()
                  {
                     try
                     {
                        answer = ask();
                     }
                     catch (...)
                     {
                        askFailed = std::current_exception();
                     }
                  });

                  bool otherRan = false;
                  std::exception_ptr otherFailed;
                  try
                  {
                     if (postgres)
                     {
                        bool waiting = false;
                        for (int i = 0; i < 250 && !waiting; i++)
                        {
                           std::shared_ptr<DALRecordset> waits = probe.raw_->CreateRecordset();
                           Check(waits->TryOpen(probe.raw_, SQLCommand(Formatter::Format("select count(*) as waiting from pg_stat_activity where pid = {0} and wait_event_type = 'Lock'", session)), error) == DALConnection::DALSuccess,
                              _T("the tester could not see whether the script waits: ") + error);
                           waiting = waits->GetLongValue("waiting") > 0;
                           if (!waiting)
                              std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        }
                        Check(waiting, _T("the script's statement did not wait for the other transaction's row within five seconds"));
                     }
                     else
                     {
                        std::this_thread::sleep_for(std::chrono::milliseconds(300));
                     }

                     String otherError;
                     otherRan = other.GetConnection()->TryExecute(Update(_T("aatxnprobe"), 100), otherError, 0, 0) == DALConnection::DALSuccess;
                  }
                  catch (...)
                  {
                     // Joined first: a thread destroyed while it can still be joined ends
                     // the process. The other transaction is rolled back below, which ends
                     // the script's wait.
                     otherFailed = std::current_exception();
                  }

                  String rollbackError;
                  if (otherFailed)
                     other.Rollback(rollbackError);

                  asker.join();
                  if (other.IsOpen())
                     other.Rollback(rollbackError);

                  if (askFailed)
                     std::rethrow_exception(askFailed);
                  if (otherFailed)
                     std::rethrow_exception(otherFailed);

                  // Where a deadlock is detected, the script's transaction was the victim
                  // when the other's update could go on: the database rolled the script's
                  // back and freed its row 1.
                  if (compact || otherRan)
                     return answer;

                  Check(attempt < 3, _T("the deadlock chose the other transaction as its victim, not the script's, three times, so this check could not be made"));
                  probe.Say(Formatter::Format("a deadlock chose the other transaction as its victim, not the script's, at attempt {0}: set up again", attempt));
               }
            };

            // Each case below is a check of its own: a failure is kept and the next case
            // run, and the failures are said together at the end, so that one run says
            // which of them the code fails.
            std::vector<std::string> failed;
            auto run = [&](const std::function<void()> &check)
            {
               try
               {
                  check();
               }
               catch (const std::exception &failure)
               {
                  failed.push_back(failure.what());
               }
            };

            // The script goes on past the deadlock: its next statement, then its commit,
            // and what the Database object tells the script at each.
            run([&]()
            {
               probe.Reset();
               ScriptTransaction script;
               BeginOn(script, pool);
               String victimError;
               const ScriptTransaction::Outcome victim = deadlock(script, [&]() -> ScriptTransaction::Outcome
               {
                  return Script(script, Update(_T("aatxnprobeb"), 10), victimError);
               });

               const bool ended = script.GetConnection()->IsTransactionFailed();
               String nextError;
               const ScriptTransaction::Outcome next = Script(script, Insert(3, 30), nextError);

               // What another session sees before the transaction ends. Not on SQL
               // Server Compact, where a read waits for the rows the transaction holds.
               const int seen = compact ? -1 : probe.Value(_T("aatxnprobe"), 3);

               String commitError;
               const bool committed = script.Commit(commitError);

               const int one = probe.Value(_T("aatxnprobe"), 1);
               const int two = probe.Value(_T("aatxnprobe"), 2);
               const int three = probe.Value(_T("aatxnprobe"), 3);
               const int held = probe.Value(_T("aatxnprobeb"), 1);

               Check(victim == ScriptTransaction::Failed, Formatter::Format("the script's statement that met the deadlock was {0}, not failed", OutcomeName(victim)));

               if (compact)
               {
                  // The lock wait timed out: its statement alone was taken back, and the
                  // transaction went on and committed - as before.
                  Check(!ended && next == ScriptTransaction::Ran && committed,
                     Formatter::Format("on SQL Server Compact a transaction whose lock wait timed out was said to have ended ({0}), or its next statement was {1}, or it was not committed: {2}",
                        ended ? 1 : 0, OutcomeName(next), commitError));
                  Check(one == 1 && two == 20 && three == 30 && held == 0,
                     Formatter::Format("on SQL Server Compact a transaction whose lock wait timed out left {0}, {1}, {2} and {3}, not 1, 20, 30 and 0", one, two, three, held));
                  probe.Say(_T("SQL Server Compact: a lock wait that timed out took back its own statement, and the transaction went on and committed"));
               }
               else
               {
                  Check(seen == -1 && three == -1,
                     Formatter::Format("a statement the script ran after the database had rolled its transaction back at a deadlock was saved on its own, as on 6e1a336c5 on MySQL and MariaDB: {0} before the transaction's end and {1} after it, where nothing was to be saved",
                        seen, three));
                  Check(one == 0 && two == -1 && held == 0,
                     Formatter::Format("the transaction the database rolled back at a deadlock left rows 1 and 2 at {0} and {1}, and the other's row at {2}, not 0, none and 0", one, two, held));
                  Check(ended, _T("a transaction the database had rolled back at a deadlock was not said to have ended (DALConnection::IsTransactionFailed), as on 6e1a336c5 on MySQL and MariaDB"));
                  Check(!committed, _T("the commit of a transaction the database had rolled back at a deadlock reported success, as on 6e1a336c5 on MySQL and MariaDB"));

                  if (postgres)
                  {
                     // Refused by the database itself, whose transaction is aborted and
                     // open, and the commit refused by PGConnection.
                     Check(next == ScriptTransaction::Failed,
                        Formatter::Format("on PostgreSQL the script's statement after the deadlock was {0}, not failed: the database refuses it itself", OutcomeName(next)));
                     Check(Says(commitError, _T("Postgres: the transaction was rolled back, not committed")),
                        _T("on PostgreSQL the commit after the deadlock was not refused as one the database had aborted: ") + commitError);
                  }
                  else
                  {
                     // Refused by the Database object (ScriptTransaction), which says why,
                     // as it said at the statement that met the deadlock what to do.
                     Check(victimError.Find(_T("The database has ended the transaction and rolled back what it held")) > 0,
                        _T("the error of the script's statement that met the deadlock did not say that the database had ended the transaction: ") + victimError);
                     Check(next == ScriptTransaction::Refused,
                        Formatter::Format("the script's statement after the deadlock was {0}, not refused", OutcomeName(next)));
                     Check(Says(nextError, _T("The statement was not run: the database ended this transaction at an earlier statement in it that failed")),
                        _T("the refusal of the script's statement after the deadlock did not say why: ") + nextError);
                     Check(Says(commitError, _T("The transaction was rolled back, not committed: the database ended it at a statement in it that failed")),
                        _T("the commit after the deadlock was not refused as the Database object refuses it: ") + commitError);
                  }

                  probe.Say(_T("a deadlock that ended a transaction the pool handed out: the script's next statement refused, the commit failed, nothing saved"));
               }
            });

            // The control: the same statement sent past the refusal is committed on its
            // own, and the rollback does not take it back. Not on PostgreSQL, which
            // refuses it itself, nor on SQL Server Compact, whose transaction goes on.
            // The rollback of the ended transaction is answered with success, as the
            // Database object answers it (SQL Server refuses a ROLLBACK with nothing
            // open, 3903).
            if (!compact && !postgres)
            {
               run([&]()
               {
                  probe.Reset();
                  ScriptTransaction script;
                  BeginOn(script, pool);
                  deadlock(script, [&]() -> ScriptTransaction::Outcome
                  {
                     String error;
                     return Script(script, Update(_T("aatxnprobeb"), 10), error);
                  });

                  String error;
                  const bool sent = script.GetConnection()->TryExecute(Insert(4, 40), error, 0, 0) == DALConnection::DALSuccess;
                  const int seen = probe.Value(_T("aatxnprobe"), 4);

                  String rollbackError;
                  const bool rolledBack = script.Rollback(rollbackError);

                  Check(sent && seen == 40 && probe.Value(_T("aatxnprobe"), 4) == 40,
                     _T("the control: a statement sent past the refusal after the deadlock was not committed on its own, so the check above cannot tell the refusal from a database that keeps the transaction"));
                  Check(rolledBack, _T("the rollback of a transaction a deadlock had ended was refused: ") + rollbackError);
                  probe.Say(_T("the control: a statement sent past the refusal after a deadlock is committed on its own"));
               });
            }

            // A statement marked [IGNORE-ERRORS] that the deadlock chooses as its
            // victim, sent as Database.ExecuteSQL sends one (ScriptTransaction::Execute).
            // MySQL and MariaDB answer its failure as a success, as the marker asks,
            // and have ended the transaction all the same: the Database object answers
            // the statement as failed, saying that the failure the marker let through
            // ended the transaction and what to do, refuses the next statement and
            // fails the commit, with nothing saved. Taken for run, the script would go
            // on as if its transaction held what it had done. On PostgreSQL the
            // marker's success stands, the transaction aborted and still open: the
            // database refuses the next statement itself, and PGConnection the commit.
            // SQL Server's providers answer a victim with E_FAIL, which the marker
            // never lets through: the statement fails, with the advice. Not on SQL
            // Server Compact, where no deadlock is detected.
            if (!compact)
            {
               run([&]()
               {
                  probe.Reset();
                  ScriptTransaction script;
                  BeginOn(script, pool);
                  String victimError;
                  const ScriptTransaction::Outcome victim = deadlock(script, [&]() -> ScriptTransaction::Outcome
                  {
                     return Script(script, SQLCommand(_T("update aatxnprobeb set v = v + 10 where id = 1 /* [IGNORE-ERRORS] */")), victimError);
                  });

                  String nextError;
                  const ScriptTransaction::Outcome next = Script(script, Insert(3, 30), nextError);
                  const int seen = probe.Value(_T("aatxnprobe"), 3);

                  String commitError;
                  const bool committed = script.Commit(commitError);

                  const int one = probe.Value(_T("aatxnprobe"), 1);
                  const int two = probe.Value(_T("aatxnprobe"), 2);
                  const int three = probe.Value(_T("aatxnprobe"), 3);
                  const int held = probe.Value(_T("aatxnprobeb"), 1);

                  Check(seen == -1 && three == -1,
                     Formatter::Format("a statement the script ran after its statement marked [IGNORE-ERRORS] had met a deadlock was saved on its own: {0} before the transaction's end and {1} after it, where nothing was to be saved", seen, three));
                  Check(one == 0 && two == -1 && held == 0,
                     Formatter::Format("the transaction a deadlock ended at a statement marked [IGNORE-ERRORS] left rows 1 and 2 at {0} and {1}, and the other's row at {2}, not 0, none and 0", one, two, held));
                  Check(!committed, _T("the commit of a transaction a deadlock had ended at a statement marked [IGNORE-ERRORS] reported success"));

                  if (postgres)
                  {
                     Check(victim == ScriptTransaction::Ran,
                        Formatter::Format("on PostgreSQL a statement marked [IGNORE-ERRORS] that met a deadlock was {0}, not run as the marker asks: {1}", OutcomeName(victim), victimError));
                     Check(next == ScriptTransaction::Failed,
                        Formatter::Format("on PostgreSQL the script's statement after a statement marked [IGNORE-ERRORS] had met a deadlock was {0}, not failed: the database refuses it itself", OutcomeName(next)));
                     Check(Says(commitError, _T("Postgres: the transaction was rolled back, not committed")),
                        _T("on PostgreSQL the commit after a statement marked [IGNORE-ERRORS] had met a deadlock was not refused as one the database had aborted: ") + commitError);
                  }
                  else
                  {
                     const String marked = mssql ? String(_T("The database has ended the transaction and rolled back what it held")) :
                        String(_T("the statement is marked [IGNORE-ERRORS], and its failure, which the marker let through, ended the transaction. The database has ended the transaction and rolled back what it held"));
                     Check(victim == ScriptTransaction::Failed && victimError.Find(marked) > 0 && victimError.Find(_T("Call Database.RollbackTransaction")) > 0,
                        Formatter::Format("a statement marked [IGNORE-ERRORS] that met a deadlock which ended the transaction was {0}, not failed with what to do: {1}", OutcomeName(victim), victimError));
                     Check(next == ScriptTransaction::Refused && Says(nextError, _T("The statement was not run: the database ended this transaction at an earlier statement in it that failed")),
                        Formatter::Format("the script's statement after a statement marked [IGNORE-ERRORS] had met a deadlock was {0}, not refused: {1}", OutcomeName(next), nextError));
                     Check(Says(commitError, _T("The transaction was rolled back, not committed: the database ended it at a statement in it that failed")),
                        _T("the commit after a statement marked [IGNORE-ERRORS] had met a deadlock was not refused as the Database object refuses it: ") + commitError);
                  }

                  probe.Say(_T("a statement marked [IGNORE-ERRORS] that a deadlock chose as its victim: answered as the end of the transaction, the next statement refused, the commit failed, nothing saved"));
               });
            }

            // Database.ExecuteSQLScript and Database.EnsurePrerequisites after a
            // deadlock has ended the transaction (ScriptTransaction::ExecuteScript and
            // ScriptTransaction::EnsurePrerequisites): each refused without running
            // anything, as a statement is - a script of one insert, whose row is never
            // saved, and the prerequisites of a version that has none, so that nothing
            // but the refusal answers them. Sent, the script's insert would be
            // committed on its own, outside any transaction, on MySQL, MariaDB and SQL
            // Server, and the prerequisites said to be met. Not on PostgreSQL, whose
            // database refuses the script itself, nor on SQL Server Compact, whose
            // transaction goes on.
            if (!compact && !postgres)
            {
               run([&]()
               {
                  probe.Reset();
                  const String file = FileUtilities::Combine(IniFileSettings::Instance()->GetTempDirectory(), _T("aatxnafter-") + GUIDCreator::GetGUID() + _T(".sql"));
                  Check(FileUtilities::WriteToFile(file, AnsiString(String(_T("insert into aatxnprobe (id, v) values (50, 50)\n\n")))), _T("the tester could not write its script to ") + file);

                  ScriptTransaction::Outcome scriptOutcome = ScriptTransaction::Ran;
                  ScriptTransaction::Outcome prerequisitesOutcome = ScriptTransaction::Ran;
                  String scriptError;
                  String prerequisitesError;
                  int seen = -2;
                  std::exception_ptr failure;
                  try
                  {
                     ScriptTransaction script;
                     BeginOn(script, pool);
                     deadlock(script, [&]() -> ScriptTransaction::Outcome
                     {
                        String error;
                        return Script(script, Update(_T("aatxnprobeb"), 10), error);
                     });

                     scriptOutcome = script.ExecuteScript(file, scriptError);
                     seen = probe.Value(_T("aatxnprobe"), 50);
                     prerequisitesOutcome = script.EnsurePrerequisites(1, prerequisitesError);

                     String rollbackError;
                     script.Rollback(rollbackError);
                  }
                  catch (...)
                  {
                     failure = std::current_exception();
                  }

                  FileUtilities::DeleteFile(file);

                  if (failure)
                     std::rethrow_exception(failure);

                  const int after = probe.Value(_T("aatxnprobe"), 50);
                  const String refusal = _T("The statement was not run: the database ended this transaction at an earlier statement in it that failed");

                  Check(seen == -1 && after == -1,
                     Formatter::Format("the insert of a script run after a deadlock had ended its transaction was saved on its own, as on 6e1a336c5 on MySQL and MariaDB: {0} before the transaction's end and {1} after it, where nothing was to be saved", seen, after));
                  Check(scriptOutcome == ScriptTransaction::Refused && Says(scriptError, refusal),
                     Formatter::Format("a script run after a deadlock had ended its transaction was {0}, not refused: {1}", OutcomeName(scriptOutcome), scriptError));
                  Check(prerequisitesOutcome == ScriptTransaction::Refused && Says(prerequisitesError, refusal),
                     Formatter::Format("the prerequisites asked for after a deadlock had ended the transaction were {0}, not refused: {1}", OutcomeName(prerequisitesOutcome), prerequisitesError));
                  probe.Say(_T("a script and the prerequisites asked for after a deadlock had ended the transaction: both refused, nothing saved"));
               });
            }

            // A script of statements in the transaction (Database.ExecuteSQLScript,
            // ScriptTransaction::ExecuteScript), whose statement marked
            // [IGNORE-ERRORS] is the deadlock's victim. MySQL, MariaDB and PostgreSQL
            // answer its failure as a success, which has ended the transaction: the
            // runner stops there, and the rest of the script is not run. On 6e1a336c5
            // it went on, and on MySQL and MariaDB the script's next statement was
            // committed on its own. (SQL Server's providers answer a victim with
            // E_FAIL, a lost connection's, which the marker never lets through.)
            if (!compact)
            {
               run([&]()
               {
                  probe.Reset();

                  const String separator = postgres ? _T(";\n\n") : _T("\n\n");
                  const String file = FileUtilities::Combine(IniFileSettings::Instance()->GetTempDirectory(), _T("aatxnscript-") + GUIDCreator::GetGUID() + _T(".sql"));
                  const String text = _T("insert into aatxnprobe (id, v) values (5, 50)") + separator +
                                      _T("update aatxnprobeb set v = v + 10 where id = 1 /* [IGNORE-ERRORS] */") + separator +
                                      _T("insert into aatxnprobe (id, v) values (6, 60)") + separator;
                  Check(FileUtilities::WriteToFile(file, AnsiString(text)), _T("the tester could not write its script to ") + file);

                  bool ran = false;
                  String scriptError;
                  int seen = -2;
                  std::exception_ptr failure;
                  try
                  {
                     ScriptTransaction script;
                     BeginOn(script, pool);
                     deadlock(script, [&]() -> ScriptTransaction::Outcome
                     {
                        const ScriptTransaction::Outcome outcome = script.ExecuteScript(file, scriptError);
                        ran = outcome == ScriptTransaction::Ran;
                        return outcome;
                     });

                     seen = probe.Value(_T("aatxnprobe"), 6);

                     String rollbackError;
                     script.Rollback(rollbackError);
                  }
                  catch (...)
                  {
                     failure = std::current_exception();
                  }

                  FileUtilities::DeleteFile(file);

                  if (failure)
                     std::rethrow_exception(failure);

                  Check(seen == -1 && probe.Value(_T("aatxnprobe"), 6) == -1,
                     _T("a statement of a script run after its statement marked [IGNORE-ERRORS] had met a deadlock was saved on its own, as on 6e1a336c5 on MySQL and MariaDB: the runner went on past the end of the transaction"));
                  Check(!ran, _T("a script whose statement marked [IGNORE-ERRORS] met a deadlock that ended its transaction was said to have run: ") + scriptError);
                  Check(probe.Value(_T("aatxnprobe"), 5) == -1 && probe.Value(_T("aatxnprobe"), 2) == -1 && probe.Value(_T("aatxnprobe"), 1) == 0,
                     _T("a script's transaction a deadlock ended left a row of it"));

                  // On MySQL and MariaDB the runner stopped, and the Database object says
                  // what to do; on PostgreSQL the database refused the next statement.
                  if (mysql)
                  {
                     Check(scriptError.Find(_T("The script was stopped after this statement")) > 0 && scriptError.Find(_T("Call Database.RollbackTransaction")) > 0,
                        _T("the error of a script stopped after a deadlock that ended its transaction did not say so: ") + scriptError);
                  }

                  probe.Say(_T("a script whose statement marked [IGNORE-ERRORS] met a deadlock: the runner stopped there, and nothing after it was saved"));
               });
            }

            // MariaDB's 1020, a row another session changed and committed after the
            // transaction read it, under innodb_snapshot_isolation (on by default in
            // MariaDB 11.8): the whole transaction rolled back, as at a deadlock. MySQL
            // has no such check and no such variable: the update reads the row as
            // committed, and the transaction commits whole - the control.
            if (mysql)
            {
               run([&]()
               {
                  bool snapshotIsolation = false;
                  {
                     std::shared_ptr<DALRecordset> isolation = probe.raw_->CreateRecordset();
                     String error;
                     if (isolation->TryOpen(probe.raw_, SQLCommand(_T("select @@innodb_snapshot_isolation as snapshotisolation")), error) == DALConnection::DALSuccess && !isolation->IsEOF())
                        snapshotIsolation = isolation->GetLongValue("snapshotisolation") != 0;
                  }

                  probe.Reset();
                  ScriptTransaction script;
                  BeginOn(script, pool);
                  String error;
                  Check(Script(script, Insert(2, 20), error) == ScriptTransaction::Ran, _T("the script's insert failed: ") + error);

                  std::shared_ptr<DALRecordset> read = script.GetConnection()->CreateRecordset();
                  Check(read->TryOpen(script.GetConnection(), SQLCommand(_T("select v from aatxnprobe where id = 1")), error) == DALConnection::DALSuccess,
                     _T("the script's read failed: ") + error);

                  probe.Raw(_T("update aatxnprobe set v = v + 5 where id = 1"));

                  const ScriptTransaction::Outcome changed = Script(script, Update(_T("aatxnprobe"), 1), error);
                  const ScriptTransaction::Outcome next = Script(script, Insert(3, 30), error);
                  const int seen = probe.Value(_T("aatxnprobe"), 3);

                  String commitError;
                  const bool committed = script.Commit(commitError);

                  const int one = probe.Value(_T("aatxnprobe"), 1);
                  const int two = probe.Value(_T("aatxnprobe"), 2);
                  const int three = probe.Value(_T("aatxnprobe"), 3);

                  if (snapshotIsolation)
                  {
                     Check(changed == ScriptTransaction::Failed, Formatter::Format("MariaDB answered an update of a row changed since the transaction read it with {0}, not its error 1020", OutcomeName(changed)));
                     Check(seen == -1 && three == -1,
                        Formatter::Format("a statement the script ran after MariaDB had rolled its transaction back at a row changed since it was read (1020) was saved on its own, as on 6e1a336c5: {0} before the transaction's end and {1} after it",
                           seen, three));
                     Check(one == 5 && two == -1, Formatter::Format("the transaction MariaDB rolled back at its error 1020 left rows 1 and 2 at {0} and {1}, not 5 and none", one, two));
                     Check(next == ScriptTransaction::Refused && !committed,
                        Formatter::Format("after MariaDB's error 1020 the script's next statement was {0} and its commit {1}, where it is refused and the commit fails",
                           OutcomeName(next), committed ? _T("reported success") : _T("failed")));
                     probe.Say(_T("MariaDB's 1020, a row changed since the transaction read it: the next statement refused, the commit failed, nothing saved"));
                  }
                  else
                  {
                     Check(changed == ScriptTransaction::Ran && next == ScriptTransaction::Ran && seen == -1 && committed,
                        Formatter::Format("with no snapshot isolation, an update of a row another session had changed was {0}, the next statement {1}, seen {2} before the commit, and the commit {3}: {4}",
                           OutcomeName(changed), OutcomeName(next), seen, committed ? _T("succeeded") : _T("failed"), commitError));
                     Check(one == 6 && two == 20 && three == 30, Formatter::Format("with no snapshot isolation the transaction left {0}, {1} and {2}, not 6, 20 and 30", one, two, three));
                     probe.Say(_T("no snapshot isolation (MySQL): a row changed since the transaction read it is updated, and the transaction commits whole"));
                  }
               });
            }

            // A lock wait that timed out (1205), the script's made a second long on a
            // pool of its own, so that the pool's connections keep theirs. With
            // innodb_rollback_on_timeout on, the whole transaction is rolled back, as at
            // a deadlock; off (the servers' default), its own statement alone, and the
            // transaction goes on - as before.
            if (mysql)
            {
               run([&]()
               {
                  bool rollbackOnTimeout = false;
                  {
                     std::shared_ptr<DALRecordset> variable = probe.raw_->CreateRecordset();
                     String error;
                     Check(variable->TryOpen(probe.raw_, SQLCommand(_T("select @@innodb_rollback_on_timeout as rollbackontimeout")), error) == DALConnection::DALSuccess && !variable->IsEOF(),
                        _T("the tester could not read innodb_rollback_on_timeout: ") + error);
                     rollbackOnTimeout = variable->GetLongValue("rollbackontimeout") != 0;
                  }

                  probe.Reset();
                  std::shared_ptr<DatabaseConnectionManager> timing = OpenPool(settings, 1);
                  std::exception_ptr failure;
                  try
                  {
                     ScriptTransaction script;
                     BeginOn(script, timing);
                     String error;
                     Check(script.GetConnection()->TryExecute(SQLCommand(_T("set session innodb_lock_wait_timeout = 1")), error, 0, 0) == DALConnection::DALSuccess,
                        _T("the tester could not shorten the script's lock wait: ") + error);
                     Check(Script(script, Insert(2, 20), error) == ScriptTransaction::Ran, _T("the script's insert failed: ") + error);

                     ScriptTransaction::Outcome waited = ScriptTransaction::Ran;
                     {
                        ScriptTransaction other;
                        BeginOn(other, pool);
                        Check(other.GetConnection()->TryExecute(Update(_T("aatxnprobe"), 100), error, 0, 0) == DALConnection::DALSuccess, _T("the other transaction's update failed: ") + error);
                        waited = Script(script, Update(_T("aatxnprobe"), 1), error);
                     }

                     const ScriptTransaction::Outcome next = Script(script, Insert(3, 30), error);
                     const int seen = probe.Value(_T("aatxnprobe"), 3);

                     String commitError;
                     const bool committed = script.Commit(commitError);

                     const int one = probe.Value(_T("aatxnprobe"), 1);
                     const int two = probe.Value(_T("aatxnprobe"), 2);
                     const int three = probe.Value(_T("aatxnprobe"), 3);

                     Check(waited == ScriptTransaction::Failed, Formatter::Format("the script's update of a row another transaction held was {0}, not failed after its lock wait timed out", OutcomeName(waited)));

                     if (rollbackOnTimeout)
                     {
                        Check(seen == -1 && three == -1,
                           Formatter::Format("a statement the script ran after a lock wait that timed out had rolled its transaction back (innodb_rollback_on_timeout on) was saved on its own, as on 6e1a336c5: {0} before the transaction's end and {1} after it",
                              seen, three));
                        Check(one == 0 && two == -1, Formatter::Format("the transaction a lock wait that timed out rolled back left rows 1 and 2 at {0} and {1}, not 0 and none", one, two));
                        Check(next == ScriptTransaction::Refused && !committed,
                           Formatter::Format("after a lock wait that timed out with innodb_rollback_on_timeout on, the script's next statement was {0} and its commit {1}, where it is refused and the commit fails",
                              OutcomeName(next), committed ? _T("reported success") : _T("failed")));
                        probe.Say(_T("a lock wait that timed out with innodb_rollback_on_timeout on: the next statement refused, the commit failed, nothing saved"));
                     }
                     else
                     {
                        Check(next == ScriptTransaction::Ran && seen == -1 && committed,
                           Formatter::Format("after a lock wait that timed out with innodb_rollback_on_timeout off, the script's next statement was {0}, seen {1} before the commit, and the commit {2}: {3}",
                              OutcomeName(next), seen, committed ? _T("succeeded") : _T("failed"), commitError));
                        Check(one == 0 && two == 20 && three == 30, Formatter::Format("after a lock wait that timed out with innodb_rollback_on_timeout off the transaction left {0}, {1} and {2}, not 0, 20 and 30", one, two, three));
                        probe.Say(_T("a lock wait that timed out with innodb_rollback_on_timeout off: its own statement taken back, and the transaction went on and committed"));
                     }
                  }
                  catch (...)
                  {
                     failure = std::current_exception();
                  }

                  timing->Disconnect();

                  if (failure)
                     std::rethrow_exception(failure);
               });
            }

            // A metadata-lock wait that timed out (1205 as well): a transaction holds a
            // shared metadata lock on aatxnprobeb, an ALTER TABLE of it waits behind
            // that, and the script's read of the table waits behind the ALTER until its
            // lock_wait_timeout, made a second long on a pool of its own. That ends
            // nothing, whatever innodb_rollback_on_timeout says: the script's statement
            // fails, and the transaction goes on and commits whole (measured with the
            // databases' own clients on MySQL 8.4.11 and MariaDB 11.8.9, each with
            // innodb_rollback_on_timeout on and off, 16 October 2026). On f7dcb3862, where
            // innodb_rollback_on_timeout is on, every 1205 was taken for the end of the
            // transaction: the rest of a transaction still open was refused, and its
            // commit rolled back what a commit would have saved.
            if (mysql)
            {
               run([&]()
               {
                  probe.Reset();
                  std::shared_ptr<DatabaseConnectionManager> timing = OpenPool(settings, 1);
                  std::shared_ptr<DALConnection> alterer = probe.Connect(_T("the ALTER TABLE that waits"));
                  std::thread alter;
                  bool altered = false;
                  std::exception_ptr failure;
                  try
                  {
                     ScriptTransaction script;
                     BeginOn(script, timing);
                     String error;
                     Check(script.GetConnection()->TryExecute(SQLCommand(_T("set session lock_wait_timeout = 1")), error, 0, 0) == DALConnection::DALSuccess,
                        _T("the tester could not shorten the script's metadata-lock wait: ") + error);
                     Check(Script(script, Insert(2, 20), error) == ScriptTransaction::Ran, _T("the script's insert failed: ") + error);

                     ScriptTransaction::Outcome waited = ScriptTransaction::Ran;
                     String waitError;
                     {
                        ScriptTransaction holder;
                        BeginOn(holder, pool);
                        std::shared_ptr<DALRecordset> held = holder.GetConnection()->CreateRecordset();
                        Check(held->TryOpen(holder.GetConnection(), SQLCommand(_T("select v from aatxnprobeb where id = 1")), error) == DALConnection::DALSuccess,
                           _T("the holder's read failed: ") + error);

                        Check(alterer->TryExecute(SQLCommand(_T("set session lock_wait_timeout = 30")), error, 0, 0) == DALConnection::DALSuccess,
                           _T("the tester could not set the ALTER TABLE's metadata-lock wait: ") + error);
                        alter = std::thread([&]()
                        {
                           String alterError;
                           altered = alterer->TryExecute(SQLCommand(_T("alter table aatxnprobeb add column w int null")), alterError, 0, 0) == DALConnection::DALSuccess;
                        });

                        // The ALTER TABLE waiting for the holder's lock, seen in the
                        // server's own list of the tester's sessions, before the script
                        // reads the table.
                        bool waiting = false;
                        for (int i = 0; i < 250 && !waiting; i++)
                        {
                           std::shared_ptr<DALRecordset> waits = probe.raw_->CreateRecordset();
                           Check(waits->TryOpen(probe.raw_, SQLCommand(_T("select count(*) as waiting from information_schema.processlist where state like 'Waiting for table metadata lock%' and info like 'alter table aatxnprobeb%'")), error) == DALConnection::DALSuccess,
                              _T("the tester could not see whether the ALTER TABLE waits: ") + error);
                           waiting = waits->GetLongValue("waiting") > 0;
                           if (!waiting)
                              std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        }
                        Check(waiting, _T("the ALTER TABLE did not wait for the holder's metadata lock within five seconds"));

                        waited = Script(script, SQLCommand(_T("select v from aatxnprobeb where id = 1")), waitError);

                        // The holder's end lets the ALTER TABLE go on.
                        String ignored;
                        holder.Rollback(ignored);
                     }

                     alter.join();

                     const bool ended = script.GetConnection()->IsTransactionFailed();
                     const ScriptTransaction::Outcome next = Script(script, Insert(3, 30), error);
                     const int seen = probe.Value(_T("aatxnprobe"), 3);

                     String commitError;
                     const bool committed = script.Commit(commitError);

                     const int two = probe.Value(_T("aatxnprobe"), 2);
                     const int three = probe.Value(_T("aatxnprobe"), 3);

                     Check(waited == ScriptTransaction::Failed && waitError.Find(_T("Lock wait timeout")) > 0,
                        Formatter::Format("the script's read of a table an ALTER TABLE waited for was {0}, not failed after its metadata-lock wait timed out: {1}", OutcomeName(waited), waitError));
                     Check(!ended && next == ScriptTransaction::Ran && seen == -1 && committed,
                        Formatter::Format("after a metadata-lock wait that timed out, which ends nothing, the transaction was said to have ended ({0}), its next statement was {1}, seen {2} before the commit, and the commit {3}: {4}",
                           ended ? 1 : 0, OutcomeName(next), seen, committed ? _T("succeeded") : _T("failed"), commitError));
                     Check(two == 20 && three == 30,
                        Formatter::Format("after a metadata-lock wait that timed out the transaction left {0} and {1}, not 20 and 30", two, three));
                     Check(altered, _T("the ALTER TABLE the script's read waited behind did not run once the holder's transaction ended"));
                  }
                  catch (...)
                  {
                     failure = std::current_exception();
                  }

                  // The ALTER TABLE ends once the holder's transaction has, which leaving
                  // the block above saw to whatever failed in it.
                  if (alter.joinable())
                     alter.join();

                  String ignored;
                  if (altered)
                     alterer->TryExecute(SQLCommand(_T("alter table aatxnprobeb drop column w")), ignored, 0, 0);

                  alterer->Disconnect();
                  timing->Disconnect();

                  if (failure)
                     std::rethrow_exception(failure);

                  probe.Say(_T("a metadata-lock wait that timed out: its own statement failed, and the transaction went on and committed"));
               });
            }

            // Database.BeginTransaction called while its transaction is open: the open
            // one is rolled back and its connection given back to the pool before the
            // second begins. Up to 6.3.6 the Database object let go of the first with
            // its transaction open - the pool never had the connection back, and what
            // the transaction held stayed locked until the server stopped. Every
            // engine: the pool's busy connections are one more, not two, and the first
            // transaction's row is gone.
            run([&]()
            {
               probe.Reset();
               const size_t busyBefore = pool->busy_connections_.size();

               ScriptTransaction script;
               BeginOn(script, pool);
               String error;
               Check(Script(script, Insert(7, 70), error) == ScriptTransaction::Ran, _T("the first transaction's insert failed: ") + error);
               const std::shared_ptr<DALConnection> first = script.GetConnection();

               BeginOn(script, pool);
               const size_t busyAfter = pool->busy_connections_.size();

               // Not on SQL Server Compact, where a read would wait for a row a
               // transaction left open still holds.
               const int seen = compact ? -1 : probe.Value(_T("aatxnprobe"), 7);

               const ScriptTransaction::Outcome second = Script(script, Insert(8, 80), error);
               String commitError;
               const bool committed = script.Commit(commitError);

               // A first transaction left open, which this case is here to find, is
               // rolled back and given back now, before anything is said: its locks
               // would hold up every case after this one, and the tables' drop at the
               // end would wait for them.
               const bool leaked = pool->busy_connections_.count(first) > 0;
               if (leaked)
               {
                  String ignored;
                  pool->RollbackTransaction(first, ignored);
               }

               Check(!leaked && busyAfter == busyBefore + 1,
                  Formatter::Format("a second Begin while a transaction was open left {0} of the pool's connections taken, where the first should have been given back and one taken: {1} before",
                     (int) busyAfter, (int) busyBefore));
               Check(second == ScriptTransaction::Ran && committed, _T("the second transaction was not committed: ") + error + _T(" ") + commitError);
               Check(seen == -1 && probe.Value(_T("aatxnprobe"), 7) == -1 && probe.Value(_T("aatxnprobe"), 8) == 80,
                  Formatter::Format("a second Begin while a transaction was open left the first's row at {0} (and {1} once the second was committed) and the second's at {2}, not none, none and 80",
                     seen, probe.Value(_T("aatxnprobe"), 7), probe.Value(_T("aatxnprobe"), 8)));
               Check(pool->busy_connections_.size() == busyBefore, _T("the second transaction's connection was not given back at its commit"));
               probe.Say(_T("a second Begin while a transaction is open: the first rolled back and its connection given back"));
            });

            if (!failed.empty())
            {
               std::string all;
               for (const std::string &one : failed)
                  all += (all.empty() ? "" : " AND ") + one;
               throw std::logic_error(all);
            }
         }

         // The session lost in the middle of a unit of work: nothing of the attempt is
         // committed, and Run's next attempt commits it once. Then the same in a
         // transaction the pool hands out, held as the Database object holds its
         // script's (ScriptTransaction), under DALConnection::Execute - which
         // Database.EnsurePrerequisites' prerequisites run their statements with -
         // and under DALRecordset::Open, which a script's macros run their queries
         // with: each reconnects, and runs nothing on the new session, where it would
         // be committed on its own, outside the transaction; the transaction is said
         // to have ended (IsTransactionFailed, on every engine), the script's next
         // statement is refused and its commit fails, each saying why. Up to
         // 1701f240f both ran the statement again on the new session and everything
         // after it ran there in autocommit, the script's next statement committed on
         // its own and nothing refused (ActiveActive.md, C17).
         {
            probe.Reset();
            int calls = 0;
            DatabaseTransaction::RunResult result = DatabaseTransaction::Run(pool, [&](DatabaseTransaction &transaction)
            {
               calls++;
               if (!transaction.Execute(Insert(10, 1)))
                  return false;
               if (calls == 1)
                  probe.EndSessionOf(transaction.root_->connection_);
               return transaction.Execute(Insert(11, 1));
            });
            Check(result.outcome == DatabaseTransaction::Committed && result.attempts == 2 && calls == 2,
               Formatter::Format("a unit of work whose session ended in the middle was not committed by a second attempt: outcome {0}, {1} attempts, {2} calls: {3}",
                  (int) result.outcome, result.attempts, calls, result.error));
            Check(probe.Value(_T("aatxnprobe"), 10) == 1 && probe.Value(_T("aatxnprobe"), 11) == 1, _T("a unit of work retried after a lost session did not leave its two rows once each"));

            // A scope whose session was lost sends no ROLLBACK to a session that is
            // gone, and opens its connection again before the pool has it back
            // (DatabaseTransaction::Fail_, Release_): the connection goes back
            // holding no transaction, and a statement the pool runs on it outside
            // any - DALConnection::Execute, on a pool of that one connection - runs.
            // It would be refused as one of a transaction lost with its session if
            // the pool kept what BeginTransaction recorded
            // (DALConnection::ForgetTransaction).
            {
               probe.Reset();
               std::shared_ptr<DatabaseConnectionManager> single = OpenPool(settings, 1);
               std::exception_ptr failure;
               try
               {
                  {
                     DatabaseTransaction transaction(single);
                     Check(transaction.Execute(Insert(12, 1)), _T("the insert of a scope on a pool of one connection failed"));
                     probe.EndSessionOf(transaction.root_->connection_);
                     Check(!transaction.Execute(Insert(13, 1)) && transaction.GetFailure() == DatabaseTransaction::FailureConnection,
                        _T("a statement on a scope's lost session was not failed as a lost connection"));
                  }

                  String error;
                  Check(single->Execute(Insert(14, 1), 0, 0, error),
                     _T("a statement the pool ran outside any transaction, on the connection a scope had lost its session on, failed: ") + error);
                  Check(probe.Value(_T("aatxnprobe"), 12) == -1 && probe.Value(_T("aatxnprobe"), 13) == -1 && probe.Value(_T("aatxnprobe"), 14) == 1,
                     _T("a scope whose session was lost, and a statement the pool ran after it, left rows 12, 13 and 14 other than none, none and 1"));
               }
               catch (...)
               {
                  failure = std::current_exception();
               }

               single->Disconnect();

               if (failure)
                  std::rethrow_exception(failure);
            }

            // On every engine: MySQL and MariaDB over TLS answer a statement on a
            // session the server ended with the TLS layer's end of file (2026), which
            // MySQLConnection::CheckError takes for a lost connection as it takes
            // 2013 over a plain one (unit AA-01), so Execute and Open reconnect there
            // too. Each case is a check of its own, said together at the end.
            std::vector<std::string> lostFailed;
            for (int path = 0; path < 2; path++)
            {
               const String through = path == 0 ? _T("DALConnection::Execute") : _T("DALRecordset::Open");
               try
               {
                  probe.Reset();
                  ScriptTransaction script;
                  BeginOn(script, pool);
                  String error;
                  Check(Script(script, Insert(20, 1), error) == ScriptTransaction::Ran, _T("the script's first insert failed: ") + error);
                  probe.EndSessionOf(script.GetConnection());

                  bool again = false;
                  if (path == 0)
                  {
                     again = script.GetConnection()->Execute(Insert(21, 1), error);
                  }
                  else
                  {
                     std::shared_ptr<DALRecordset> read = script.GetConnection()->CreateRecordset();
                     again = read->Open(script.GetConnection(), SQLCommand(_T("select v from aatxnprobe where id = 1")));
                     if (again)
                        Check(script.GetConnection()->TryExecute(Insert(21, 1), error, 0, 0) == DALConnection::DALSuccess, _T("the insert after the query run again failed: ") + error);
                  }

                  const bool ended = script.GetConnection()->IsTransactionFailed();
                  String nextError;
                  const ScriptTransaction::Outcome next = Script(script, Insert(22, 1), nextError);
                  const int seen = probe.Value(_T("aatxnprobe"), 22);

                  String commitError;
                  const bool committed = script.Commit(commitError);

                  const int twenty = probe.Value(_T("aatxnprobe"), 20);
                  const int twentyOne = probe.Value(_T("aatxnprobe"), 21);
                  const int twentyTwo = probe.Value(_T("aatxnprobe"), 22);

                  Check(twenty == -1 && twentyOne == -1 && twentyTwo == -1 && seen == -1,
                     Formatter::Format("a transaction whose session ended under {0} left rows 20, 21 and 22 at {1}, {2} and {3} (22 at {4} before the commit), not none: what ran on the new session was committed on its own, as up to 1701f240f",
                        through, twenty, twentyOne, twentyTwo, seen));
                  Check(!again,
                     Formatter::Format("after the session of a transaction the pool handed out ended, {0} ran its statement again on the new session, outside the transaction, as up to 1701f240f", through));
                  Check(ended,
                     Formatter::Format("a transaction whose session ended under {0} was not said to have ended (DALConnection::IsTransactionFailed)", through));
                  Check(next == ScriptTransaction::Refused && Says(nextError, _T("The statement was not run: the connection to the database was lost inside this transaction")),
                     Formatter::Format("after a transaction's session ended under {0}, the script's next statement was {1}, not refused as one of a transaction lost with its session: {2}",
                        through, OutcomeName(next), nextError));
                  Check(!committed && Says(commitError, _T("The transaction was rolled back, not committed: the connection to the database was lost inside it")),
                     Formatter::Format("after a transaction's session ended under {0}, the commit {1}: {2}",
                        through, committed ? _T("reported success") : _T("failed without saying that the session was lost"), commitError));
               }
               catch (const std::exception &failure)
               {
                  lostFailed.push_back(failure.what());
               }
            }

            if (!lostFailed.empty())
            {
               std::string all;
               for (const std::string &one : lostFailed)
                  all += (all.empty() ? "" : " AND ") + one;
               throw std::logic_error(all);
            }

            // A session that ends before the COMMIT: whether the COMMIT reached the
            // database is not known, so the scope says so and is not run again.
            probe.Reset();
            {
               DatabaseTransaction transaction(pool);
               Check(transaction.Execute(Insert(30, 1)), _T("the insert before the lost commit failed"));
               probe.EndSessionOf(transaction.root_->connection_);
               Check(!transaction.Commit() && transaction.GetFailure() == DatabaseTransaction::FailureCommitUnknown,
                  _T("a COMMIT whose session had ended was not said to be of unknown outcome"));
            }
            Check(probe.Value(_T("aatxnprobe"), 30) == -1, _T("a transaction whose session ended before its COMMIT was committed"));

            if (provokeErrors)
            {
               // Run reports it (HM8501) and does not run the unit again.
               probe.Reset();
               int runs = 0;
               DatabaseTransaction::RunResult unknown = DatabaseTransaction::Run(pool, [&](DatabaseTransaction &transaction)
               {
                  runs++;
                  if (!transaction.Execute(Insert(31, 1)))
                     return false;
                  probe.EndSessionOf(transaction.root_->connection_);
                  return true;
               });
               Check(unknown.outcome == DatabaseTransaction::CommitUnknown && runs == 1,
                  Formatter::Format("a unit of work whose COMMIT's connection was lost was not said to be of unknown outcome ({0}), or was run again: outcome {1}, {2} runs: {3}",
                     (int) DatabaseTransaction::CommitUnknown, (int) unknown.outcome, runs, unknown.error));
               Check(probe.Value(_T("aatxnprobe"), 31) == -1, _T("the unit of work whose session ended before its COMMIT was committed"));
            }

            // The pool's connections were opened again by the scopes that lost them:
            // a statement through the manager works on each.
            for (int i = 0; i < 4; i++)
               Check(pool->Execute(Update(_T("aatxnprobeb"), 1)), _T("a pool connection a transaction lost was not opened again"));

            probe.Say(_T("a session lost in the middle: nothing committed and the retry commits once; in a transaction the pool handed out nothing run on the new session, the next statement refused, the commit failed; a lost COMMIT is not run again"));
         }

         // MariaDB: its error 4031 is a statement's own refusal (a CREATE TRIGGER that
         // FOLLOWS a trigger there is not), where MySQL's is an idle session the server
         // closed (MySQLConnection::ClassifyLastFailure, as CheckError has it since
         // unit AA-01): a unit of work it fails is failed once, as a statement the
         // database refused, and not run again as one whose connection was lost.
         // Which error MariaDB gave is read first with GET DIAGNOSTICS, on the
         // tester's own connection; refused for another reason (a privilege), the
         // check is said and left out. It reports HM5032, as any failed statement does.
         if (mysql && provokeErrors)
         {
            std::shared_ptr<DALRecordset> version = probe.raw_->CreateRecordset();
            String error;
            Check(version->TryOpen(probe.raw_, SQLCommand(_T("select version() as serverversion")), error) == DALConnection::DALSuccess && !version->IsEOF(),
               _T("the tester could not read the server's version: ") + error);
            String fork = version->GetStringValue("serverversion");
            fork.MakeLower();

            if (fork.Find(_T("mariadb")) >= 0)
            {
               const String trigger = _T("create trigger aatxntrigger before insert on aatxnprobe for each row follows aatxnnosuchtrigger set new.v = 1");

               probe.Reset();
               if (probe.raw_->TryExecute(SQLCommand(trigger), error, 0, 0) == DALConnection::DALSuccess)
               {
                  probe.Raw(_T("drop trigger aatxntrigger"));
                  Check(false, _T("MariaDB made a trigger that FOLLOWS a trigger there is not"));
               }

               probe.Raw(_T("get diagnostics condition 1 @aatxnerror = mysql_errno"));
               std::shared_ptr<DALRecordset> diagnosed = probe.raw_->CreateRecordset();
               Check(diagnosed->TryOpen(probe.raw_, SQLCommand(_T("select @aatxnerror as errorcode")), error) == DALConnection::DALSuccess && !diagnosed->IsEOF(),
                  _T("the tester could not read the error MariaDB gave: ") + error);
               const long code = diagnosed->GetLongValue("errorcode");

               if (code != 4031)
               {
                  probe.Say(Formatter::Format("MariaDB's error 4031 inside a scope not checked: the server refused the trigger with error {0} first", (int) code));
               }
               else
               {
                  int calls = 0;
                  DatabaseTransaction::RunResult result = DatabaseTransaction::Run(pool, [&](DatabaseTransaction &transaction)
                  {
                     calls++;
                     return transaction.Execute(SQLCommand(trigger));
                  });
                  Check(result.outcome == DatabaseTransaction::Failed && result.attempts == 1 && calls == 1,
                     Formatter::Format("a unit of work a statement MariaDB refused with error 4031 failed was not failed once as a statement the database refused: outcome {0}, {1} attempts, {2} calls: {3}",
                        (int) result.outcome, result.attempts, calls, result.error));
                  probe.Say(_T("MariaDB's error 4031, a trigger the statement names that does not exist: the unit of work failed once and was not run again"));
               }
            }
         }

         // MySQL and MariaDB: a database with a table that is not InnoDB has no
         // transactions, and a scope on it says so.
         if (mysql)
         {
            probe.Raw(_T("create table aatxnprobem (id int not null) ENGINE=MyISAM"));

            // A pool connected after the table was made, as LoadSupportsTransactions_
            // looks at the tables when a connection opens.
            std::shared_ptr<DatabaseConnectionManager> myisam = OpenPool(settings, 1);

            probe.Reset();
            {
               DatabaseTransaction transaction(myisam);
               Check(transaction.IsOpen() && !transaction.IsTransactional(), _T("a scope on a database with a MyISAM table did not say that it is not transactional"));

               // Run as the pool runs it, and counted as the pool counts it.
               __int64 inserted = -2;
               const bool ran = transaction.Execute(Insert(40, 1), 0, 0, &inserted);
               Check(ran && inserted == 1, Formatter::Format("an insert on a database that is not transactional failed, or counted {0}, not 1", inserted));
               transaction.Rollback();
            }
            Check(probe.Value(_T("aatxnprobe"), 40) == 1, _T("a scope on a database that is not transactional took back what it wrote, which it cannot"));

            myisam->Disconnect();
            probe.Raw(_T("drop table aatxnprobem"));

            probe.Say(_T("a database with a MyISAM table: the scope says it is not transactional"));
         }
      };

      std::exception_ptr failure;
      try
      {
         probe.Start();
         checks();
      }
      catch (...)
      {
         failure = std::current_exception();
      }

      try
      {
         probe.Finish();
      }
      catch (...)
      {
         if (!failure)
            failure = std::current_exception();
      }

      if (failure)
         std::rethrow_exception(failure);
   }

#ifndef HM_PLATFORM_POSIX
   void
   DatabaseTransactionTester::TestCompact()
   {
      const String directory = IniFileSettings::Instance()->GetTempDirectory() + FileUtilities::PathSeparator + _T("DatabaseTransactionTest-") + GUIDCreator::GetGUID();
      if (!FileUtilities::CreateDirectory(directory))
         Fail_(_T("The tester's directory could not be created under the temp folder."));

      try
      {
         const String name = _T("DatabaseTransactionTest");
         const String password = PasswordGenerator::Generate();
         String error;
         if (!SQLCEConnection::CreateDatabase(directory, name, password, error))
            Fail_(_T("The tester's Compact database could not be created: ") + error);

         const String empty;
         std::shared_ptr<DatabaseSettings> settings(new DatabaseSettings(empty, empty, name, empty, password, directory, empty, DatabaseSettings::TypeMSSQLCompactEdition, 0));

         Test(settings, [](const String &line) { OutputDebugString((_T("hMailServer: DatabaseTransaction: ") + line + _T("\n")).c_str()); }, false);
      }
      catch (...)
      {
         FileUtilities::DeleteDirectory(directory, true);
         throw;
      }

      FileUtilities::DeleteDirectory(directory, true);
   }

   void
   DatabaseTransactionTester::TestOnErrorOnTheServersDatabase()
   {
      // ErrorManager fires OnError only while event scripts run, and with them off
      // the failure below would be an error reported for nothing: the fixture that
      // runs ClassTester with scripting off asserts the error log empty.
      if (!Configuration::Instance() || !Configuration::Instance()->GetUseScriptServer())
      {
         OutputDebugString(_T("hMailServer: DatabaseTransaction: OnError outside the transaction - not checked, event scripts are off\n"));
         return;
      }

      std::shared_ptr<DatabaseConnectionManager> manager = Application::Instance()->GetDBManager();
      Check(manager != nullptr, _T("the server has no database connection manager"));

      DatabaseTransaction transaction(manager);
      Check(transaction.IsOpen(), _T("a transaction did not open on the server's own database: ") + transaction.GetErrorMessage());

      // A statement the database refuses, reported (HM5032) after the transaction
      // has been rolled back - and while it is reported, ErrorManager runs the
      // OnError script of the fixture that runs this, which saves a message to the
      // queue: on the pool's connections, outside this failed transaction, where
      // its Save would be refused and the message never sent. The fixture
      // (RegressionTests.API.Events) finds the message delivered.
      Check(!transaction.Execute(SQLCommand(_T("insert into aatxn_onerror_missing (id) values (1)"))),
         _T("an insert into a table that does not exist succeeded"));
      Check(transaction.GetFailure() == DatabaseTransaction::FailureStatement,
         _T("an insert into a table that does not exist did not fail the transaction as a statement the database refused"));

      OutputDebugString(_T("hMailServer: DatabaseTransaction: OnError outside the transaction - a statement failed inside one on the server's own database, and was reported\n"));
   }
#endif
}
