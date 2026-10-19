// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <libpq-fe.h>
#include <atomic>

#include "DALConnection.h"

namespace HM
{
   class PGConnection : public DALConnection
   {
   public:
      PGConnection(std::shared_ptr<DatabaseSettings> pSettings);
      virtual ~PGConnection();

      virtual ConnectionResult Connect(String &sErrorMessage);
      virtual bool Disconnect();
      virtual ExecutionResult TryExecute(const SQLCommand &command, String &sErrorMessage, __int64 *iInsertID = 0, int iIgnoreErrors = 0, __int64 *rowsAffected = nullptr);

      // PQcmdTuples' text as TryExecute's rowsAffected: the digits as a number, and
      // -1 for the empty text it gives a command that counts nothing (CREATE, SET)
      // or for anything that is not digits.
      static __int64 ReadCommandTuples(const char *text);
      virtual bool IsConnected() const;

      DALConnection::ExecutionResult CheckError(PGresult *pResult, const String &sAdditionalInfo, String &sOutputErrorMessage) const;
      PGconn *GetConnection() const;

      virtual bool GetSupportsCommandParameters() const {return false; }

      // The conninfo Connect hands PQconnectdb: host, port, user, password and
      // dbname - postgres when database is empty - then sslmode and sslrootcert
      // when they are not empty. Every value is in single quotes with each single
      // quote and backslash in it escaped by a backslash, which is libpq's conninfo
      // syntax, so that libpq reads back exactly the value given, whatever it holds:
      // a quote, a backslash, a space, an equals sign, a keyword's text. Up to 6.3.5
      // the values were written into the quotes as they were, and a password with a
      // quote or a backslash in it ended the value there, or took the next keyword
      // into it. The bytes are DALConnection::ClientBytes': UTF-8, which a SCRAM
      // password and the role and database names of a server in UTF8 are, but on
      // Windows Latin-1 for a value Latin-1 carries, as such a value always went
      // there - and sslrootcert, a file libpq opens, is a path in the ANSI code
      // page on Windows, as fopen reads it. False, with the reason, for a value
      // that cannot be written so: text with an unpaired surrogate, or a path with
      // a character the code page does not have.
      static bool MakeConninfo(const String &server, long port, const String &user, const String &password,
                               const String &database, const String &sslMode, const String &sslRootCert,
                               AnsiString &conninfo, String &error);

      // One value as MakeConninfo writes it: the bytes in single quotes, with a
      // backslash before each single quote and each backslash among them.
      static AnsiString QuoteConninfoValue(const AnsiString &bytes);

      // Whether the server treats a backslash in a string literal as an
      // ordinary character (standard_conforming_strings on, the default since
      // 9.1) - read from the server's own answer when a connection comes up,
      // and what decides whether a backslash is doubled on the way in. False
      // until a connection has said otherwise, since not doubling on a server
      // that does escape would let a trailing backslash swallow the closing
      // quote.
      static bool StandardConformingStrings();

      // Re-reads standard_conforming_strings from the connection's own
      // parameter set - libpq keeps it current from the ParameterStatus
      // message a reload sends - so a change made under a running server is
      // seen by the next statement rather than by the next connect.
      static void NoteServerParameters(PGconn *connection);
      virtual void OnConnected();

      virtual bool BeginTransaction(String &sErrorMessage);
      virtual bool CommitTransaction(String &sErrorMessage);
      virtual bool RollbackTransaction(String &sErrorMessage);
      /*
         libpq has no per-statement timeout API, so the mechanism is the server
         side: statement_timeout, a GUC expressed in MILLISECONDS, which aborts
         any statement running longer with SQLSTATE 57014. 0 disables it, which
         is also PostgreSQL's own default.

         It is a SESSION setting, and this connection is pooled, so it does not
         belong to whichever caller set it last - which is the objection that
         kept this empty. That is answered by giving the server one limit,
         [Settings] DatabaseStatementTimeout, applying it at the end of Connect()
         so every pooled connection carries it from the moment it exists, and
         having SQLScriptRunner restore that same limit instead of the literal 30
         it used to restore. Reconnect() calls Connect(), so it survives a dropped
         connection too - which matters here more than on the ADO backends,
         because a GUC lives in the server-side session and dies with it.

         A failure to set it is logged and otherwise ignored: a connection that
         works is worth more than a timeout that does not, and a role without
         permission to SET statement_timeout is a configuration this server has
         no business refusing to run on.

         One caveat worth knowing before calling this from somewhere new: a plain
         SET inside an open transaction is TRANSACTION-local on PostgreSQL and
         reverts when that transaction ends. Neither of the two callers is inside
         one - Connect() runs before anything, and SQLScriptRunner is handed a
         connection from the pool and opens no transaction of its own - so the
         value is a session value in practice. A future caller inside a
         transaction would get a limit that quietly disappears at COMMIT.
      */
      virtual void SetTimeout(int seconds);

      virtual bool CheckServerVersion(String &errorMessage);

      virtual std::shared_ptr<DALRecordset> CreateRecordset();

      virtual void EscapeString(String &sInput);

      virtual std::shared_ptr<IMacroExpander> CreateMacroExpander();

      // DALConnection's, from the SQLSTATE CheckError keeps of the last failure,
      // and from PQstatus and PQtransactionStatus.
      virtual LastFailure ClassifyLastFailure(ExecutionResult result);
      virtual void ForgetLastFailure();
      virtual bool GetFailedStatementEndsTransaction() const { return true; }

      // A backslash is an escape in a '...' literal only while the server says
      // standard_conforming_strings is off (StandardConformingStrings), as
      // SQLStatement::Escape reads it; in an E'...' always (DALConnection::
      // EndOfLiteral).
      virtual bool GetBackslashEscapesInLiterals() const { return !StandardConformingStrings(); }
      virtual bool IsTransactionFailed() const;

   protected:

      // DALConnection's: a connection libpq has found gone (PQstatus) - a session
      // the server ended, met by the statement sent after it - or one Disconnect
      // has finished. PQtransactionStatus answers PQTRANS_UNKNOWN for it, not
      // PQTRANS_INERROR, so that IsTransactionFailed does not say it.
      virtual bool IsSessionGone_() const;

   private:

  
      PGconn *dbconn_;

      // The SQLSTATE of the last statement that failed, or empty: kept by
      // CheckError, which is const and which PGRecordset shares.
      mutable std::string last_sql_state_;

      static std::atomic<bool> standard_conforming_strings_;

      bool is_connected_;
   };

   // PGConnection::MakeConninfo held to libpq's own reading of what it writes
   // (PQconninfoParse): hostile passwords, users and database names read back
   // byte for byte, and none of them moving the host, the port or sslmode. Throws
   // std::logic_error with a sentence. ClassTester runs it inside the Windows
   // service; connection-string-tests on Linux.
   class PGConnectionTester
   {
   public:
      void Test();
   };

}
