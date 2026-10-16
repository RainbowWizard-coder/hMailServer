// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class DALConnection;
   class DatabaseConnectionManager;

   // The transaction a script runs its statements in: the COM Database object's,
   // from Database.BeginTransaction to CommitTransaction or RollbackTransaction, on
   // one connection the pool hands out (DatabaseConnectionManager::BeginTransaction).
   // InterfaceDatabase holds one and turns what it answers into its errors, and
   // nothing more; DatabaseTransactionTester drives this same code on every
   // database it is run against, so that what the Database object does with a
   // transaction is held by a test on Linux as on Windows.
   //
   // A statement that fails in it is the script's to answer, and the transaction
   // goes on without it - unless the database has ended the transaction at that
   // failure and goes on in autocommit, or the connection was lost under it and
   // the session the transaction was on with it
   // (DALConnection::IsTransactionEndedIntoAutocommit), where a statement run
   // after it would be committed on its own.
   // From then on every statement, script and prerequisite is refused without
   // being sent, Commit rolls back and fails, and Rollback succeeds, as
   // DatabaseTransaction runs nothing after a failure. On PostgreSQL a failure
   // aborts the transaction and leaves it open: the database refuses every later
   // statement itself until the rollback or a ROLLBACK TO SAVEPOINT, which a script
   // may send to go on, and PGConnection::CommitTransaction refuses the commit - so
   // there the statements go to the database as they always did - but for a
   // session lost, after which PostgreSQL's new session would commit them too.
   //
   // The errors are the Database object's own: each says what failed, and once
   // the transaction has ended, that it has and what to do - call
   // Database.RollbackTransaction, and run the whole transaction again.
   class ScriptTransaction
   {
   public:
      ScriptTransaction();

      // A transaction still open is rolled back, and its connection given back to
      // the pool: as the Database object's own end did.
      ~ScriptTransaction();

      // One transaction, one connection the pool holds for it: a copy would give
      // the connection back twice.
      ScriptTransaction(const ScriptTransaction &) = delete;
      ScriptTransaction &operator=(const ScriptTransaction &) = delete;

      // What became of a statement, a script or the prerequisites asked for:
      // run; run and failed - error says why, and whether the transaction has
      // ended at it; or refused without being sent, the transaction having ended
      // at an earlier failure.
      enum Outcome
      {
         Ran = 0,
         Failed = 1,
         Refused = 2
      };

      // A transaction begun on a connection the manager hands out. One open
      // already is rolled back first and its connection given back, where the
      // Database object let go of it with its transaction open, so that the pool
      // never had it back and what it held stayed locked until the server stopped.
      bool Begin(std::shared_ptr<DatabaseConnectionManager> manager, String &error);

      bool IsOpen() const { return connection_ != nullptr; }

      // The connection the transaction runs on, while it is open: for a caller that
      // reads what the transaction itself sees - DatabaseTransactionTester's.
      const std::shared_ptr<DALConnection> &GetConnection() const { return connection_; }

      // A statement, sent once (DALConnection::TryExecute), and its insert id.
      Outcome Execute(const String &statement, __int64 *insertId, String &error);

      // A script of statements (SQLScriptRunner), which stops at the first that
      // fails, and after one whose failure its [IGNORE-ERRORS] marker let through
      // when that failure has ended the transaction.
      Outcome ExecuteScript(const String &file, String &error);

      // The prerequisites of an upgrade to the database version (PrerequisiteList).
      Outcome EnsurePrerequisites(int databaseVersion, String &error);

      // Committed; or, where the database has ended the transaction, rolled back
      // and failed with nothing of it committed. The connection goes back to the
      // pool either way, and the transaction is over.
      bool Commit(String &error);

      // Rolled back. Succeeds for a transaction the database has ended already,
      // whatever the ROLLBACK is answered with: SQL Server refuses one with nothing
      // open (3903), and a session that is gone refuses it too.
      bool Rollback(String &error);

      // Whether the database has ended the transaction: so recorded, or so the
      // connection says now, which is then recorded with failure - what the
      // statement that ended it failed with.
      bool HasEnded(const String &failure = String());

   private:

      // The refusal of a statement of a transaction the database has ended, and
      // what is added to the error of the statement that ended it.
      String Refusal_() const;
      static String Advice_();

      // The connection handed back by the caller that ends the transaction, and
      // the record of it cleared.
      std::shared_ptr<DALConnection> Take_();

      std::shared_ptr<DatabaseConnectionManager> manager_;
      std::shared_ptr<DALConnection> connection_;

      // Once the database has ended the transaction at a statement in it that
      // failed: that it has, and what that statement failed with.
      bool ended_;
      String ended_by_;
   };
}
