// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "DatabaseTransaction.h"

#include "DALConnection.h"
#include "DALRecordset.h"
#include "DatabaseConnectionManager.h"
#include "DatabaseUnavailableMarker.h"
#include "SQLCommand.h"
#include "SQLStatement.h"

#include <chrono>
#include <random>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   thread_local DatabaseTransaction *DatabaseTransaction::current_ = nullptr;
   thread_local DatabaseTransaction::Suspension *DatabaseTransaction::suspension_ = nullptr;

   namespace
   {
      // The savepoint a statement that may be ignored runs under on PostgreSQL
      // (Execute_). One name: the statements of a scope run one at a time, and
      // each savepoint is released before the next statement.
      const TCHAR *const SetSavepoint = _T("SAVEPOINT hm_transaction_ignore");
      const TCHAR *const UndoToSavepoint = _T("ROLLBACK TO SAVEPOINT hm_transaction_ignore");
      const TCHAR *const ReleaseSavepoint = _T("RELEASE SAVEPOINT hm_transaction_ignore");

      unsigned __int64
      MicrosecondsSince(std::chrono::steady_clock::time_point started)
      {
         return (unsigned __int64) std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
      }

      // A statement's parameters written into its text, for the two backends that
      // take none (MySQL, PostgreSQL), as DALConnection::Execute and
      // DALRecordset::Open do before they hand a statement to the backend.
      SQLCommand
      AsTheConnectionTakesIt(const std::shared_ptr<DALConnection> &connection, const SQLCommand &command)
      {
         SQLCommand copy = command;

         if (!connection->GetSupportsCommandParameters() && command.GetParameters().size() > 0)
         {
            SQLStatement statement;
            String text = statement.GenerateFromCommand(command);

            copy.ClearParameters();
            copy.SetQueryString(text);
         }

         return copy;
      }

      // The pause before the next attempt of a unit of work. After a deadlock or a
      // serialisation failure a little, and growing, with a random part so that
      // the two units that met do not meet again at the same moment; after a lost
      // connection a second, as DALConnection::Execute waits before it reconnects.
      int
      PauseBeforeAttempt(int attempt, bool connectionLost)
      {
         if (connectionLost)
            return 1000;

         thread_local std::mt19937 generator((unsigned int) std::chrono::steady_clock::now().time_since_epoch().count() ^
                                             (unsigned int) std::hash<std::thread::id>()(std::this_thread::get_id()));
         std::uniform_int_distribution<int> spread(10, 50);
         return spread(generator) * (attempt - 1);
      }
   }

   DatabaseTransaction::DatabaseTransaction(std::shared_ptr<DatabaseConnectionManager> manager) :
      manager_(manager),
      root_(this),
      previous_(nullptr),
      linked_(false),
      thread_(std::this_thread::get_id()),
      committed_(false),
      ended_(false),
      in_transaction_(false),
      transactional_(true),
      reconnect_on_release_(false),
      rollback_only_(false),
      joined_open_(0),
      failure_(FailureNone)
   {
      try
      {
         Open_();
      }
      catch (...)
      {
         // A constructor that throws has no destructor run, and Open_ has by then
         // put this scope on the thread's chain and may hold a pooled connection:
         // a stop's interruption met at the wait for one (GetConnection_), or
         // passed on through a report made inside the BEGIN or the reconnect
         // (see WorkQueue::Stop), left the chain pointing at a scope that no longer
         // existed - read by every later statement of the thread's
         // (DatabaseTransaction::Current_) - and the connection out of the pool for
         // good. Undone here, interruption held off, and the exception passed on.
         // No BEGIN is open on the connection: each report it can come from is made
         // before the BEGIN is sent or on a session the reconnect has just made or
         // closed.
         boost::this_thread::disable_interruption disabled;

         if (root_ != this)
         {
            root_->joined_open_--;
         }
         else if (connection_)
         {
            try
            {
               Release_();
            }
            catch (...)
            {
               if (connection_)
               {
                  manager_->ReleaseConnection_(connection_);
                  connection_.reset();
               }
            }
         }

         if (linked_ && current_ == this)
         {
            current_ = previous_;
            linked_ = false;
         }

         throw;
      }
   }

   void
   DatabaseTransaction::Open_()
   {
      if (!manager_)
      {
         // Nothing to route to and nothing to begin. Not linked, so that nothing
         // is refused on the thread's behalf either; IsOpen says false.
         ended_ = true;
         failure_ = FailureConnection;
         error_ = _T("There is no database connection manager to begin a transaction on.");
         return;
      }

      // Joined: an open scope of the same manager is already this thread's. Its
      // transaction is the one the statements belong in, whether or not it has
      // failed - a scope begun beside a failed one would commit on its own what
      // the caller means as part of the failed unit.
      DatabaseTransaction *outer = Current_(manager_.get());
      if (outer != nullptr && !outer->root_->ended_)
      {
         root_ = outer->root_;
         root_->joined_open_++;

         previous_ = current_;
         current_ = this;
         linked_ = true;
         return;
      }

      // Linked before anything can fail, so that a scope which could not begin
      // still routes the thread's statements here and refuses them, rather than
      // letting them commit one by one on the pool's connections.
      previous_ = current_;
      current_ = this;
      linked_ = true;

      connection_ = manager_->GetConnection_();
      if (!connection_)
      {
         // GetConnection_ has reported a pool that timed out, and marked the
         // thread (DatabaseUnavailableMarker). Not a lost connection, which Run
         // tries again: the pool has waited for one already, as long as
         // DBConnectionAcquireTimeout lets it, and a unit that waited again each
         // time would hold whatever its caller holds for minutes.
         failure_ = FailureUnavailable;
         error_ = _T("No database connection could be had from the pool, so the transaction was not begun.");
         return;
      }

      connection_->ForgetLastFailure();
      transactional_ = connection_->GetSupportsTransactions();

      String error;
      bool begun = connection_->BeginTransaction(error);
      DALConnection::LastFailure kind = DALConnection::LastFailureOther;

      if (!begun)
      {
         kind = connection_->ClassifyLastFailure(DALConnection::DALUnknown);

         if (kind == DALConnection::LastFailureConnection)
         {
            // The pooled connection was lost before this unit used it: a database
            // that restarted, a session it ended. Nothing of the unit has been run
            // on it, so one reconnect and a second BEGIN run nothing twice - what
            // DALConnection::Execute's own retry does for a single statement.
            String reconnectError;
            if (connection_->Reconnect(reconnectError))
            {
               connection_->ForgetLastFailure();
               transactional_ = connection_->GetSupportsTransactions();
               begun = connection_->BeginTransaction(error);

               if (!begun)
                  kind = connection_->ClassifyLastFailure(DALConnection::DALUnknown);
            }
            else if (!reconnectError.IsEmpty())
            {
               error = reconnectError;
            }
         }
      }

      if (!begun)
      {
         switch (kind)
         {
         case DALConnection::LastFailureConnection:
            failure_ = FailureConnection;
            reconnect_on_release_ = true;
            break;
         case DALConnection::LastFailureRetryable:
            failure_ = FailureRetryable;
            break;
         default:
            failure_ = FailureStatement;
            break;
         }

         error_ = _T("The transaction could not be begun: ") + error;

         // The connection goes back now; the scope stays its thread's, and refuses
         // the unit's statements until it ends.
         Release_();

         if (failure_ == FailureStatement)
            ErrorManager::Instance()->ReportError(ErrorManager::High, 8502, "DatabaseTransaction::DatabaseTransaction", error_);

         return;
      }

      in_transaction_ = true;
   }

   DatabaseTransaction::~DatabaseTransaction()
   {
      // A scope ends on the thread that made it, where its place in the thread's
      // chain is.
      HM_ASSERT(std::this_thread::get_id() == thread_);

      // Interruption held off, as a destructor cannot pass a stop's interruption on
      // (WorkQueue::Stop): the rollback, a reconnect the scope asks for and the
      // connection's return to the pool complete, rather than end at a report
      // whose OnError script's call met the stop - which the catch below would
      // take, with the connection still out of the pool. A request made meanwhile
      // stays pending for the next interruption point after the scope.
      boost::this_thread::disable_interruption disabled;

      try
      {
         if (root_ != this)
         {
            // A joined scope that ended without saying its part was done: what it
            // wrote is in the transaction and cannot be taken back alone.
            if (!committed_)
            {
               root_->rollback_only_ = true;

               if (root_->failure_ == FailureNone && !root_->ended_)
               {
                  root_->failure_ = FailureAbandoned;
                  root_->error_ = _T("A unit of work joined to this transaction ended without its part being done, so the transaction is rolled back.");
               }
            }

            root_->joined_open_--;
         }
         else
         {
            HM_ASSERT(joined_open_ == 0);

            if (!ended_)
               End_();
         }
      }
      catch (...)
      {
         // A destructor that may not throw. The backends catch their own
         // failures; whatever escapes them is said, and the thread's chain is
         // mended below.
         LOG_DEBUG(_T("DatabaseTransaction: ending a transaction threw; its connection may not have gone back to the pool."));
      }

      if (linked_)
      {
         if (current_ == this)
         {
            current_ = previous_;
         }
         else
         {
            // Not the innermost: a scope destroyed out of the order it was made
            // in, which a scope on the stack never is. Taken out of the chain
            // wherever it is, so that the thread's chain stays whole.
            HM_ASSERT(0);

            for (DatabaseTransaction *scope = current_; scope != nullptr; scope = scope->previous_)
            {
               if (scope->previous_ == this)
               {
                  scope->previous_ = previous_;
                  break;
               }
            }
         }

         linked_ = false;
      }
   }

   DatabaseTransaction *
   DatabaseTransaction::Current_(const DatabaseConnectionManager *manager)
   {
      for (DatabaseTransaction *scope = current_; scope != nullptr; scope = scope->previous_)
      {
         if (scope->manager_.get() == manager)
            return scope;
      }

      return nullptr;
   }

   bool
   DatabaseTransaction::Routes_() const
   {
      // A joined scope's statements belong in the transaction it joined, and are
      // refused rather than sent to the pool once that has ended.
      if (root_ != this)
         return true;

      return !ended_;
   }

   bool
   DatabaseTransaction::IsOpen() const
   {
      if (std::this_thread::get_id() != thread_)
         return false;

      const DatabaseTransaction *root = root_;
      return !root->ended_ && root->in_transaction_ && root->failure_ == FailureNone && !root->rollback_only_;
   }

   bool
   DatabaseTransaction::IsJoined() const
   {
      return root_ != this;
   }

   bool
   DatabaseTransaction::IsTransactional() const
   {
      return root_->transactional_;
   }

   DatabaseTransaction::Failure
   DatabaseTransaction::GetFailure() const
   {
      return root_->failure_;
   }

   String
   DatabaseTransaction::GetErrorMessage() const
   {
      return root_->error_;
   }

   bool
   DatabaseTransaction::Usable_(String &sErrorMessage) const
   {
      if (std::this_thread::get_id() != thread_)
      {
         // A scope is its thread's. Two threads on one connection would interleave
         // their statements in one transaction, and neither would know.
         HM_ASSERT(0);
         sErrorMessage = _T("A database transaction was used from a thread other than the one that began it, and the statement was not run.");
         return false;
      }

      const DatabaseTransaction *root = root_;

      if (root->ended_)
      {
         sErrorMessage = _T("The database transaction this statement belongs to has ended, so the statement was not run.");
         return false;
      }

      if (root->failure_ != FailureNone)
      {
         sErrorMessage = _T("The database transaction this statement belongs to has already failed, so the statement was not run: ") + root->error_;
         return false;
      }

      if (!root->connection_ || !root->in_transaction_)
      {
         sErrorMessage = _T("The database transaction this statement belongs to was never begun, so the statement was not run.");
         return false;
      }

      return true;
   }

   bool
   DatabaseTransaction::Execute(const SQLCommand &command, __int64 *iInsertID, int iIgnoreErrors, __int64 *rowsAffected)
   {
      if (rowsAffected != nullptr)
         *rowsAffected = -1;

      String discarded;
      return manager_ ? manager_->ExecuteInTransaction_(*this, command, iInsertID, iIgnoreErrors, discarded, rowsAffected, nullptr) : false;
   }

   bool
   DatabaseTransaction::Execute(const SQLStatement &statement, __int64 *iInsertID, int iIgnoreErrors, __int64 *rowsAffected)
   {
      return Execute(statement.GetCommand(), iInsertID, iIgnoreErrors, rowsAffected);
   }

   std::shared_ptr<DALRecordset>
   DatabaseTransaction::OpenRecordset(const SQLCommand &command)
   {
      return OpenRecordset_(command);
   }

   std::shared_ptr<DALRecordset>
   DatabaseTransaction::OpenRecordset(const SQLStatement &statement)
   {
      return OpenRecordset_(statement.GetCommand());
   }

   bool
   DatabaseTransaction::Execute_(const SQLCommand &command, __int64 *iInsertID, int iIgnoreErrors, String &sErrorMessage,
                                 __int64 *rowsAffected, bool *reconnected)
   {
      if (iInsertID != nullptr)
         *iInsertID = 0;

      // Until the statement has run and what it did is known: a failure counts -1
      // (DALConnection::TryExecute), and nothing has been run again.
      if (rowsAffected != nullptr)
         *rowsAffected = -1;

      if (reconnected != nullptr)
         *reconnected = false;

      if (!Usable_(sErrorMessage))
      {
         LOG_DEBUG(_T("DatabaseTransaction: ") + sErrorMessage);
         return false;
      }

      DatabaseTransaction *root = root_;

      // The same fault injection as the pool's statements, and answered the same
      // way: a statement the database refused, reported by nobody. It fails the
      // transaction, which is what a refused statement inside one does.
      if (DatabaseConnectionManager::SimulateFailureFor_(command))
      {
         sErrorMessage = _T("Simulated database failure ([Settings] SimulateDatabaseFailureFor).");
         root->Fail_(FailureStatement, sErrorMessage);
         return false;
      }

      std::shared_ptr<DALConnection> connection = root->connection_;

      if (!root->transactional_)
      {
         // A MySQL database with a table that is not InnoDB, where nothing is a
         // transaction: the statement runs as the pool runs it - again after a
         // reconnect, with the backend's own ignoring, and reported when it fails
         // (HM5032) - which is what one node has always done on such a database.
         // Only the first failure still ends the unit.
         std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

         String message;
         __int64 counted = -1;
         const bool executed = connection->Execute(command, message, iInsertID, iIgnoreErrors, &counted, reconnected);

         manager_->MeasureQuery_(command, MicrosecondsSince(started));

         if (executed)
         {
            if (rowsAffected != nullptr)
               *rowsAffected = counted;

            sErrorMessage = _T("");
            return true;
         }

         sErrorMessage = message;
         root->Fail_(FailureStatement, message);
         return false;
      }

      const SQLCommand toRun = AsTheConnectionTakesIt(connection, command);

      // Whether a failure of this statement may be one the caller does not count
      // as a failure: a row that is there already, or a statement marked
      // [IGNORE-ERRORS], which the backend itself answers as a success. On
      // PostgreSQL either aborts the transaction, so the statement runs under a
      // savepoint that is rolled back to when it failed.
      const bool ignoreDuplicate = (iIgnoreErrors & DALConnection::DALErrorInSQL) != 0;
      const bool marked = DALConnection::HasIgnoreErrorsMarker(command.GetQueryString(),
                                                               connection->GetBackslashEscapesInLiterals());
      const bool savepoint = root->transactional_ && connection->GetFailedStatementEndsTransaction() &&
                             (ignoreDuplicate || marked);

      if (savepoint && !RunControl_(SetSavepoint, sErrorMessage))
         return false;

      connection->ForgetLastFailure();

      String message;
      __int64 counted = -1;
      std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

      // Once, and with no failure ignored by the backend: its own ignoring lets
      // through more than a row that is there already - every integrity violation
      // on SQL Server, a foreign key's refusal on SQL Server Compact
      // (DatabaseTransaction.h). What this caller may ignore is decided below,
      // from what the failure was.
      DALConnection::ExecutionResult result = connection->TryExecute(toRun, message, iInsertID, 0, &counted);

      manager_->MeasureQuery_(command, MicrosecondsSince(started));

      if (result == DALConnection::DALSuccess)
      {
         if (marked)
         {
            // A statement marked [IGNORE-ERRORS] whose failure the backend answered
            // as a success, which the marker allows for anything but a lost
            // connection. A deadlock is not "this object exists already": it has
            // rolled the transaction back, and what followed would commit on its
            // own.
            switch (connection->ClassifyLastFailure(DALConnection::DALSuccess))
            {
            case DALConnection::LastFailureRetryable:
               sErrorMessage = _T("A statement marked [IGNORE-ERRORS] met a deadlock, a serialisation failure or a lock wait that timed out, which ends the transaction.");
               root->Fail_(FailureRetryable, sErrorMessage);
               return false;
            case DALConnection::LastFailureConnection:
               sErrorMessage = _T("A statement marked [IGNORE-ERRORS] lost the connection, which ends the transaction.");
               root->Fail_(FailureConnection, sErrorMessage);
               return false;
            default:
               break;
            }
         }

         if (connection->IsTransactionFailed())
         {
            // A statement marked [IGNORE-ERRORS] whose failure the backend answered
            // as a success, and the failure has ended the transaction all the
            // same: on PostgreSQL any failure aborts it, which the savepoint takes
            // back; on SQL Server some roll it back (a conversion error), and what
            // followed would commit on its own (ADOConnection::IsTransactionFailed).
            if (savepoint)
            {
               // Taken back to the savepoint: the statement changed no row.
               const bool undone = RunControl_(UndoToSavepoint, sErrorMessage) &&
                                   RunControl_(ReleaseSavepoint, sErrorMessage);
               if (undone && rowsAffected != nullptr)
                  *rowsAffected = 0;

               return undone;
            }

            sErrorMessage = _T("A statement failed inside the transaction, and the database has ended the transaction, so nothing of it is committed.");
            root->Fail_(FailureStatement, sErrorMessage);
            return false;
         }

         if (savepoint)
         {
            const bool released = RunControl_(ReleaseSavepoint, sErrorMessage);
            if (released && rowsAffected != nullptr)
               *rowsAffected = counted;

            return released;
         }

         // What TryExecute counted: the rows the statement matched, or 0 for a
         // failure the [IGNORE-ERRORS] marker let through.
         if (rowsAffected != nullptr)
            *rowsAffected = counted;

         sErrorMessage = _T("");
         return true;
      }

      DALConnection::LastFailure kind = connection->ClassifyLastFailure(result);

      if (kind == DALConnection::LastFailureDuplicate && ignoreDuplicate)
      {
         // The row is there already, which this caller does not count as a
         // failure; it reads the insert id of 0 as just that, and a count of 0,
         // since the statement changed no row.
         if (iInsertID != nullptr)
            *iInsertID = 0;

         if (savepoint)
         {
            const bool undone = RunControl_(UndoToSavepoint, sErrorMessage) &&
                                RunControl_(ReleaseSavepoint, sErrorMessage);
            if (undone && rowsAffected != nullptr)
               *rowsAffected = 0;

            return undone;
         }

         if (rowsAffected != nullptr)
            *rowsAffected = 0;

         sErrorMessage = _T("");
         return true;
      }

      sErrorMessage = message;

      switch (kind)
      {
      case DALConnection::LastFailureConnection:
         root->Fail_(FailureConnection, message);
         break;
      case DALConnection::LastFailureRetryable:
         root->Fail_(FailureRetryable, message);
         break;
      default:
         root->Fail_(FailureStatement, message);

         // Reported as the pool reports a statement that failed, and after the
         // rollback, with the connection back in the pool (Fail_), so that an
         // OnError script that reads or writes the rows this transaction held
         // finds them free and a connection to do it on.
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5032, "DatabaseTransaction::Execute", message);
         break;
      }

      return false;
   }

   std::shared_ptr<DALRecordset>
   DatabaseTransaction::OpenRecordset_(const SQLCommand &command)
   {
      std::shared_ptr<DALRecordset> empty;

      String message;
      if (!Usable_(message))
      {
         // Empty is "nothing was found" to the caller unless the thread says
         // otherwise: a lookup refused here did not find that nothing was there.
         LOG_DEBUG(_T("DatabaseTransaction: ") + message);
         DatabaseUnavailableMarker::Mark();
         return empty;
      }

      DatabaseTransaction *root = root_;

      if (DatabaseConnectionManager::SimulateFailureFor_(command))
      {
         root->Fail_(FailureStatement, _T("Simulated database failure ([Settings] SimulateDatabaseFailureFor)."));
         return empty;
      }

      std::shared_ptr<DALConnection> connection = root->connection_;
      std::shared_ptr<DALRecordset> recordset = connection->CreateRecordset();

      if (!root->transactional_)
      {
         // As in Execute_: on a database that is not transactional the query runs
         // as the pool runs it, again after a reconnect, and is reported (HM5033)
         // when it fails.
         std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

         const bool opened = recordset->Open(connection, command);

         manager_->MeasureQuery_(command, MicrosecondsSince(started));

         if (opened)
            return recordset;

         root->Fail_(FailureStatement, _T("A query could not be run (HM5033)."));
         DatabaseUnavailableMarker::Mark();
         return empty;
      }

      const SQLCommand toRun = AsTheConnectionTakesIt(connection, command);

      connection->ForgetLastFailure();

      std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

      DALConnection::ExecutionResult result = recordset->TryOpen(connection, toRun, message);

      manager_->MeasureQuery_(command, MicrosecondsSince(started));

      if (result == DALConnection::DALSuccess)
         return recordset;

      switch (connection->ClassifyLastFailure(result))
      {
      case DALConnection::LastFailureConnection:
         root->Fail_(FailureConnection, message);
         break;
      case DALConnection::LastFailureRetryable:
         root->Fail_(FailureRetryable, message);
         break;
      default:
         root->Fail_(FailureStatement, message);
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5033, "DatabaseTransaction::OpenRecordset", message);
         break;
      }

      // As the pool does for a query that could not be run: the database did not
      // answer, which is not "the database answered with nothing".
      DatabaseUnavailableMarker::Mark();

      return empty;
   }

   bool
   DatabaseTransaction::RunControl_(const String &sql, String &sErrorMessage)
   {
      DatabaseTransaction *root = root_;
      std::shared_ptr<DALConnection> connection = root->connection_;

      connection->ForgetLastFailure();

      String message;
      DALConnection::ExecutionResult result = connection->TryExecute(SQLCommand(sql), message, 0, 0);
      if (result == DALConnection::DALSuccess)
         return true;

      sErrorMessage = message;

      switch (connection->ClassifyLastFailure(result))
      {
      case DALConnection::LastFailureConnection:
         root->Fail_(FailureConnection, message);
         break;
      case DALConnection::LastFailureRetryable:
         root->Fail_(FailureRetryable, message);
         break;
      default:
         root->Fail_(FailureStatement, message);
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5032, "DatabaseTransaction::Execute", message);
         break;
      }

      return false;
   }

   void
   DatabaseTransaction::Fail_(Failure failure, const String &message)
   {
      // Called on the outermost scope. The first failure is the one that counts:
      // what follows it is its consequence.
      HM_ASSERT(root_ == this);

      if (failure_ == FailureNone)
      {
         failure_ = failure;
         error_ = message;
      }

      if (failure == FailureConnection || failure == FailureCommitUnknown)
      {
         // The session is gone, and the database has rolled back whatever it had
         // not committed. It is opened again before the pool has it back.
         reconnect_on_release_ = true;
         in_transaction_ = false;
      }
      else
      {
         // Rolled back now rather than when the scope ends, so that the locks it
         // holds - on MySQL a lock wait that timed out took back only its own
         // statement - are free for the other units at once.
         RollBack_();
      }

      // And the connection goes back to the pool now, not when the scope ends:
      // whatever runs while the failure is reported - an OnError script, which
      // ErrorManager runs outside the transaction, on the pool - and the rest of
      // the server find it there. On SQL Server Compact the pool holds the one
      // connection the server has (IniFileSettings), so a failed scope that kept
      // it until its caller let it go made the script wait for it a minute a
      // statement, and every other thread with it (measured on the bench, 13
      // October 2026). The scope stays its thread's, and refuses every statement
      // until it ends.
      Release_();
   }

   void
   DatabaseTransaction::RollBack_()
   {
      if (!in_transaction_ || !connection_)
         return;

      in_transaction_ = false;

      // Nothing to take back on a database that is not transactional, and
      // MySQLConnection::RollbackTransaction would report one as needing a
      // restore from backup (HM5104).
      if (!transactional_)
         return;

      String rollbackError;
      if (!connection_->RollbackTransaction(rollbackError))
      {
         // SQL Server has already rolled back a transaction it chose as a
         // deadlock's victim, and refuses a ROLLBACK with no transaction open (3903;
         // measured with SQLOLEDB on SQL Server 2022, 13 October 2026, the
         // victim's @@TRANCOUNT 0). Whatever the reason, a connection that may still
         // hold a transaction does not go back to the pool as it is: it is opened
         // again first. On SQL Server that ends nothing by itself - ADO's Close
         // hands the session to the provider's pool as it is, a transaction left
         // open still open (measured the same day) - but each refusal met there
         // has no transaction left to take back: the deadlock's, and a session the
         // database has ended.
         LOG_DEBUG(_T("DatabaseTransaction: the rollback was refused, so the connection will be opened again: ") + rollbackError);
         reconnect_on_release_ = true;
      }
   }

   void
   DatabaseTransaction::Release_()
   {
      HM_ASSERT(root_ == this);

      RollBack_();

      if (!connection_)
         return;

      if (reconnect_on_release_)
      {
         // A session that was lost, or whose transaction may still be open, is
         // replaced before the pool has it back: the next statement on it must
         // neither find it dead nor run inside what this scope left. If the
         // database cannot be reached now, the pool's own reconnect deals with it
         // at the connection's next use.
         String reconnectError;
         connection_->Reconnect(reconnectError);
         reconnect_on_release_ = false;
      }

      manager_->ReleaseConnection_(connection_);
      connection_.reset();
   }

   void
   DatabaseTransaction::End_()
   {
      Release_();
      ended_ = true;
   }

   bool
   DatabaseTransaction::Commit()
   {
      String error;
      if (!Usable_(error))
      {
         if (root_ == this && !ended_)
            End_();

         return false;
      }

      if (root_ != this)
      {
         // A joined scope's part is done; the outermost scope commits.
         committed_ = true;
         return true;
      }

      if (rollback_only_ || joined_open_ > 0)
      {
         // A joined scope ended without its part done, or is still open while the
         // transaction it joined is asked to commit: neither can be committed as
         // the unit its writers meant.
         HM_ASSERT(joined_open_ == 0);

         if (failure_ == FailureNone)
         {
            failure_ = FailureAbandoned;
            error_ = _T("The transaction was asked to commit while a unit of work joined to it had not finished, so it is rolled back.");
         }

         End_();
         return false;
      }

      if (connection_->IsTransactionFailed())
      {
         // PostgreSQL would answer the COMMIT with success and roll back; SQL
         // Server, whose transaction has ended, would refuse it.
         Fail_(FailureStatement, _T("A statement failed inside the transaction, and the database has ended the transaction, so nothing of it is committed."));
         End_();
         return false;
      }

      if (!transactional_)
      {
         // MySQL on a database with a table that is not InnoDB: every statement
         // was committed as it ran, and there is nothing to commit.
         in_transaction_ = false;
         committed_ = true;
         End_();
         return true;
      }

      connection_->ForgetLastFailure();

      String commitError;
      if (connection_->CommitTransaction(commitError))
      {
         in_transaction_ = false;
         committed_ = true;
         End_();
         return true;
      }

      switch (connection_->ClassifyLastFailure(DALConnection::DALUnknown))
      {
      case DALConnection::LastFailureConnection:
         // The COMMIT may have reached the database with only its answer lost.
         Fail_(FailureCommitUnknown, _T("The connection was lost during the COMMIT, so whether the transaction was committed is not known: ") + commitError);
         break;
      case DALConnection::LastFailureRetryable:
         Fail_(FailureRetryable, commitError);
         break;
      default:
         Fail_(FailureStatement, _T("The transaction could not be committed: ") + commitError);
         ErrorManager::Instance()->ReportError(ErrorManager::High, 8502, "DatabaseTransaction::Commit", error_);
         break;
      }

      End_();
      return false;
   }

   void
   DatabaseTransaction::Rollback()
   {
      if (std::this_thread::get_id() != thread_)
      {
         HM_ASSERT(0);
         return;
      }

      if (root_ != this)
      {
         // The part this joined scope wrote cannot be taken back alone.
         committed_ = false;
         root_->rollback_only_ = true;

         if (root_->failure_ == FailureNone && !root_->ended_)
         {
            root_->failure_ = FailureAbandoned;
            root_->error_ = _T("A unit of work joined to this transaction was rolled back, so the whole transaction is.");
         }

         return;
      }

      if (ended_)
         return;

      if (failure_ == FailureNone)
      {
         failure_ = FailureAbandoned;
         error_ = _T("The transaction was rolled back.");
      }

      End_();
   }

   DatabaseTransaction::RunResult
   DatabaseTransaction::Run(std::shared_ptr<DatabaseConnectionManager> manager, const Work &work, int attempts)
   {
      RunResult result;

      if (attempts < 1)
         attempts = 1;

      DatabaseTransaction *outer = Current_(manager.get());
      if (outer != nullptr && !outer->root_->ended_)
      {
         // Inside a unit of work of this manager on this thread: a part of it,
         // run once. A deadlock here has rolled back the outer transaction, so
         // only the outer unit can be run again, by the Run that runs it.
         DatabaseTransaction transaction(manager);
         result.attempts = 1;

         if (transaction.IsOpen() && work(transaction) && transaction.Commit())
         {
            result.outcome = Committed;
            return result;
         }

         result.outcome = Failed;
         result.error = transaction.GetErrorMessage();
         return result;
      }

      for (int attempt = 1; attempt <= attempts; attempt++)
      {
         result.attempts = attempt;

         Failure failure = FailureNone;
         bool transactional = true;

         {
            DatabaseTransaction transaction(manager);

            if (transaction.IsOpen() && work(transaction) && transaction.Commit())
            {
               result.outcome = Committed;
               result.error = _T("");
               return result;
            }

            failure = transaction.GetFailure();
            transactional = transaction.IsTransactional();
            result.error = transaction.GetErrorMessage();
         }

         if (failure == FailureCommitUnknown)
         {
            result.outcome = CommitUnknown;

            ErrorManager::Instance()->ReportError(ErrorManager::High, 8501, "DatabaseTransaction::Run",
               _T("A unit of work lost its database connection while its transaction was being committed, so whether it was committed is not known, and it was not run again: ") + result.error);
            return result;
         }

         // The work said no, or a statement failed for a reason a retry would not
         // mend, or the pool had no connection to give; or nothing a failed
         // attempt wrote can be taken back.
         const bool retryable = failure == FailureRetryable || failure == FailureConnection;
         if (!retryable || !transactional)
         {
            result.outcome = Failed;

            if (failure == FailureNone)
               result.error = _T("The unit of work did not complete, and its transaction was rolled back.");

            return result;
         }

         if (attempt == attempts)
            break;

         // Without the database's own message, which carries the statement and its
         // values: HM8500 has it when Run gives up.
         String line;
         line.Format(_T("DatabaseTransaction: attempt %d of %d of a unit of work met %s and was rolled back; it is run again from the start."),
            attempt, attempts,
            failure == FailureConnection ? _T("a lost database connection") : _T("a deadlock, a serialisation failure or a lock wait that timed out"));
         LOG_APPLICATION(line);

         Sleep(PauseBeforeAttempt(attempt + 1, failure == FailureConnection));
      }

      result.outcome = GaveUp;

      String message;
      message.Format(_T("A unit of work was given up after %d attempts, each ended by a deadlock, a serialisation failure, a lock wait that timed out or a lost database connection, and rolled back. The last: %s"),
         result.attempts, result.error.c_str());
      ErrorManager::Instance()->ReportError(ErrorManager::High, 8500, "DatabaseTransaction::Run", message);

      return result;
   }

   DatabaseTransaction::Suspension::Suspension() :
      hidden_(current_),
      enclosing_(suspension_)
   {
      current_ = nullptr;
      suspension_ = this;
   }

   DatabaseTransaction::Suspension::~Suspension()
   {
      // Scopes opened during the suspension have ended (they are on the stack
      // inside it), so the thread's chain is the one hidden.
      HM_ASSERT(current_ == nullptr);
      HM_ASSERT(suspension_ == this);
      current_ = hidden_;
      suspension_ = enclosing_;
   }

   size_t
   DatabaseTransaction::ConnectionsHeldOnThisThread_(const DatabaseConnectionManager *manager)
   {
      // A connection is held by the outermost scope of a transaction; a joined
      // scope runs on its root's. The chain in force, then each one a Suspension
      // hides: the scopes opened inside a Suspension form a chain of their own,
      // so no scope is in two.
      size_t held = 0;

      auto count = [&](const DatabaseTransaction *chain)
      {
         for (const DatabaseTransaction *scope = chain; scope != nullptr; scope = scope->previous_)
         {
            if (scope->manager_.get() == manager && scope->root_ == scope && scope->connection_)
               held++;
         }
      };

      count(current_);

      for (const Suspension *suspension = suspension_; suspension != nullptr; suspension = suspension->enclosing_)
         count(suspension->hidden_);

      return held;
   }
}
