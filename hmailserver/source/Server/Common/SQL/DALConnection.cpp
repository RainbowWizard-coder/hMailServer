// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"

#include "DALConnection.h"
#include "DatabaseConnectionManager.h"
#include "DatabaseSettings.h"
#include "DatabaseTls.h"
#include "../Cache/CacheContainer.h"
#include "../Util/Unicode.h"

#include <stdexcept>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   DALConnection::DALConnection(std::shared_ptr<DatabaseSettings> pDatabaseSettings) :
      database_settings_(pDatabaseSettings),
      try_count_(6),
      transport_encrypted_(false),
      transaction_open_(false),
      transaction_session_lost_(false)
   {

   }

   DALConnection::~DALConnection()
   {

   }

   void
   DALConnection::SetTransportSecurity_(bool encrypted, const String &description)
   {
      transport_encrypted_ = encrypted;
      transport_security_ = description;

      DatabaseTls::NoteConnection(description);
   }

   bool
   DALConnection::Execute(const SQLCommand &command, String &sErrorMessage, __int64 *iInsertID, int iIgnoreErrors, __int64 *rowsAffected, bool *reconnected)
   {
      if (rowsAffected != nullptr)
         *rowsAffected = -1;

      if (reconnected != nullptr)
         *reconnected = false;

      SQLCommand commandCopy = command;

      if (!GetSupportsCommandParameters() && command.GetParameters().size() > 0)
      {
         SQLStatement statement;
         String nonParameterQueryString = statement.GenerateFromCommand(command);

         commandCopy.ClearParameters();
         commandCopy.SetQueryString(nonParameterQueryString);
      }

      // What the statement failed with when the connection was lost under it.
      String lostWith;

      int iNoOfTries = 6;
      for (int i = 1 ; i <= try_count_; i++)
      {
         // Not run on a session that has replaced the one a transaction
         // BeginTransaction began was on - the statement that met the lost
         // connection, run again, included: it would be committed on its own,
         // outside the transaction, which the database rolled back with the session
         // (IsTransactionSessionLost). The caller is told why, and reports it.
         if (transaction_session_lost_)
         {
            sErrorMessage = TransactionSessionLostError(lostWith);
#ifndef _ERROR_LOGGING_IN_MESSAGE_BOXES
            LOG_APPLICATION(_T("DALConnection - ") + sErrorMessage);
#endif
            if (rowsAffected != nullptr)
               *rowsAffected = -1;
            return false;
         }

         if (i == 2 || i == 4)
         {
            // Try to reconnect
            Reconnect(sErrorMessage);
         }

         DALConnection::ExecutionResult result = TryExecute(commandCopy, sErrorMessage, iInsertID, iIgnoreErrors, rowsAffected);
         if (result == DALConnection::DALSuccess)
         {
            sErrorMessage = "";
            return true;
         }

         if (result != DALConnection::DALConnectionProblem)
            break;

         // There's been a connection problem. Try to reconnect. Whatever happens
         // next, the statement may already have been run: the connection can have
         // gone after the database committed it and before its answer arrived, and
         // the next attempt sends it again (Execute's declaration).
         if (reconnected != nullptr)
            *reconnected = true;

         lostWith = sErrorMessage;

         Sleep(1000);
         Reconnect(sErrorMessage);
      }

      if (rowsAffected != nullptr)
         *rowsAffected = -1;

      // We've reached the maximum number of 
      // reties without success. Time to give up.
      ErrorManager::Instance()->ReportError(ErrorManager::High, 5032, "DALConnection::Execute", sErrorMessage);

      return false;
   }

   bool
   DALConnection::ClientBytes(const String &value, AnsiString &bytes)
   {
      bytes.Empty();

#ifndef HM_PLATFORM_POSIX
      // DALConnection.h says why: the bytes such a value has always gone as here.
      bool latin1 = true;
      for (int i = 0; i < value.GetLength() && latin1; i++)
         latin1 = (unsigned int) value[i] <= 0xFF;

      if (latin1)
      {
         std::string narrow;
         narrow.reserve((size_t) value.GetLength());
         for (int i = 0; i < value.GetLength(); i++)
            narrow += (char) (unsigned char) value[i];
         bytes = narrow.c_str();
         return true;
      }
#endif

      return Unicode::ToUtf8Exact(value, bytes);
   }

   int
   DALConnection::EndOfLiteral(const String &sql, int open, bool backslashEscapes)
   {
      const int length = sql.GetLength();

      bool escapes = backslashEscapes;

      if (!escapes && open > 0 && (sql[open - 1] == _T('E') || sql[open - 1] == _T('e')))
      {
         // PostgreSQL's escape string, unless the E ends a longer word.
         bool endsWord = false;

         if (open > 1)
         {
            const TCHAR before = sql[open - 2];
            endsWord = (before >= _T('a') && before <= _T('z')) || (before >= _T('A') && before <= _T('Z')) ||
                       (before >= _T('0') && before <= _T('9')) || before == _T('_') || before == _T('$') ||
                       (unsigned int) before > 127;
         }

         escapes = !endsWord;
      }

      for (int i = open + 1; i < length; i++)
      {
         const TCHAR c = sql[i];

         if (escapes && c == _T('\\'))
         {
            // The character after it cannot close the literal.
            i++;
            continue;
         }

         if (c == _T('\''))
         {
            if (i + 1 < length && sql[i + 1] == _T('\''))
            {
               // Doubled '' - still inside the literal.
               i++;
               continue;
            }

            return i;
         }
      }

      return length;
   }

   bool
   DALConnection::HasIgnoreErrorsMarker(const String &sql, bool backslashEscapes)
   {
      // See the comment on the declaration in DALConnection.h for why this is a
      // scan rather than a Find().
      const String marker = _T("[IGNORE-ERRORS]");
      const int markerLength = marker.GetLength();
      const int length = sql.GetLength();

      bool inLineComment = false;
      bool inBlockComment = false;

      for (int i = 0; i < length; i++)
      {
         TCHAR c = sql[i];

         if (inLineComment)
         {
            if (c == _T('\n'))
            {
               inLineComment = false;
               continue;
            }
         }
         else if (inBlockComment)
         {
            if (c == _T('*') && i + 1 < length && sql[i + 1] == _T('/'))
            {
               inBlockComment = false;
               i++;
               continue;
            }
         }
         else
         {
            if (c == _T('\''))
            {
               // Past the literal, to its closing quote (the loop steps over it).
               i = EndOfLiteral(sql, i, backslashEscapes);
               continue;
            }

            if (c == _T('-') && i + 1 < length && sql[i + 1] == _T('-'))
            {
               inLineComment = true;
               i++;
               continue;
            }

            if (c == _T('/') && i + 1 < length && sql[i + 1] == _T('*'))
            {
               inBlockComment = true;
               i++;
               continue;
            }

            // Ordinary statement text. The marker does not count here: it is a
            // directive to the DAL, not SQL, so it can only legitimately live in
            // a comment.
            continue;
         }

         // Inside a comment.
         if (c == _T('[') && i + markerLength <= length &&
             sql.Mid(i, markerLength).Compare(marker.c_str()) == 0)
            return true;
      }

      return false;
   }

   void
   DALConnection::ApplyDefaultTimeout()
   {
      SetTimeout(IniFileSettings::Instance()->GetDatabaseStatementTimeout());
   }

   void
   DALConnection::SetTransactionOpen_(bool open)
   {
      transaction_open_ = open;
      transaction_session_lost_ = false;
   }

   bool
   DALConnection::IsTransactionSessionGone() const
   {
      return transaction_session_lost_ || (transaction_open_ && IsSessionGone_());
   }

   bool
   DALConnection::IsTransactionEndedIntoAutocommit() const
   {
      return IsTransactionSessionGone() || (!GetFailedStatementEndsTransaction() && IsTransactionFailed());
   }

   String
   DALConnection::TransactionSessionLostError(const String &lostWith)
   {
      String error = _T("The statement was not run on a new connection: the connection to the database was lost inside the transaction it belongs to, and the database rolled back what the transaction held with the session; on a new connection the statement would be saved on its own, outside the transaction.");
      if (!lostWith.IsEmpty())
         error += _T(" The connection was lost with: ") + lostWith;
      return error;
   }

   bool
   DALConnection::Reconnect(String &sErrorMessage)
   {
#ifndef _ERROR_LOGGING_IN_MESSAGE_BOXES
      LOG_APPLICATION("DALConnection - Reconnecting to the database");
#endif

      // A transaction BeginTransaction began goes with the session it is on: the
      // database rolls back what it held, and the new session holds none.
      // IsTransactionSessionLost says so until the transaction's end, and nothing
      // is run on the new session in its place (Execute, DALRecordset::Open) - but
      // what Connect itself runs to set the new session up, its queries among them
      // (MySQLConnection::LoadSupportsTransactions_ opens a recordset), which a
      // refusal there would leave unread: so it is said once Connect has returned.
      const bool transactionLost = transaction_open_;
      transaction_session_lost_ = false;

      // Disconnect first...
      Disconnect();

      // ... and now try to reconnect.
      bool connected = false;
      try
      {
         connected = Connect(sErrorMessage) == Connected;
      }
      catch (...)
      {
         // A connect cut part way - by a stop's interruption passed on through a
         // report it made (WorkQueue::Stop), which DatabaseConnectionManager's
         // HeldConnection_ then gives back to the pool - is left closed, as a
         // refused one is, rather than half made: a session that has not been
         // told its character set, database or timeout. The next statement on it
         // finds it closed and connects again.
         boost::this_thread::disable_interruption disabled;
         transaction_session_lost_ = transactionLost;
         Disconnect();
         throw;
      }

      transaction_session_lost_ = transactionLost;

      if (!connected)
         return false;

      CacheContainer::Instance()->Clear();

      return true;
   }

   void
   SqlLiteralScanTester::Test()
   {
      struct Engine
      {
         const char *name;
         bool backslashEscapes;
         const TCHAR *prefix;
      };

      // Each engine's reading of a literal, and the prefix it is written with:
      // N'...' on SQL Server and Compact (PersistentMessageMetaData::SaveObject).
      const Engine engines[] =
      {
         { "MySQL", true, _T("") },
         { "PostgreSQL with standard_conforming_strings off", true, _T("") },
         { "PostgreSQL with standard_conforming_strings on", false, _T("") },
         { "SQL Server and Compact", false, _T("N") },
      };

      const TCHAR *values[] =
      {
         _T(""), _T("plain"), _T("ends in a backslash\\"), _T("\\"), _T("\\\\"), _T("a\\'b"), _T("'"), _T("''"),
         _T("it's"), _T("/* [IGNORE-ERRORS] */"), _T("-- [IGNORE-ERRORS]"), _T("*/ [IGNORE-ERRORS] /*"),
         _T("\\' /* [IGNORE-ERRORS] */ '"), _T("x\\\\' -- [IGNORE-ERRORS]"), _T("E'\\' /* [IGNORE-ERRORS] */"),
      };

      // No marker, the marker in a block comment, and in a line comment.
      const TCHAR *suffixes[] = { _T(""), _T(" /* [IGNORE-ERRORS] */"), _T("\n--- [IGNORE-ERRORS]") };

      for (const Engine &engine : engines)
      {
         const String masked = String(engine.prefix) + _T("'?'");

         for (const TCHAR *first : values)
         {
            for (const TCHAR *second : values)
            {
               for (int s = 0; s < 3; s++)
               {
                  // Two values as Escape writes them for the engine, the way
                  // SqlLogDevice's batch and the sort's copy of the headers put
                  // theirs into a statement.
                  const String sql = _T("insert into hm_log (a, b, c) values (") +
                     String(engine.prefix) + _T("'") + SQLStatement::EscapeFor(first, engine.backslashEscapes) + _T("', ") +
                     String(engine.prefix) + _T("'") + SQLStatement::EscapeFor(second, engine.backslashEscapes) + _T("', 5)") +
                     suffixes[s];
                  const String expected = _T("insert into hm_log (a, b, c) values (") + masked + _T(", ") + masked +
                     _T(", 5)") + suffixes[s];

                  if (DALConnection::HasIgnoreErrorsMarker(sql, engine.backslashEscapes) != (s != 0))
                     throw std::logic_error(std::string("SqlLiteralScanTester: on ") + engine.name + ", the marker is " +
                        (s != 0 ? "not found in [" : "found in [") + Unicode::ToUtf8(sql).c_str() + "]");

                  const String redacted = DatabaseConnectionManager::RedactSqlLiterals_(sql, engine.backslashEscapes);

                  if (redacted.Compare(expected) != 0)
                     throw std::logic_error(std::string("SqlLiteralScanTester: on ") + engine.name + ", [" +
                        Unicode::ToUtf8(sql).c_str() + "] is masked as [" + Unicode::ToUtf8(redacted).c_str() + "]");
               }
            }
         }
      }

      struct Reading
      {
         const TCHAR *sql;
         bool backslashEscapes;
         bool marked;
         const TCHAR *redacted;
      };

      // Statements read differently by the engines that take a backslash for an
      // escape and those that do not; PostgreSQL's E'...', which takes one on
      // every setting, and a word ending in an e before a literal, which is no
      // E'...'; a literal nothing closes.
      const Reading readings[] =
      {
         { _T("select 'a\\' /* [IGNORE-ERRORS] */"), false, true, _T("select '?' /* [IGNORE-ERRORS] */") },
         { _T("select 'a\\' /* [IGNORE-ERRORS] */"), true, false, _T("select '?") },
         { _T("select E'it\\'s', 'b' /* [IGNORE-ERRORS] */"), false, true, _T("select E'?', '?' /* [IGNORE-ERRORS] */") },
         { _T("select e'it\\'s /* [IGNORE-ERRORS] */'"), false, false, _T("select e'?'") },
         { _T("select 1 where a like'x\\' /* [IGNORE-ERRORS] */"), false, true, _T("select 1 where a like'?' /* [IGNORE-ERRORS] */") },
         { _T("select 'it''s' -- [IGNORE-ERRORS]"), false, true, _T("select '?' -- [IGNORE-ERRORS]") },
         { _T("select 'abc"), false, false, _T("select '?") },
         { _T("select '"), true, false, _T("select '") },
      };

      for (const Reading &reading : readings)
      {
         const String sql = reading.sql;

         if (DALConnection::HasIgnoreErrorsMarker(sql, reading.backslashEscapes) != reading.marked)
            throw std::logic_error(std::string("SqlLiteralScanTester: the marker is ") +
               (reading.marked ? "not found in [" : "found in [") + Unicode::ToUtf8(sql).c_str() + "]" +
               (reading.backslashEscapes ? " with backslash escapes" : " without backslash escapes"));

         const String redacted = DatabaseConnectionManager::RedactSqlLiterals_(sql, reading.backslashEscapes);

         if (redacted.Compare(reading.redacted) != 0)
            throw std::logic_error(std::string("SqlLiteralScanTester: [") + Unicode::ToUtf8(sql).c_str() +
               "] is masked as [" + Unicode::ToUtf8(redacted).c_str() + "]" +
               (reading.backslashEscapes ? " with backslash escapes" : " without backslash escapes"));
      }
   }

}