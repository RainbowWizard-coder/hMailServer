// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <functional>
#include <thread>

#include "DALConnection.h"

namespace HM
{
   class DatabaseConnectionManager;
   class DALRecordset;
   class SQLCommand;
   class SQLStatement;

   /*
      A database transaction the persistence layer can use: one connection of the
      pool, taken for as long as the scope lives, with every statement of the unit
      of work run on it - ActiveActive.md, unit AA-02.

      Without it nothing inside the server could be one transaction: each
      DatabaseConnectionManager::Execute and OpenRecordset borrows a connection of
      its own from the pool and gives it back, so a transaction begun on one
      connection covered none of the statements a save makes. The scope fixes that
      by being its thread's: while it is open, an Execute or an OpenRecordset made
      through the same manager ON THE THREAD THAT OPENED IT runs on the scope's
      connection, inside the transaction. That is how the existing saves -
      PersistentMessage::AddObject, the folder counters, an administrative save -
      are put inside one transaction without each being rewritten. Another
      thread's statements go to the pool as before: a scope is never shared
      between threads, and a call made on it from another thread is refused.

      Opened inside another scope of the same manager on the same thread, a scope
      JOINS it rather than beginning a second transaction: its statements run in
      the outer one, its Commit commits nothing (the outer scope's does), and a
      joined scope that ends without Commit makes the whole transaction roll back,
      since what it wrote cannot be taken back alone. A Suspension hides the
      thread's scopes for its life, so that work which must not be part of the
      unit - an OnError event script run while one of its statements is being
      reported - goes to the pool as it always did.

      NO STATEMENT INSIDE A SCOPE IS RUN AGAIN - except on a MySQL database that
      is not transactional, below. DALConnection::Execute and
      DALRecordset::Open run a statement again after a reconnect, and on a new
      session a statement is outside the transaction: it commits on its own, while
      everything before it was rolled back with the session that was lost. So a
      statement here is sent once (DALConnection::TryExecute, DALRecordset::TryOpen),
      and the first failure fails the scope: every later statement in it is
      refused without being sent, and the commit is refused. The engines differ in
      what a failure leaves - SQL Server and MySQL roll the whole transaction back
      at a deadlock and go on in autocommit, SQL Server does so at a conversion
      error too, PostgreSQL refuses everything until the rollback, a MySQL lock
      wait that timed out takes back only its own statement - and refusing
      everything after the first failure is the one rule that is right on all
      four.

      What is retried is the whole unit of work, by Run: a bounded number of
      times, on a lost connection and on the engines' deadlock and serialisation
      failures (SQL Server 1205; MySQL and MariaDB 1213, 1205 and 1020; PostgreSQL
      40001 and 40P01; SQL Server Compact's lock timeout 25090), rolled back before
      each new attempt - on MySQL a lock wait that timed out took back only its own
      statement. A connection lost during the COMMIT is never retried: the commit
      may have reached the database with only its answer lost, and running the
      unit again would apply it twice. Run says so instead (CommitUnknown), and the
      caller reads back.

      A failure the caller asked to ignore - DALConnection::DALErrorInSQL, "this
      row is there already", which several inserts pass - is ignored here only
      when it is that: a primary key's or a unique index's duplicate the backend
      names (DALConnection::ClassifyLastFailure: SQL Server 2627 and 2601, MySQL
      and MariaDB 1062, PostgreSQL 23505, SQL Server Compact 25016). Outside a
      scope the backends' own ignoring is wider: on SQL Server it lets through
      every integrity violation - an insert a foreign key refused for want of the
      row it refers to (547), a NULL into a NOT NULL column (515), a CHECK
      constraint (547) - and on SQL Server Compact a foreign key's refusal
      (25026 for a row whose parent is not there, 25025 for a delete of a row a
      foreign key refers to; a NULL into a NOT NULL column is an error in the
      command there, 25005, and a CHECK cannot be declared). Since unit AA-01 it
      lets through nothing else on any backend (DALConnection::TryExecute): no
      unknown error, and on MySQL no lost connection, as it did before. Inside a
      transaction that would commit a unit without a row the database refused. On
      PostgreSQL, where any failed statement aborts the transaction, a statement
      that may be ignored runs under a savepoint, so that an ignored duplicate
      leaves the transaction as it was. A statement marked [IGNORE-ERRORS], whose
      failure the backend itself answers as a success, fails the scope when that
      failure has ended the transaction (DALConnection::IsTransactionFailed): on
      SQL Server a conversion error rolls the transaction back.

      On a MySQL database with a table that is not InnoDB nothing is
      transactional (MySQLConnection::LoadSupportsTransactions_): the scope still
      runs the unit on one connection, IsTransactional() says false, a rollback
      takes nothing back, and Run does not retry a unit it cannot undo. Its
      statements run as the pool runs them (DALConnection::Execute,
      DALRecordset::Open), again after a reconnect, with the backend's own
      ignoring. One node is still served as it was before transactions, where
      nothing was atomic.

      What it reports. A statement that fails for a reason a retry would not mend
      is reported as the pool reports it (HM5032, HM5033), and a BEGIN or a COMMIT
      the database refuses for such a reason as HM8502. A deadlock, a
      serialisation failure, a lock wait or a lost connection is not: inside Run
      it is retried, and only giving up is reported (HM8500); in a scope of its
      own, its caller is told and says what failed in its own words. Run reports a
      commit whose outcome is not known (HM8501). Each attempt Run makes again is
      a line of the application log.

      Two rules for callers. Take any lock of the process (FolderManipulationLock,
      a mutex) BEFORE opening a scope, never inside one: a thread that holds row
      locks in its transaction and waits for a mutex can wait for ever on a thread
      that holds the mutex and waits for one of those rows, and neither the
      database's deadlock detector nor the process can see the cycle. And keep the
      unit short and free of anything but the database: a scope holds a pooled
      connection and its locks until it ends, and the work Run is given may run
      more than once, so it must take its inputs afresh each time and change
      nothing outside the database until Run returns.

      On SQL Server Compact the pool holds one connection (IniFileSettings), so
      an open scope holds the only one the server has: every other thread's
      statement waits for it, for DBConnectionAcquireTimeout at most. A scope
      that fails gives its connection back at once, before the failure is
      reported; and a statement its own thread makes outside it while it is
      open - under a Suspension - is refused at once (HM8503) when this thread's
      scopes hold every connection the pool has lent out, since a wait could
      only end when the thread gave one back.
   */
   class DatabaseTransaction
   {
   public:

      // Begins a transaction on a connection of the manager's pool and makes it
      // this thread's scope for that manager - or, inside an open scope of the
      // same manager on this thread, joins that one. IsOpen() says whether it
      // can be used.
      explicit DatabaseTransaction(std::shared_ptr<DatabaseConnectionManager> manager);

      // Rolls back a transaction that was not committed, and gives the connection
      // back to the pool. A joined scope ended without Commit makes the
      // transaction it joined roll back as well.
      ~DatabaseTransaction();

      DatabaseTransaction(const DatabaseTransaction &) = delete;
      DatabaseTransaction &operator=(const DatabaseTransaction &) = delete;

      // Whether statements can be run in it: the transaction began (or the one
      // joined is open), nothing has failed in it, and it has not ended.
      bool IsOpen() const;

      // Whether it joined a scope opened before it on this thread.
      bool IsJoined() const;

      // Whether a rollback takes back what was written: false only on a MySQL
      // database with a table that is not InnoDB.
      bool IsTransactional() const;

      // A statement on the transaction's connection, sent once and not run again
      // after a reconnect - except on a MySQL database that is not transactional,
      // where it runs as the pool runs it (above). Otherwise the same as
      // DatabaseConnectionManager::Execute, and recorded by the audit trail in the
      // same way; a failure fails the scope. iIgnoreErrors may be
      // DALConnection::DALErrorInSQL, for an insert whose row may be there
      // already: then such a failure is not one, and the insert id is 0.
      // rowsAffected, when given, is the count DatabaseConnectionManager::
      // ExecuteCount gives (DALConnection::TryExecute): the rows the statement
      // matched, 0 for a failure that was not one, -1 when it failed.
      bool Execute(const SQLCommand &command, __int64 *iInsertID = 0, int iIgnoreErrors = 0, __int64 *rowsAffected = nullptr);
      bool Execute(const SQLStatement &statement, __int64 *iInsertID = 0, int iIgnoreErrors = 0, __int64 *rowsAffected = nullptr);

      // A query on the transaction's connection, once - or, on a MySQL database
      // that is not transactional, as the pool runs it. Empty when it failed, which
      // fails the scope and marks the thread as DatabaseUnavailableMarker says, so
      // that "the database did not answer" is not read as "nothing was found".
      std::shared_ptr<DALRecordset> OpenRecordset(const SQLCommand &command);
      std::shared_ptr<DALRecordset> OpenRecordset(const SQLStatement &statement);

      // Commits the transaction and gives the connection back; false, with the
      // transaction rolled back, when anything in it failed or a joined scope
      // ended without its own Commit. A joined scope's Commit commits nothing: it
      // says that its part is done, and the outermost scope's Commit decides.
      bool Commit();

      // Rolls the transaction back now and gives the connection back. Of a joined
      // scope, makes the transaction it joined roll back.
      void Rollback();

      // What ended the transaction, when something did.
      enum Failure
      {
         FailureNone = 0,

         // A statement failed for a reason a retry would not mend.
         FailureStatement = 1,

         // A deadlock, a serialisation failure or a lock wait that timed out.
         FailureRetryable = 2,

         // The connection was lost before the COMMIT was sent: nothing of the
         // unit was committed.
         FailureConnection = 3,

         // The connection was lost during the COMMIT: whether the database
         // committed it is not known.
         FailureCommitUnknown = 4,

         // A joined scope ended without Commit, or Rollback was asked for.
         FailureAbandoned = 5,

         // The pool had no connection to give - it waited for one as long as
         // DBConnectionAcquireTimeout lets it, or holds none - so the transaction
         // was not begun.
         FailureUnavailable = 6
      };

      Failure GetFailure() const;
      String GetErrorMessage() const;

      // What Run made of a unit of work.
      enum Outcome
      {
         // The work returned true and the transaction committed. Of a unit run
         // inside another scope (joined), that its part is done: the outer
         // transaction commits it or not.
         Committed = 0,

         // The work returned false, or a statement failed for a reason a retry
         // would not mend, or the database is not transactional and the attempt
         // failed: rolled back, not retried. Also when the pool had no connection
         // to give: it has waited for one already.
         Failed = 1,

         // Every attempt ended in a deadlock, a serialisation failure, a lock
         // wait that timed out or a lost connection (HM8500).
         GaveUp = 2,

         // The connection was lost during the COMMIT, so the unit may or may not
         // have been committed (HM8501). Not retried: read back.
         CommitUnknown = 3
      };

      struct RunResult
      {
         RunResult() : outcome(Failed), attempts(0) {}

         Outcome outcome;
         int attempts;
         String error;
      };

      // The work of one attempt: its statements, through the transaction or
      // through the manager on this thread. True to commit, false to roll back.
      typedef std::function<bool(DatabaseTransaction &transaction)> Work;

      static const int DefaultAttempts = 5;

      // Runs the work in a transaction of its own, again from the start after a
      // deadlock, a serialisation failure, a lock wait that timed out or a
      // connection lost before the COMMIT, up to attempts times. Inside an open
      // scope of the same manager on this thread, runs it once, joined: a
      // failure there fails the outer transaction, and the outer Run retries the
      // whole of it.
      static RunResult Run(std::shared_ptr<DatabaseConnectionManager> manager, const Work &work, int attempts = DefaultAttempts);

      // Hides this thread's scopes for its life: statements made on the thread
      // meanwhile go to the pool, as they would with no scope open. Scopes opened
      // inside it are transactions of their own.
      class Suspension
      {
      public:
         Suspension();
         ~Suspension();

         Suspension(const Suspension &) = delete;
         Suspension &operator=(const Suspension &) = delete;

      private:
         friend class DatabaseTransaction;

         DatabaseTransaction *hidden_;

         // The Suspension this one was made inside, or none.
         Suspension *enclosing_;
      };

   private:

      friend class DatabaseConnectionManager;
      friend class DatabaseConnectionManagerTester;
      friend class DatabaseTransactionTester;

      void Open_();

      // The innermost scope of this manager on this thread, open or not, or none.
      static DatabaseTransaction *Current_(const DatabaseConnectionManager *manager);

      // How many of the manager's connections this thread's scopes hold, the ones
      // a Suspension hides included: DatabaseConnectionManager::GetConnection_
      // does not wait for a connection when these are all it has lent out, since
      // only this thread could give one back.
      static size_t ConnectionsHeldOnThisThread_(const DatabaseConnectionManager *manager);

      // Whether the manager's statements on this thread are to run here: a joined
      // scope's always are (and are refused once what it joined has ended); an
      // outermost scope's while its transaction is open.
      bool Routes_() const;

      // The statement and the query, once, for Execute and for the manager's
      // routing. The audit trail's record is the manager's
      // (DatabaseConnectionManager::ExecuteInTransaction_). rowsAffected as
      // Execute says it; reconnected, when given, whether the statement was run
      // again after a reconnect, which only a scope on a MySQL database that is not
      // transactional does (DatabaseConnectionManager::ExecuteCount).
      bool Execute_(const SQLCommand &command, __int64 *iInsertID, int iIgnoreErrors, String &sErrorMessage,
                    __int64 *rowsAffected = nullptr, bool *reconnected = nullptr);
      std::shared_ptr<DALRecordset> OpenRecordset_(const SQLCommand &command);

      // Whether a statement may be sent now, and why not when it may not.
      bool Usable_(String &sErrorMessage) const;

      // One statement of the scope's own (a savepoint's), on the connection.
      bool RunControl_(const String &sql, String &sErrorMessage);

      // The first failure, on the outermost scope: recorded, the transaction
      // rolled back at once so that its locks are free, and the connection given
      // back to the pool before the failure is reported. The scope stays its
      // thread's and refuses every statement until it ends.
      void Fail_(Failure failure, const String &message);

      // A ROLLBACK of what is still open, and a reconnect at release when the
      // database refused it.
      void RollBack_();

      // Rolls back what is still open, reconnects a session that was lost or may
      // still hold a transaction, and gives the connection back to the pool.
      void Release_();

      // Release_, and the end of the scope's routing: the thread's statements go
      // to the pool from here on.
      void End_();

      std::shared_ptr<DatabaseConnectionManager> manager_;

      // The outermost scope this one joined, or this scope.
      DatabaseTransaction *root_;

      // The thread's scope before this one (of any manager), so that scopes
      // nest; restored when this one ends.
      DatabaseTransaction *previous_;
      bool linked_;

      std::thread::id thread_;

      // A joined scope's own: whether it said its part was done.
      bool committed_;

      // The outermost scope's: the transaction. ended_ once Commit, Rollback or
      // the destructor has ended it; in_transaction_ while a BEGIN is
      // outstanding on the connection.
      std::shared_ptr<DALConnection> connection_;
      bool ended_;
      bool in_transaction_;
      bool transactional_;
      bool reconnect_on_release_;
      bool rollback_only_;
      int joined_open_;
      Failure failure_;
      String error_;

      static thread_local DatabaseTransaction *current_;

      // The innermost Suspension on this thread, or none.
      static thread_local Suspension *suspension_;
   };

   // The scope held to what it promises, on a real database, through
   // DatabaseConnectionManager and DatabaseTransaction: a rollback discards; the
   // manager's own Execute joins the transaction and a Suspension does not, and its
   // ExecuteCount joins it too and counts the transaction's run of the statement; two
   // scopes on one row wait for each other; a deadlock between two units of work
   // is retried by Run and both commit once each, where scopes alone lose one; a
   // connection lost in the middle of a scope commits nothing and the retry
   // commits once, where DALConnection::Execute's reconnect commits the
   // statement after it on its own (the negative control, C17); a failed
   // statement fails the scope and refuses the rest; scopes join and nest; an
   // insert a foreign key refused and a NULL into a NOT NULL column are not taken
   // for a row that is there already, with DALErrorInSQL or without; a statement
   // marked [IGNORE-ERRORS] that failed leaves the unit committed whole or not at
   // all (on SQL Server, where a conversion error rolls the transaction back, not
   // at all), and one that met a row there already, which leaves the transaction
   // open on every engine, committed whole; on a pool of one connection a failed
   // scope's connection is there for a statement made outside it and an open
   // scope's is not waited for, and on a pool of two such a statement waits for
   // the connection another thread gives back. PostgreSQL also: an ignored
   // duplicate leaves the transaction usable, and a COMMIT that PostgreSQL
   // turned into a ROLLBACK is reported as a failure. MySQL also: a database
   // with a table that is not InnoDB reports that it is not transactional, and
   // on MariaDB a unit of work that its error 4031 failed - a statement's own
   // refusal there, an idle session closed on MySQL - is failed once and not run
   // again.
   //
   // On pools and tables of its own (aatxnprobe, aatxnprobeb, aatxnprobec, and on
   // MySQL aatxnprobem), which it makes, empties and removes, so it can be run
   // against a database a server has made before the server starts on it; never
   // against one a server is using as a node of a set. On Windows ClassTester runs
   // TestCompact, on a SQL Server Compact database of the tester's own in the temp
   // folder - the Windows gate's engine. On Linux the program
   // database-transaction-tests runs Test against the database its configuration
   // file names: PostgreSQL in the Linux suite, MySQL and MariaDB in the database
   // matrix. SQL Server's is run on a Windows node whose own database it is
   // (ActiveActive.md, "SQL Server"): Test against that database, through the
   // server's ADO code, with a login that may end a session there (KILL needs
   // ALTER ANY CONNECTION, and seeing it gone VIEW SERVER STATE). It says what
   // failed by throwing std::logic_error with a sentence.
   class DatabaseTransactionTester
   {
   public:
      typedef std::function<void(const String &line)> Report;

      // Every check on the database the settings name. provokeErrors: whether a
      // check may run a statement that fails for a reason a retry would not mend,
      // which is reported to the error log as any such statement is (HM5032) -
      // not inside the running server, whose error log the fixture that runs
      // ClassTester asserts empty.
      void Test(std::shared_ptr<DatabaseSettings> settings, Report report, bool provokeErrors);

      // A pool of the tester's own, of this many connections to the database the
      // settings name, beside the server's.
      static std::shared_ptr<DatabaseConnectionManager> OpenPool(std::shared_ptr<DatabaseSettings> settings, int connections);

#ifndef HM_PLATFORM_POSIX
      // ClassTester's: a SQL Server Compact database of the tester's own, made
      // in the temp folder and removed again.
      void TestCompact();

      // ClassTester's, and only while event scripts run: a statement that fails
      // inside a transaction on the server's own database, so that the OnError
      // script a regression test installs runs while it is reported
      // (ErrorManager runs it under a Suspension) - the test finds what the
      // script saved committed. With scripting off it does nothing.
      void TestOnErrorOnTheServersDatabase();
#endif

   private:

      static void Fail_(const String &what);
   };
}
