// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "ScriptTransaction.h"

#include "DALConnection.h"
#include "DatabaseConnectionManager.h"
#include "SQLCommand.h"
#include "SQLScriptRunner.h"
#include "Prerequisites/PrerequisiteList.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   ScriptTransaction::ScriptTransaction() :
      ended_(false)
   {

   }

   ScriptTransaction::~ScriptTransaction()
   {
      try
      {
         if (connection_)
         {
            String ignored;
            Rollback(ignored);
         }
      }
      catch (...)
      {
         // A destructor may not throw. The connection stays with the pool's busy
         // set, as the Database object's did when its rollback failed, and the
         // debug log says so.
         LOG_DEBUG(_T("ScriptTransaction: a transaction still open at its end could not be rolled back."));
      }
   }

   bool
   ScriptTransaction::Begin(std::shared_ptr<DatabaseConnectionManager> manager, String &error)
   {
      if (connection_)
      {
         String ignored;
         Rollback(ignored);
      }

      manager_ = manager;
      connection_ = manager_ ? manager_->BeginTransaction(error) : nullptr;
      ended_ = false;
      ended_by_.Empty();

      return connection_ != nullptr;
   }

   bool
   ScriptTransaction::HasEnded(const String &failure)
   {
      // Not PostgreSQL's failure: it aborts the transaction there and leaves it open,
      // the database refuses every later statement by itself - nothing is committed
      // on its own - until the rollback or a ROLLBACK TO SAVEPOINT, which a script
      // may send to go on, and PGConnection::CommitTransaction refuses the commit. So
      // the script's statements go to the database as they always did. On every
      // engine, a session lost with the transaction
      // (DALConnection::IsTransactionEndedIntoAutocommit).
      if (!ended_ && connection_ && connection_->IsTransactionEndedIntoAutocommit())
      {
         ended_ = true;
         ended_by_ = failure;
      }

      return ended_;
   }

   String
   ScriptTransaction::Advice_()
   {
      return _T("The database has ended the transaction and rolled back what it held; every later statement of it is refused, and Database.CommitTransaction fails. Call Database.RollbackTransaction, and run the whole transaction again.");
   }

   String
   ScriptTransaction::Refusal_() const
   {
      if (connection_ && connection_->IsTransactionSessionLost())
         return _T("The statement was not run: the connection to the database was lost inside this transaction, and the database rolled back what it held with the session, and a statement run now would be saved on its own, outside any transaction. Call Database.RollbackTransaction, and run the whole transaction again.");

      String refusal = _T("The statement was not run: the database ended this transaction at an earlier statement in it that failed");
      if (!ended_by_.IsEmpty())
         refusal += _T(" (") + ended_by_ + _T(")");
      refusal += _T(", and rolled back what it held, and a statement run now would be saved on its own, outside any transaction. Call Database.RollbackTransaction, and run the whole transaction again.");
      return refusal;
   }

   std::shared_ptr<DALConnection>
   ScriptTransaction::Take_()
   {
      std::shared_ptr<DALConnection> connection = connection_;
      connection_.reset();
      ended_ = false;
      ended_by_.Empty();
      return connection;
   }

   ScriptTransaction::Outcome
   ScriptTransaction::Execute(const String &statement, __int64 *insertId, String &error)
   {
      if (insertId)
         *insertId = 0;

      // Not sent into a transaction the database has ended: it would be saved on
      // its own, in autocommit, on MySQL, MariaDB and SQL Server.
      if (HasEnded())
      {
         error = Refusal_();
         return Refused;
      }

      String failure;
      __int64 value = 0;
      if (connection_->TryExecute(SQLCommand(statement), failure, &value, 0) == DALConnection::DALSuccess)
      {
         // A failure the [IGNORE-ERRORS] marker answered as a success may have
         // ended the transaction all the same - a deadlock on MySQL and MariaDB,
         // a conversion error on SQL Server - and the script is told so.
         if (HasEnded(_T("a statement marked [IGNORE-ERRORS], whose failure the marker let through")))
         {
            error = _T("Execution of SQL statement failed. Error: the statement is marked [IGNORE-ERRORS], and its failure, which the marker let through, ended the transaction. ") + Advice_();
            return Failed;
         }

         if (insertId)
            *insertId = value;

         error.Empty();
         return Ran;
      }

      error = _T("Execution of SQL statement failed. Error: ") + failure;
      if (HasEnded(failure))
         error += _T(" ") + Advice_();

      return Failed;
   }

   ScriptTransaction::Outcome
   ScriptTransaction::ExecuteScript(const String &file, String &error)
   {
      if (HasEnded())
      {
         error = Refusal_();
         return Refused;
      }

      // The runner stops at the first statement that fails, and after one whose
      // failure the [IGNORE-ERRORS] marker let through when that has ended the
      // transaction (SQLScriptRunner::ExecuteScript).
      SQLScriptRunner runner;
      String failure;
      if (!runner.ExecuteScript(connection_, file, failure))
      {
         error = _T("Execution of SQL statements failed. Error: ") + failure;
         if (HasEnded(failure))
            error += _T(" ") + Advice_();

         return Failed;
      }

      // A script that ran whole on a transaction that has ended all the same - the
      // runner stops at the end, so this is a guard - is not taken for done.
      if (HasEnded(_T("a statement of the script")))
      {
         error = _T("Execution of SQL statements failed. Error: a statement of the script ended the transaction. ") + Advice_();
         return Failed;
      }

      error.Empty();
      return Ran;
   }

   ScriptTransaction::Outcome
   ScriptTransaction::EnsurePrerequisites(int databaseVersion, String &error)
   {
      if (HasEnded())
      {
         error = Refusal_();
         return Refused;
      }

      PrerequisiteList prerequisites;
      String failure;
      if (!prerequisites.Ensure(connection_, databaseVersion, failure))
      {
         error = _T("Execution of SQL statements failed. Error: ") + failure;
         if (HasEnded(failure))
            error += _T(" ") + Advice_();

         return Failed;
      }

      // Prerequisites that said they were met on a transaction that has ended all
      // the same are not taken for met: their statements are gone with it.
      if (HasEnded(_T("a statement of the prerequisites")))
      {
         error = _T("Execution of SQL statements failed. Error: a statement of the prerequisites ended the transaction. ") + Advice_();
         return Failed;
      }

      error.Empty();
      return Ran;
   }

   bool
   ScriptTransaction::Commit(String &error)
   {
      const bool ended = HasEnded();
      const bool lost = connection_ && connection_->IsTransactionSessionLost();
      const String endedBy = ended_by_;
      std::shared_ptr<DALConnection> connection = Take_();

      if (!connection)
      {
         error = _T("No transaction started");
         return false;
      }

      if (ended)
      {
         // Not committed: the database has rolled back what the transaction held,
         // and a COMMIT now would be answered with success on MySQL and MariaDB,
         // with nothing open. Rolled back, so that the connection goes back to the
         // pool holding nothing, whatever is open on it; what that answers changes
         // nothing (SQL Server refuses a ROLLBACK with nothing open, 3903).
         String ignored;
         manager_->RollbackTransaction(connection, ignored);

         if (lost)
         {
            error = _T("The transaction was rolled back, not committed: the connection to the database was lost inside it, and the database rolled back what it held with the session. Run the whole transaction again.");
            return false;
         }

         error = _T("The transaction was rolled back, not committed: the database ended it at a statement in it that failed");
         if (!endedBy.IsEmpty())
            error += _T(" (") + endedBy + _T(")");
         error += _T(", and rolled back what it held. Run the whole transaction again.");
         return false;
      }

      return manager_->CommitTransaction(connection, error);
   }

   bool
   ScriptTransaction::Rollback(String &error)
   {
      const bool ended = HasEnded();
      std::shared_ptr<DALConnection> connection = Take_();

      if (!connection)
      {
         error = _T("No transaction started");
         return false;
      }

      // A transaction the database has ended is rolled back already, which is what
      // the script asked for, whatever the ROLLBACK is answered with: SQL Server
      // refuses one with nothing open (3903), and a lost connection refuses it too.
      if (!manager_->RollbackTransaction(connection, error) && !ended)
         return false;

      error.Empty();
      return true;
   }
}
