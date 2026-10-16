// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class DatabaseSettings;
   class MacroExpander;
   class IMacroExpander;

   class DALConnection  
   {
   public:
      DALConnection(std::shared_ptr<DatabaseSettings> pDatabaseSettings);
      virtual ~DALConnection();
      // The MySQL and PostgreSQL connections close their handle to the database
      // server in their destructors: a copy would close it a second time.
      DALConnection(const DALConnection &) = delete;
      DALConnection &operator=(const DALConnection &) = delete;

      enum ConnectionResult
      {
         Connected = 1,
         TemporaryFailure = 2,
         FatalError = 3
      };

      enum ExecutionResult
      {
         DALSuccess = 0,
         DALErrorInSQL = 1,
         DALConnectionProblem = 2,
         DALUnknown = 3
      };

      // A statement, run again after a reconnect when the connection was lost under
      // it (up to SetTryCount times). rowsAffected, when given, is what TryExecute
      // says of the run that succeeded, and -1 when none did. reconnected, when
      // given, says whether the connection was lost while the statement ran. A run
      // that lost its connection may still have been committed, with only its answer
      // lost, and the next attempt sends the statement again: so after a reconnect a
      // failure may hide a committed first run, and a 0 from an UPDATE that is not
      // idempotent may be its own first run's doing (ActiveActive.md: a claim
      // recognises itself by its epoch for this reason). Not on a connection whose
      // session was replaced inside a transaction BeginTransaction began
      // (IsTransactionSessionLost): there neither this statement nor any after it
      // is run, until the transaction's end, and false is answered with
      // TransactionSessionLostError.
      bool Execute(const SQLCommand &command, String &sErrorMessage, __int64 *iInsertID = 0, int iIgnoreErrors = 0,
                   __int64 *rowsAffected = nullptr, bool *reconnected = nullptr);

      bool Reconnect(String &sErrorMessage);

      // True when the statement carries the DAL's [IGNORE-ERRORS] marker in a
      // position where it means what it is meant to mean.
      //
      // (Line comments rather than a block comment on purpose: this text has to
      // name the SQL comment forms the marker is written in, one of which ends a
      // C block comment, so a /* */ block here closes itself twenty lines early
      // and the rest of the prose is compiled as code.)
      //
      // The marker exists so that re-running an upgrade step whose column, table
      // or index already exists is harmless on the three backends whose DDL has
      // no IF NOT EXISTS form (MSSQL, SQL CE, MySQL; the PostgreSQL scripts use
      // IF NOT EXISTS and carry no marker). Every use of it in the tree - six
      // upgrade scripts and SqlLogDevice's create statements - writes it inside a
      // trailing SQL comment, either a "---" line comment or a slash-star block.
      //
      // It used to be looked for with a plain substring search over the whole
      // statement, which meant a *value* carrying that text switched error
      // handling off for the statement it appeared in. hm_message_metadata and
      // the SQL log device both write attacker-supplied text - a subject line, a
      // logged protocol command - so that was reachable from outside, and what it
      // bought the attacker was one silently discarded write.
      //
      // This scan therefore skips over single-quoted string literals (handling
      // both the doubled '' and the MySQL backslash escape, so a literal is not
      // closed early) and honours the marker only where it appears inside a
      // comment. A value can reach neither.
      //
      // What it does NOT decide is which errors are then discarded; see the
      // callers. A lost connection is never discarded, because "the statement was
      // already applied" and "the statement never reached the server" are not the
      // same thing, and it was the second one masquerading as the first that
      // stamped schema version 6006 onto databases that had not got the column.
      static bool HasIgnoreErrorsMarker(const String &sql);

      // A value of the connection - the server's name, the user, the password, the
      // database's name - as the bytes a client library is handed for it (libpq's
      // conninfo, the MySQL client's arguments), which reach the server as they
      // are. On POSIX, UTF-8. On Windows, a value whose every character is U+00FF
      // or below goes one byte a character, Latin-1, as Unicode::ToANSI - the
      // CRT's C locale, which this process never changes - wrote it up to 6.3.5:
      // a password with such a letter that connects today was set in those bytes,
      // on a database server whose encoding is WIN1252 or LATIN1, and goes on
      // connecting. Any other value goes as UTF-8, where ToANSI left the whole
      // value empty - on Windows for a character beyond U+00FF, on POSIX for any
      // outside ASCII - and the client connected with its own defaults. False, with
      // nothing written, for text UTF-8 cannot carry (half of a surrogate pair).
      static bool ClientBytes(const String &value, AnsiString &bytes);

      virtual ConnectionResult Connect(String &sErrorMessage) = 0;
      virtual bool Disconnect() = 0;

      // One run of a statement on this connection, nothing run again.
      //
      // rowsAffected, when given, is set to the number of rows the statement
      // matched: for an UPDATE every row its WHERE clause matched, whether or not
      // the new values differ from the old - on all four backends, MySQL and
      // MariaDB included because the connection is opened with CLIENT_FOUND_ROWS
      // (MySQLConnection::ConnectOnce_) - and for an INSERT or a DELETE the rows
      // inserted or deleted. ADO's RecordsAffected on SQL Server and SQL Server
      // Compact, mysql_affected_rows on MySQL, PQcmdTuples on PostgreSQL. -1 when
      // the statement failed, or when the engine gave no count (SQL Server with
      // NOCOUNT on, which ADOConnection::Connect turns off). A failure the caller
      // asked to ignore (iIgnoreErrors, or the [IGNORE-ERRORS] marker) counts 0: a
      // statement that fails changes no row. A count is to be read for one INSERT,
      // UPDATE or DELETE alone: what the engines answer for a SELECT, for DDL or
      // for several statements in one command differs from one to the next
      // (measured on SQL Server Compact 4.0: a SELECT 0, DDL -1).
      //
      // iIgnoreErrors is 0 or DALErrorInSQL, the one failure a caller may forgive:
      // a row the database's own rules refused, which each backend knows by its
      // own code - on PostgreSQL a unique violation (SQLSTATE 23505) and nothing
      // else; on MySQL and MariaDB a duplicate key (1062) and nothing else; on SQL
      // Server any integrity violation (DB_E_INTEGRITYVIOLATION), which is a
      // duplicate key, a NULL in a NOT NULL column, a foreign key's or a CHECK's
      // refusal alike; on SQL Server Compact a duplicate key or a foreign key's
      // refusal, a NOT NULL refusal being another error there (measured on SQL
      // Server 2022 and Compact 4.0, 13 October 2026). Forgiven, it answers
      // DALSuccess, a count of 0 and no insert id. Every other failure fails as
      // it would without the flag, and a lost connection is DALConnectionProblem
      // whatever the flag says, so DALConnection::Execute reconnects and runs the
      // statement again.
      virtual ExecutionResult TryExecute(const SQLCommand &command, String &sErrorMessage, __int64 *iInsertID = 0, int iIgnoreErrors = 0, __int64 *rowsAffected = nullptr) = 0;
      virtual bool IsConnected() const = 0;
      virtual void OnConnected() = 0;

      virtual bool BeginTransaction(String &sErrorMessage) = 0;
      virtual bool CommitTransaction(String &sErrorMessage) = 0;
      virtual bool RollbackTransaction(String &sErrorMessage) = 0;
      virtual void SetTimeout(int seconds) = 0;

      // Puts the connection back on the server-wide statement timeout, [Settings]
      // DatabaseStatementTimeout.
      //
      // There has to be a "back to normal" for the same reason SQLScriptRunner has a
      // scope guard: the timeout is a property of a POOLED connection on all four
      // backends - ADO's CommandTimeout, and a session GUC/variable on PostgreSQL and
      // MySQL - so whatever the last caller left it at is what the next unrelated
      // statement inherits. SQLScriptRunner used to restore the literal 30, which was
      // right only because 30 happens to be ADO's own default; with the value now
      // configurable, and with PostgreSQL and MySQL having no default of their own,
      // it has to come from one place.
      void ApplyDefaultTimeout();

      void SetTryCount(int iTryCount) {try_count_ = iTryCount; }
      

      virtual bool GetSupportsCommandParameters() const = 0;

      virtual bool CheckServerVersion(String &errorMessage) = 0;

      // Implemented by all four backends and called by none of them. The
      // escaping that actually runs is the static SQLStatement::Escape, which
      // decides what to double from IniFileSettings rather than from the
      // connection - the same setting DALConnectionFactory used to pick the
      // backend, so the two cannot disagree. Kept because removing a pure
      // virtual touches every backend for no behaviour; do not reach for it
      // thinking it is the escaping path.
      virtual void EscapeString(String &sInput) = 0;

      virtual std::shared_ptr<DALRecordset> CreateRecordset() = 0;
      virtual std::shared_ptr<IMacroExpander> CreateMacroExpander() = 0;

      std::shared_ptr<DatabaseSettings> GetSettings() {return database_settings_; }

      // How this connection reaches the server, in one line - encrypted or not,
      // with what, whether the certificate was checked and under which TlsMode
      // (DatabaseTls::Describe) - and whether it is encrypted. Set by Connect on
      // the three network backends; empty on SQL Server Compact, which is a file.
      String GetTransportSecurity() const { return transport_security_; }
      bool GetTransportEncrypted() const { return transport_encrypted_; }

      // What the statement that failed last on this connection failed with, in
      // the terms a DatabaseTransaction acts on: the connection lost under it; a
      // deadlock, a serialisation failure or a lock wait that timed out, for which
      // a unit of work is run again whole (SQL Server 1205, MySQL and MariaDB
      // 1213, 1205 and 1020, PostgreSQL 40001 and 40P01, SQL Server Compact
      // 25090); a row that is there already, which is a primary key's or a unique
      // index's duplicate the backend names (SQL Server 2627 and 2601, MySQL and
      // MariaDB 1062, PostgreSQL 23505, SQL Server Compact 25016) and no other
      // integrity violation - the backends' own DALErrorInSQL is every integrity
      // violation on SQL Server (a foreign key's 547, a NULL's 515, a CHECK's
      // 547), and on SQL Server Compact a duplicate (25016) or a foreign key's
      // refusal (25026, or 25025 for a delete of a row a foreign key refers to);
      // or anything else. Asked straight after the failure, with what TryExecute,
      // TryOpen or the transaction call returned (DALUnknown when it said no more
      // than false), before anything else runs on the connection;
      // ForgetLastFailure before the statement, so that what is read is that
      // statement's.
      enum LastFailure
      {
         LastFailureOther = 0,
         LastFailureConnection = 1,
         LastFailureRetryable = 2,
         LastFailureDuplicate = 3
      };

      virtual LastFailure ClassifyLastFailure(ExecutionResult result) { return result == DALConnectionProblem ? LastFailureConnection : LastFailureOther; }
      virtual void ForgetLastFailure() {}

      // Whether a transaction on this connection is one: false only on MySQL and
      // MariaDB when a table of the database is not InnoDB, where BeginTransaction
      // succeeds having begun nothing (MySQLConnection::LoadSupportsTransactions_).
      virtual bool GetSupportsTransactions() const { return true; }

      // PostgreSQL's: a statement that fails inside a transaction aborts it, and
      // every statement after it is refused until the rollback - so a failure a
      // caller means to ignore needs a savepoint there (DatabaseTransaction).
      virtual bool GetFailedStatementEndsTransaction() const { return false; }

      // Whether the database has ended the transaction BeginTransaction began on
      // this connection, at a statement in it that failed - answered as a failure,
      // or as a success where the [IGNORE-ERRORS] marker or the caller let the
      // failure through - so that nothing of it will be committed and whatever the
      // session runs next would run outside it. On every engine the transaction
      // ends with its session, and once Reconnect has put a new session in the
      // place of the one it was on this says true until the transaction's end
      // (IsTransactionSessionLost), which is all the default here says. On
      // PostgreSQL any failure does (PQTRANS_INERROR), and every later statement
      // is refused until the rollback. On MySQL and MariaDB a deadlock (1213),
      // MariaDB's 1020, a lock table that is full (1206), a row-lock wait that
      // timed out where innodb_rollback_on_timeout is on (1205, which a
      // metadata-lock wait that timed out answers too, and ends nothing: the
      // session is asked) and a lost connection do, and the session goes on in
      // autocommit (MySQLConnection). On SQL Server a deadlock's victim (1205)
      // and some other failures - a conversion error among them - do, and the
      // session goes on in autocommit (ADOConnection, @@TRANCOUNT, read only
      // after a failure). On SQL Server Compact none does. Asked after a statement
      // of the transaction, before the next, and before its commit; false while no
      // transaction BeginTransaction began is open. A caller that goes on in a
      // transaction after a statement failed - the COM Database object's script -
      // runs nothing more in it once this says true where the session goes on in
      // autocommit (ScriptTransaction, SQLScriptRunner), as DatabaseTransaction
      // runs nothing more after any failure; on PostgreSQL
      // (GetFailedStatementEndsTransaction), where the transaction is aborted and
      // still open, the database refuses everything itself but the rollback and a
      // ROLLBACK TO SAVEPOINT.
      virtual bool IsTransactionFailed() const { return transaction_session_lost_; }

      // Whether the session the transaction BeginTransaction began on this
      // connection was on has gone, and Reconnect has put a new one in its place,
      // before CommitTransaction or RollbackTransaction ended the transaction:
      // DALConnection::Execute and DALRecordset::Open reconnect when the connection
      // is lost under a statement, and Database.EnsurePrerequisites' prerequisites
      // and a script's macros run their statements with them. The database rolled
      // back what the transaction held when the session went, and the new session
      // holds no transaction, so that whatever it ran would be committed on its
      // own: Execute and Open run nothing more on the connection, the statement
      // that met the lost connection included, until the transaction's end, and
      // IsTransactionFailed says true on every engine. Up to 6.3.6 they ran the
      // statement again on the new session and went on there, in autocommit
      // (ActiveActive.md). False while no transaction BeginTransaction began is
      // open, and cleared by BeginTransaction, CommitTransaction and
      // RollbackTransaction.
      bool IsTransactionSessionLost() const { return transaction_session_lost_; }

      // Whether the session the transaction BeginTransaction began on this
      // connection was on is gone: replaced by Reconnect (IsTransactionSessionLost),
      // or gone now, before anything has reconnected - lost under a statement the
      // caller sent once (TryExecute: the COM Database object's own statements),
      // or closed under the transaction, as the pool's Disconnect closes the
      // connections it has handed out (IsSessionGone_). The database rolled back
      // what the transaction held with the session either way. False while no
      // transaction BeginTransaction began is open.
      bool IsTransactionSessionGone() const;

      // What BeginTransaction recorded, forgotten: the connection holds no
      // transaction of its caller's any more. The pool says it as it takes a
      // connection back (DatabaseConnectionManager::ReleaseConnection_), so that a
      // caller that let go of a transaction without a CommitTransaction or a
      // RollbackTransaction - DatabaseTransaction, which sends no ROLLBACK to a
      // session that is gone and opens the connection again instead - leaves
      // nothing behind for the next: Execute and Open would refuse every statement
      // on a connection whose transaction lost its session.
      void ForgetTransaction() { SetTransactionOpen_(false); }

      // Whether the transaction BeginTransaction began has ended and whatever the
      // session runs next would run outside it, each statement committed on its
      // own: on every engine once its session is gone (IsTransactionSessionGone),
      // PostgreSQL included, whose libpq answers a connection that is gone with
      // PQTRANS_UNKNOWN rather than PQTRANS_INERROR, so that IsTransactionFailed
      // alone does not say it there; and where the database has ended it at a
      // failure (IsTransactionFailed) and the session goes on in autocommit -
      // MySQL, MariaDB, SQL Server - but not on PostgreSQL, whose transaction is
      // aborted and still open, the database refusing everything itself but the
      // rollback and a ROLLBACK TO SAVEPOINT (GetFailedStatementEndsTransaction).
      // A caller that goes on in a transaction after a statement failed runs
      // nothing more in it once this says true (ScriptTransaction,
      // SQLScriptRunner).
      bool IsTransactionEndedIntoAutocommit() const;

      // What Execute and Open answer for a statement they do not run because the
      // session the transaction was on is gone (IsTransactionSessionLost), with
      // what the statement that met the lost connection failed with, when it did.
      static String TransactionSessionLostError(const String &lostWith);

   protected:

      // Records what GetTransportSecurity answers, and notes it for the
      // diagnostics and the application log (DatabaseTls::NoteConnection).
      void SetTransportSecurity_(bool encrypted, const String &description);

      // Said by each backend: true by its BeginTransaction once it has begun a
      // transaction on the session, false by it before the BEGIN and by
      // CommitTransaction and RollbackTransaction whatever they were answered
      // with. Clears IsTransactionSessionLost. Reconnect reads it to know whether
      // the session it replaces held such a transaction.
      void SetTransactionOpen_(bool open);
      bool IsTransactionOpen_() const { return transaction_open_; }

      // Whether the session this connection holds is gone now, which the backend
      // can tell without a round trip: false here; PostgreSQL's from PQstatus,
      // MySQL's and SQL Server Compact's from a connection Disconnect has closed.
      // A lost connection is also said where it is met - MySQLConnection::CheckError
      // (2006, 2013, a TLS end of file), and ADOConnection::IsTransactionFailed,
      // which takes a question of @@TRANCOUNT that fails, or a closed connection,
      // for the end. Read only while a transaction BeginTransaction began is open
      // (IsTransactionSessionGone).
      virtual bool IsSessionGone_() const { return false; }

      std::shared_ptr<DatabaseSettings> database_settings_;

   private:

      int try_count_;

      bool transport_encrypted_;
      String transport_security_;

      // A transaction BeginTransaction began is open on the connection
      // (SetTransactionOpen_), and the session it was on has been replaced since
      // (Reconnect).
      bool transaction_open_;
      bool transaction_session_lost_;
   };

}
