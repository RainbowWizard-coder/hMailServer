// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "DALConnection.h"
#include "DatabaseSettings.h"
#include "DatabaseTls.h"
#include "../Cache/CacheContainer.h"
#include "../Util/Unicode.h"

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

   bool
   DALConnection::HasIgnoreErrorsMarker(const String &sql)
   {
      // See the comment on the declaration in DALConnection.h for why this is a
      // scan rather than a Find().
      const String marker = _T("[IGNORE-ERRORS]");
      const int markerLength = marker.GetLength();
      const int length = sql.GetLength();

      bool inLiteral = false;
      bool inLineComment = false;
      bool inBlockComment = false;

      for (int i = 0; i < length; i++)
      {
         TCHAR c = sql[i];

         if (inLiteral)
         {
            if (c == _T('\\') && i + 1 < length)
            {
               // Backslash escape (MySQL default sql_mode): the next character
               // cannot close the literal.
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

               inLiteral = false;
            }

            continue;
         }

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
               inLiteral = true;
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
   DALConnection::IsTransactionEndedIntoAutocommit() const
   {
      return transaction_session_lost_ || (!GetFailedStatementEndsTransaction() && IsTransactionFailed());
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
      const bool connected = Connect(sErrorMessage) == Connected;

      transaction_session_lost_ = transactionLost;

      if (!connected)
         return false;

      CacheContainer::Instance()->Clear();

      return true;
   }



}