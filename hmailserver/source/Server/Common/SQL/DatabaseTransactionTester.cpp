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

#ifndef HM_PLATFORM_POSIX
#include "SQLCEConnection.h"
#include "../Application/IniFileSettings.h"
#include "../Util/FileUtilities.h"
#include "../Util/GUIDCreator.h"
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

            // TryExecute: nothing reported when there is nothing to drop.
            String ignored;
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

         // The session lost in the middle of a unit of work: nothing of the attempt is
         // committed, and Run's next attempt commits it once. The control is what
         // DALConnection::Execute does on a transaction the pool hands out: it
         // reconnects and runs the statement again on the new session, outside the
         // transaction, where it commits on its own (ActiveActive.md, C17).
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

            probe.Reset();
            String error;
            std::shared_ptr<DALConnection> connection = pool->BeginTransaction(error);
            Check(connection != nullptr, _T("the pool's BeginTransaction failed: ") + error);
            Check(connection->Execute(Insert(20, 1), error), _T("the control's first insert failed: ") + error);
            probe.EndSessionOf(connection);
            const bool second = connection->Execute(Insert(21, 1), error);
            pool->RollbackTransaction(connection, error);
            Check(probe.Value(_T("aatxnprobe"), 20) == -1, _T("the control's first insert, made before its session ended, was committed"));
            if (second || !mysql)
            {
               Check(second && probe.Value(_T("aatxnprobe"), 21) == 1,
                  _T("the control did not show a statement run again after a reconnect committing outside its transaction"));
            }
            else
            {
               // MySQL and MariaDB over TLS answer a statement on a session the
               // server ended with the TLS layer's end of file (2026), which
               // MySQLConnection::CheckError does not take for a lost connection, so
               // DALConnection::Execute did not reconnect and did not run it again:
               // C17 does not show on such a session, and does over a plain one
               // (2013).
               Check(probe.Value(_T("aatxnprobe"), 21) == -1, _T("the control's second insert failed and was committed all the same"));
               probe.Say(_T("(the control: DALConnection::Execute took the TLS end of file of a session MySQL ended for a failed statement, not a lost connection, and did not run it again)"));
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
               Check(unknown.outcome == DatabaseTransaction::CommitUnknown && runs == 1, _T("Run ran a unit of work again after its COMMIT's connection was lost"));
               Check(probe.Value(_T("aatxnprobe"), 31) == -1, _T("the unit of work whose session ended before its COMMIT was committed"));
            }

            // The pool's connections were opened again by the scopes that lost them:
            // a statement through the manager works on each.
            for (int i = 0; i < 4; i++)
               Check(pool->Execute(Update(_T("aatxnprobeb"), 1)), _T("a pool connection a transaction lost was not opened again"));

            probe.Say(String(_T("a session lost in the middle: nothing committed and the retry commits once")) +
               (second ? _T(", where DALConnection::Execute commits the next statement on its own") : _T("")) +
               _T("; a lost COMMIT is not run again"));
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
               Check(transaction.Execute(Insert(40, 1)), _T("an insert on a database that is not transactional failed"));
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
#endif
}
