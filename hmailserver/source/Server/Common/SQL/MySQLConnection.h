// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DALConnection.h"
#include "DatabaseTls.h"
#include "MySQLInterface.h"
#include "ColumnPositionCache.h"

namespace HM
{
   

   class MySQLConnection : public DALConnection, public std::enable_shared_from_this<MySQLConnection>
   {
   public:

      enum Server
      {
         RequiredVersion = 40118
      };

      MySQLConnection(std::shared_ptr<DatabaseSettings> pSettings);
      virtual ~MySQLConnection();

      virtual ConnectionResult Connect(String &sErrorMessage);
      virtual bool Disconnect();
      virtual ExecutionResult TryExecute(const SQLCommand &command, String &sErrorMessage, __int64 *iInsertID = 0, int iIgnoreErrors = 0, __int64 *rowsAffected = nullptr);
      virtual bool IsConnected() const;

      hm_MYSQL *GetConnection() const;

      ExecutionResult CheckError(hm_MYSQL *pSQL, const String &sAdditionalInfo, String &sOutputErrorMessage) const;

      virtual void OnConnected();

      virtual bool BeginTransaction(String &sErrorMessage);
      virtual bool CommitTransaction(String &sErrorMessage);
      virtual bool RollbackTransaction(String &sErrorMessage);

      /*
         The MySQL family has no client-side statement timeout either, and the two
         mysql_options values that look like one - MYSQL_OPT_READ_TIMEOUT and
         MYSQL_OPT_WRITE_TIMEOUT - are socket timeouts, not statement timeouts:
         they fire on a silent socket, so a server that is busily working on a
         long statement never trips them, and when they do fire the statement is
         still running on the server while the client believes the connection is
         gone. They are deliberately not used here.

         What exists is a server-side session variable, and it is spelled
         differently and measured differently by the two forks:

            MariaDB 10.1.1+     max_statement_time    SECONDS
            MySQL 5.7.8+        max_execution_time    MILLISECONDS
            MySQL 5.7.4-5.7.7   max_statement_time    MILLISECONDS

         That last row is why the fork is identified before the variable is
         chosen rather than the variable simply being probed for: MariaDB and
         early MySQL agree on the NAME and disagree on the UNIT, so a server told
         "30" meaning seconds would abort every statement after 30 milliseconds.
         Getting it wrong in the other direction is silent - an unknown system
         variable is an error, so nothing would be set and nothing would say so -
         and both are worse than the mechanism being absent, so LoadTimeoutMechanism_
         identifies the fork from VERSION(), then confirms the variable exists by
         setting it to 0 (no limit) before anything relies on it.

         The honest limit, stated because it is easy to assume otherwise: MySQL's
         max_execution_time applies only to read-only SELECT statements. On MySQL a
         blocked UPDATE or DELETE still holds its pooled connection. MariaDB's
         max_statement_time covers statements generally, and PostgreSQL's
         statement_timeout covers all of them.
      */
      virtual void SetTimeout(int seconds);

      virtual bool GetSupportsCommandParameters() const {return false; }
      ColumnPositionCache& GetColumnPositionCache() {return column_position_cache_;}

      virtual bool CheckServerVersion(String &errorMessage);

      virtual std::shared_ptr<DALRecordset> CreateRecordset();

      virtual void EscapeString(String &sInput);

      virtual std::shared_ptr<IMacroExpander> CreateMacroExpander();

      // A name as MySQL reads a quoted identifier: in backquotes, each backquote
      // in it doubled. Connect's "use" and the transactions' "SHOW TABLE STATUS
      // in" name the database so, where they named it bare until 6.3.6 and a
      // database whose name MySQL takes only quoted - with a hyphen in it, say,
      // which DBSetup accepts for a database that exists - refused every
      // connection with a syntax error.
      static String QuoteIdentifier(const String &name);

      // DALConnection's, from mysql_errno - which still holds the failure's number
      // after TryExecute returns - and from whether every table is InnoDB.
      virtual LastFailure ClassifyLastFailure(ExecutionResult result);
      virtual bool GetSupportsTransactions() const { return supports_transactions_; }

      // DALConnection's: whether the database has ended the transaction
      // BeginTransaction began, at a failure in it - MySQL and MariaDB roll the
      // whole transaction back at a deadlock (1213), MariaDB at a row changed
      // since the transaction read it (1020, under innodb_snapshot_isolation,
      // which MariaDB 11.8 has on by default), both at a lock table that is full
      // (1206) and at a row-lock wait that timed out (1205) when
      // innodb_rollback_on_timeout is on, and the session goes on in autocommit,
      // so that every statement after it is committed on its own and a COMMIT
      // with nothing open is answered with success. A lost connection ends the
      // transaction with the session. The client's own status flags cannot tell
      // it: an error packet carries none, and the flags keep the last OK packet's
      // SERVER_STATUS_IN_TRANS through the failure (measured through libmariadb
      // on MySQL 8.4 and MariaDB 11.8, 15 October 2026). A metadata-lock wait that
      // timed out is 1205 too, and ends nothing, whatever innodb_rollback_on_timeout
      // says: so after a 1205 where it is on, the session itself is asked, once,
      // here (SessionHoldsTransaction_). False while no transaction
      // BeginTransaction began is open; true once the session it was on has been
      // replaced (DALConnection::IsTransactionSessionLost).
      virtual bool IsTransactionFailed() const;

   private:

      // One attempt at the server: a fresh handle (the one before it closed), the
      // plugin directory, the TLS options for the mode, mysql_real_connect. The
      // connected handle, or null with the client's error text in error and its
      // error number in errorNumber (0 when nothing reached the client).
      hm_MYSQL *ConnectOnce_(const String &server, const String &username, const String &password, const String &database, long port,
                             DatabaseTls::Mode mode, const String &caFile, String &error, unsigned int &errorNumber);

      // The TLS options for the mode, in the numbers of whichever client is loaded
      // (MySQLInterface.h). False, with the reason, when require or verify-full
      // cannot be applied because the client refused an option.
      bool ApplyTlsOptions_(DatabaseTls::Mode mode, const String &caFile, String &error);

      DALConnection::ExecutionResult GetErrorType_(hm_MYSQL *pSQL);

      void UpdatePassword_();
      void RunScriptFile_(const String &sFile) ;
      void RunCommand_(const String &sCommand) ;
      void LoadSupportsTransactions_(const String &database);
      void SetConnectionCharacterSet_();

      // innodb_rollback_on_timeout, a server variable set at the server's start,
      // read once per connection, from Connect: whether a lock wait that timed
      // out (1205) rolls the whole transaction back or its own statement alone.
      void LoadRollbackOnTimeout_();

      // Whether the failure CheckError reads, of a statement inside a transaction
      // BeginTransaction began, has ended that transaction (IsTransactionFailed).
      // Not a lock wait that timed out (1205), which the session is asked about.
      bool FailureEndsTransaction_(unsigned int errorCode, ExecutionResult result) const;

      // Whether the session holds an open transaction, asked of the session
      // itself, by statements sent as they are so that neither is reported nor
      // read as the caller's failure: a SAVEPOINT and its RELEASE SAVEPOINT, which
      // inside an open transaction both succeed and change nothing, and in
      // autocommit the SAVEPOINT does nothing and the RELEASE is refused (1305).
      // Measured with the databases' own clients on MySQL 8.4.11 and MariaDB
      // 11.8.9, each with innodb_rollback_on_timeout on and off, 16 October 2026:
      // after a metadata-lock wait that timed out both succeed and the
      // transaction's rows are still its own, uncommitted, until its ROLLBACK
      // takes them back; after a row-lock wait that timed out with
      // innodb_rollback_on_timeout on, and after a deadlock, the RELEASE is
      // refused and the next statement is committed on its own; after a row-lock
      // wait that timed out with it off both succeed. A session lost answers
      // false.
      bool SessionHoldsTransaction_() const;

      // Works out which session variable this server understands, and in which
      // unit, and proves it by setting it. Run once per connection, from Connect().
      void LoadTimeoutMechanism_();

      // True when "SET SESSION <variable> = 0" was accepted, i.e. the variable
      // exists on this server. 0 means "no limit" on every version that has any of
      // these variables, so the probe cannot leave a limit behind.
      bool TrySetTimeoutVariable_(const String &variableName, const String &value);

      hm_MYSQL *dbconn_;

      bool is_connected_;
      bool supports_transactions_;

      // Empty when this server has no statement-timeout variable at all, in which
      // case SetTimeout does nothing and said so once at connect time.
      String timeout_variable_;
      bool timeout_variable_is_seconds_;

      // Whether VERSION(), read at each Connect (LoadTimeoutMechanism_), said MySQL
      // rather than MariaDB: error 4031 is a closed idle session on MySQL 8.0.24
      // and later and an ordinary refusal on MariaDB, so CheckError takes it for a
      // lost connection only when this is true. False until it is known.
      bool server_is_mysql_;

      // Once the database has ended the transaction BeginTransaction began
      // (DALConnection::IsTransactionOpen_) at a failure in it, which CheckError -
      // const, and MySQLRecordset's too - sees; and a lock wait in it that timed
      // out where innodb_rollback_on_timeout is on, which IsTransactionFailed asks
      // the session about. Cleared by BeginTransaction, CommitTransaction and
      // RollbackTransaction.
      mutable bool transaction_ended_;
      mutable bool lock_wait_timed_out_;

      // innodb_rollback_on_timeout as read at Connect; true when it could not be
      // read, since taking a lock wait that timed out for the end of the transaction
      // only rolls back what the caller then runs again, where the other reading
      // would let what follows it be committed on its own.
      bool rollback_on_timeout_;

      ColumnPositionCache column_position_cache_;

   };

   // DALConnection::ClientBytes, which the MySQL client's arguments and libpq's
   // conninfo are written in - UTF-8 on POSIX; on Windows Latin-1 for a value it
   // carries, as before, and UTF-8 for any other, which reached the server empty -
   // and MySQLConnection::QuoteIdentifier, a database's name quoted whatever it
   // holds. Throws std::logic_error with a sentence. connection-string-tests runs
   // it on Linux, ClassTester inside the Windows service. No database,
   // configuration or network.
   class MySQLConnectionTester
   {
   public:
      void Test();
   };

}
