// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"

#include "MySQLConnection.h"
#include "MySQLRecordset.h"
#include "DatabaseSettings.h"
#include "../Application/IniFileSettings.h"
#include "Macros/MySQLMacroExpander.h"
#include "../Util/Unicode.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   MySQLConnection::MySQLConnection(std::shared_ptr<DatabaseSettings> pSettings) :
      DALConnection(pSettings)
   {
      is_connected_ = false;
      dbconn_ = 0;
      supports_transactions_ = false;
      timeout_variable_is_seconds_ = false;
      server_is_mysql_ = false;
      transaction_ended_ = false;
      lock_wait_timed_out_ = false;
      rollback_on_timeout_ = true;
   }

   MySQLConnection::~MySQLConnection()
   {
      try
      {
         if (dbconn_)
         {
            MySQLInterface::Instance()->p_mysql_close(dbconn_);
            dbconn_ = 0;
         }
      }
      catch (...)
      {

      }

   }

   DALConnection::ConnectionResult
   MySQLConnection::Connect(String &sErrorMessage)
   {
      if (!MySQLInterface::Instance()->IsLoaded())
      {
         // Load the MySQL interface.
         if (!MySQLInterface::Instance()->Load(sErrorMessage))
         {
            // Loading failed
            return FatalError;
         }
      }

      try
      {
         String sUsername = database_settings_->GetUsername();
         String sPassword = database_settings_->GetPassword();
         String sServer = database_settings_->GetServer();
         String sDatabase = database_settings_->GetDatabaseName();
         long lDBPort = database_settings_->GetPort();

         if (lDBPort == 0)
            lDBPort = 3306;

         // Each value as the client is handed it (DALConnection::ClientBytes),
         // refused here, before a connection is tried, when it cannot be written so.
         // Up to 6.3.5 Unicode::ToANSI wrote them, which on Linux made a value with
         // any character outside ASCII empty: the password reached the server as
         // none, and the user as the client's own default.
         const String *const values[] = { &sServer, &sUsername, &sPassword, &sDatabase };
         for (const String *value : values)
         {
            AnsiString bytes;
            if (!ClientBytes(*value, bytes))
            {
               sErrorMessage = _T("The database server, user, password or database name in hMailServer.ini holds a character UTF-8 cannot carry (half of a surrogate pair), so it cannot be passed to MySQL. The connection was not attempted.");
               return FatalError;
            }
         }

         // [Database] TlsMode and TlsCaFile (DatabaseTls.h). A mode this server does
         // not know, or a CA file verify-full cannot read, is a configuration to
         // correct rather than a server to wait for: nothing is attempted, and the
         // connection manager does not try again.
         DatabaseTls::Mode tlsMode = DatabaseTls::ModeDefault;
         String tlsCaFile;
         if (!DatabaseTls::FromConfiguration(DatabaseSettings::TypeMYSQLServer, tlsMode, tlsCaFile, sErrorMessage))
            return FatalError;

         const DatabaseTls::Mode effectiveMode = DatabaseTls::Effective(tlsMode);

         unsigned int errorNumber = 0;
         hm_MYSQL *pResult = ConnectOnce_(sServer, sUsername, sPassword, sDatabase, lDBPort, effectiveMode, tlsCaFile, sErrorMessage, errorNumber);

         // prefer, the way libpq means it: TLS the two ends cannot agree on - a server
         // that offers only TLS 1.0 or 1.1 to a client that no longer speaks them, the
         // usual case - is a connection without TLS rather than no connection, which is
         // what a server that worked before TlsMode existed has to go on getting. Only
         // for the client's TLS error: a refused login is never tried a second time.
         String tlsNotAgreed;
         if (pResult == 0 && effectiveMode == DatabaseTls::ModePrefer && errorNumber == HM_MYSQL_CR_SSL_CONNECTION_ERROR)
         {
            tlsNotAgreed = sErrorMessage;
            pResult = ConnectOnce_(sServer, sUsername, sPassword, sDatabase, lDBPort, DatabaseTls::ModeDisable, String(), sErrorMessage, errorNumber);

            if (pResult == 0)
               sErrorMessage = _T("With TLS: ") + tlsNotAgreed + _T(" Without TLS: ") + sErrorMessage;
         }

         if (pResult == 0)
         {
            // From MySQL manual:
            //
            // Return Values:
            //
            // A MYSQL* connection handle if the connection was successful, NULL if the connection was
            // unsuccessful. For a successful connection, the return value is the same as the value
            // of the first parameter.
            //
            // ConnectOnce_ has put the client's own words in sErrorMessage. The bundled
            // MariaDB Connector/C client ships the auth plugins for every
            // common MySQL/MariaDB account type, so "Authentication plugin '<x>' cannot
            // be loaded" should now only happen if the Bin\plugin folder is missing or
            // incomplete (a broken install). GSSAPI/SSPI "Windows authentication"
            // additionally requires the hMailServer service's Windows identity to match
            // the database account, which a service running as LocalSystem/NT SERVICE
            // normally will not. In either case, translate the raw client error into
            // actionable guidance instead of dead-ending the setup wizard.
            String sLowerError = sErrorMessage;
            sLowerError.MakeLower();

            // The client's own wording says what happened with TLS but not which
            // setting to change.
            if (errorNumber == HM_MYSQL_CR_SSL_CONNECTION_ERROR || !tlsNotAgreed.IsEmpty())
               sErrorMessage += _T("\r\n\r\n") + DatabaseTls::Guidance(DatabaseSettings::TypeMYSQLServer, tlsMode);

            // Access denied on a connection made without TLS is also what an account
            // created with REQUIRE SSL answers, and the server does not say which.
            if (errorNumber == 1045u && (effectiveMode == DatabaseTls::ModeDisable || !tlsNotAgreed.IsEmpty()))
            {
               sErrorMessage += _T("\r\n\r\n"
                  "The connection was made without TLS. If the database account requires it (CREATE USER ... REQUIRE SSL), "
                  "the server refuses it this way: set TlsMode under [Database] in hMailServer.ini to prefer or stronger.");
            }

            if (sLowerError.Find(_T("plugin")) >= 0 &&
                (sLowerError.Find(_T("cannot be loaded")) >= 0 || sLowerError.Find(_T("gssapi")) >= 0))
            {
               sErrorMessage += _T("\r\n\r\n"
                  "The database account uses an authentication method this hMailServer install could not "
                  "complete. If the message mentions a plugin that 'cannot be loaded', the Bin\\plugin "
                  "folder shipped with hMailServer is missing - reinstall/repair so the bundled MariaDB "
                  "Connector/C auth plugins are present. If it mentions GSSAPI/Windows authentication, the "
                  "hMailServer service's Windows identity must match the database account; the simplest fix "
                  "is to connect with a normal user name and password instead. To convert an account to "
                  "password authentication on the server:\r\n"
                  "    ALTER USER 'user'@'host' IDENTIFIED WITH mysql_native_password BY 'password';   -- MySQL\r\n"
                  "    ALTER USER 'user'@'host' IDENTIFIED VIA mysql_native_password USING PASSWORD('password');   -- MariaDB");
            }

            return TemporaryFailure;
         }

         if (CheckError(pResult, "mysql_real_connect()", sErrorMessage) != DALConnection::DALSuccess)
            return TemporaryFailure;

         // What the session is, asked of the client: the cipher of its TLS session, or
         // none. require is enforced here on the MariaDB client, whose options cannot
         // say "TLS or nothing" without also checking the certificate (ApplyTlsOptions_)
         // - so the login has been made by now, and the connection is closed before a
         // statement crosses it. The Oracle client's REQUIRED and both clients'
         // verification refuse before the login, and are asked again here all the same.
         MySQLInterface *client = MySQLInterface::Instance();
         const bool cipherKnown = client->p_mysql_get_ssl_cipher != 0;
         const char *cipher = cipherKnown ? client->p_mysql_get_ssl_cipher(dbconn_) : nullptr;
         const bool encrypted = cipher != nullptr && cipher[0] != 0;
         const bool tlsRequired = effectiveMode == DatabaseTls::ModeRequire || effectiveMode == DatabaseTls::ModeVerifyFull;

         if (tlsRequired && ((cipherKnown && !encrypted) || (!cipherKnown && effectiveMode == DatabaseTls::ModeRequire)))
         {
            sErrorMessage = cipherKnown
               ? Formatter::Format("The MySQL/MariaDB server at {0} did not offer TLS, and TlsMode is {1}: the connection was closed before it was used.",
                    sServer, DatabaseTls::ModeName(tlsMode))
               : Formatter::Format("The MySQL client library cannot say whether the connection to {0} is encrypted (it has no mysql_get_ssl_cipher), "
                    "and TlsMode is {1}: the connection was closed before it was used.", sServer, DatabaseTls::ModeName(tlsMode));
            sErrorMessage += _T("\r\n\r\n") + DatabaseTls::Guidance(DatabaseSettings::TypeMYSQLServer, tlsMode);

            client->p_mysql_close(dbconn_);
            dbconn_ = 0;
            return TemporaryFailure;
         }

         String transportDetail;
         if (encrypted)
            transportDetail = String(cipher);
         else if (!tlsNotAgreed.IsEmpty())
            transportDetail = _T("TLS could not be agreed on (") + tlsNotAgreed + _T("), so the connection was made without it");
         else if (effectiveMode == DatabaseTls::ModeDisable)
            transportDetail = _T("TLS is switched off");
         else
            transportDetail = _T("the server offered no TLS");

         SetTransportSecurity_(encrypted, DatabaseTls::Describe(DatabaseSettings::TypeMYSQLServer, encrypted, transportDetail,
            encrypted && effectiveMode == DatabaseTls::ModeVerifyFull, tlsMode));

         SetConnectionCharacterSet_();

         if (!sDatabase.IsEmpty())
         {
            String switch_db_command = "use " + QuoteIdentifier(sDatabase);

            if (TryExecute(SQLCommand(switch_db_command), sErrorMessage, 0, 0) != DALConnection::DALSuccess)
            {
               return TemporaryFailure;
            }
         }

         LoadSupportsTransactions_(sDatabase);
         LoadRollbackOnTimeout_();

         is_connected_ = true;

         // Both of these run statements, so they go after is_connected_. The
         // mechanism is worked out once per connection and then applied; the session
         // variable dies with the session, so Reconnect() - which calls Connect() -
         // is what puts it back after a dropped connection.
         LoadTimeoutMechanism_();
         ApplyDefaultTimeout();
      }
      catch (const boost::thread_interrupted&)
      {
         // A stop's interruption, not a fault: see WorkQueue::Stop. Met in a report
         // made inside the connect (DatabaseTls' reading of TlsMode, the character
         // set's, the TryExecute and recordsets of the session's set-up), whose OnError
         // script's call to the server can meet the stop. Passed on, with the
         // connection closed, as a refused connect leaves it, rather than half made;
         // DALConnection::Reconnect and DatabaseConnectionManager's HeldConnection_ do
         // the rest.
         boost::this_thread::disable_interruption disabled;
         Disconnect();
         LOG_DEBUG("MySQLConnection::Connect: a stop of the servers interrupted the connection to the database, which is left closed.");
         throw;
      }
      catch (...)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5008, "MySQLConnection::Connect", "An unhandled error occurred when connecting to the database.");
         return TemporaryFailure;
      }

      return Connected;
   }

   hm_MYSQL *
   MySQLConnection::ConnectOnce_(const String &server, const String &username, const String &password, const String &database, long port,
                                 DatabaseTls::Mode mode, const String &caFile, String &error, unsigned int &errorNumber)
   {
      MySQLInterface *client = MySQLInterface::Instance();

      error.Empty();
      errorNumber = 0;

      // A second attempt starts from a fresh handle: the options of the first stay
      // set on the old one, which a failed mysql_real_connect leaves for mysql_close.
      if (dbconn_)
      {
         client->p_mysql_close(dbconn_);
         dbconn_ = 0;
      }

      dbconn_ = client->p_mysql_init(NULL);
      if (dbconn_ == 0)
      {
         error = _T("The MySQL client library could not allocate a connection (mysql_init returned nothing).");
         return 0;
      }

      // Point the client authentication-plugin directory at the "plugin" sub-folder
      // of the Bin directory, where the bundled MariaDB Connector/C auth plugins
      // ship (caching_sha2_password for MySQL 8.0+, client_ed25519 / auth_gssapi_client
      // / parsec for MariaDB, sha256_password, dialog, ...). This lets the one bundled
      // client authenticate against essentially any MySQL or MariaDB account type the
      // user has configured, instead of failing with "Authentication plugin '<x>'
      // cannot be loaded". The default auth plugin is intentionally NOT forced, so
      // the client negotiates whatever the account actually uses. The plugins are
      // MariaDB's and the option number is MariaDB's (MySQLInterface.h), so an Oracle
      // libmysql.dll put in the bundled one's place is not handed either.
#ifndef HM_PLATFORM_POSIX
      if (client->p_mysql_options != 0 && client->IsMariaDbClient())
      {
         AnsiString sPluginDir = Unicode::ToANSI(client->GetLibraryDirectory() + _T("\\plugin"));
         client->p_mysql_options(dbconn_, HM_MYSQL_PLUGIN_DIR, sPluginDir.c_str());
      }
#else
      // Nothing is pointed anywhere on POSIX, and that is the whole of the
      // POSIX behaviour rather than an omission. The plugin directory above
      // exists because Windows carries its own copy of the client in the Bin
      // directory with the plugins in a sub-folder beside it; here the client
      // is a packaged shared object whose plugin directory was compiled into
      // it by the packager and is already right. Overriding it with a path
      // built from a Windows separator would take the authentication plugins
      // away, which is the failure this branch exists to avoid.
#endif

      if (!ApplyTlsOptions_(mode, caFile, error))
         return 0;

      // Connect has refused a value ClientBytes cannot write.
      AnsiString serverText, usernameText, passwordText, databaseText;
      ClientBytes(server, serverText);
      ClientBytes(username, usernameText);
      ClientBytes(password, passwordText);
      ClientBytes(database, databaseText);

      // CLIENT_FOUND_ROWS: mysql_affected_rows counts the rows an UPDATE matched, as
      // ADO and libpq count them, where without it MySQL and MariaDB count only the
      // rows whose values it changed - and an UPDATE that sets what a row already
      // holds (a lease renewed by its holder, a claim run again after a reconnect)
      // counts 0, which a claim reads as lost (measured on MySQL 8.4 and MariaDB
      // 11.8 on 12 October 2026, build/lab/active-active/sqlcheck.py). It changes
      // nothing else the server reads: only the count, which until TryExecute's
      // rowsAffected nothing asked for, and not mysql_insert_id.
      hm_MYSQL *result = client->p_mysql_real_connect(
                  dbconn_,
                  serverText.c_str(),
                  usernameText.c_str(),
                  passwordText.c_str(),
                  databaseText.c_str(), port, 0, HM_MYSQL_CLIENT_FOUND_ROWS);

      if (result == 0)
      {
         const char *clientError = client->p_mysql_error(dbconn_);
         error = clientError != nullptr ? String(clientError) : String(_T("mysql_real_connect failed and the client gave no reason."));
         errorNumber = client->p_mysql_errno(dbconn_);
      }

      return result;
   }

   bool
   MySQLConnection::ApplyTlsOptions_(DatabaseTls::Mode mode, const String &caFile, String &error)
   {
      MySQLInterface *client = MySQLInterface::Instance();

      const bool mustApply = mode == DatabaseTls::ModeRequire || mode == DatabaseTls::ModeVerifyFull;

      if (client->p_mysql_options == 0)
      {
         if (!mustApply)
            return true;

         error = Formatter::Format("The MySQL client library has no mysql_options, so TlsMode={0} cannot be applied to it. The connection was not attempted.",
            DatabaseTls::ModeName(mode));
         return false;
      }

      // The file name as the client opens it, in the system's code page as the
      // plugin directory is: an administrator should keep it to ASCII.
      const AnsiString caFileName = caFile.IsEmpty() ? AnsiString() : Unicode::ToANSI(caFile);
      int refusedOption = 0;

      if (client->IsMariaDbClient())
      {
         // MariaDB Connector/C, as measured on 25 September 2026 against MySQL 8.4 and
         // MariaDB 11.4 with the Windows build hMailServer bundled then (3.4.9) and
         // Ubuntu 26.04's alike (docs/DatabaseTls.md has the table). What this function
         // sets - the first three rows - was measured again through hMailServer on
         // 12 October 2026 against MySQL 8.4 and MariaDB 11.8, twenty cases, 3.4.9
         // beside 3.4.11 (the build bundled since 6.3.7), each case alike on both. The
         // last row, which this function never sets (the CA file reaches it for
         // verify-full alone: DatabaseTls::FromConfiguration), was not measured again;
         // it holds for 3.4.11 by its source, 3.4.10 having read the file there and
         // checked the certificate, and 3.4.11 having taken that back (CONC-854).
         //
         //   ENFORCE 0, VERIFY 0   no TLS - and what both builds did when asked
         //                         nothing, which is why both are set every time
         //   ENFORCE 1, VERIFY 0   TLS when the server offers it and plaintext when it
         //                         does not: prefer. require is therefore checked
         //                         after the connection is made (Connect).
         //   ENFORCE 1, VERIFY 1   TLS or no connection, the certificate's chain and
         //                         its name checked; a server without TLS is refused
         //                         before the login ("SSL is required, but the server
         //                         does not support it"). With no CA file the system's
         //                         store is used - and a MariaDB 11.4 server's own
         //                         certificate is accepted when the password proves
         //                         it, which is MariaDB's design, not a gap.
         //   a CA file beside VERIFY 0 is not read at all.
         char enforce = 1;
         if (mode == DatabaseTls::ModeDisable)
            enforce = 0;

         char verify = 0;
         if (mode == DatabaseTls::ModeVerifyFull)
            verify = 1;

         if (client->p_mysql_options(dbconn_, HM_MYSQL_OPT_SSL_ENFORCE, &enforce) != 0)
            refusedOption = HM_MYSQL_OPT_SSL_ENFORCE;
         else if (client->p_mysql_options(dbconn_, HM_MARIADB_OPT_SSL_VERIFY_SERVER_CERT, &verify) != 0)
            refusedOption = HM_MARIADB_OPT_SSL_VERIFY_SERVER_CERT;
         else if (!caFileName.IsEmpty() && client->p_mysql_options(dbconn_, HM_MARIADB_OPT_SSL_CA, caFileName.c_str()) != 0)
            refusedOption = HM_MARIADB_OPT_SSL_CA;
      }
      else
      {
         // The Oracle client says all of it with one option, and says it before the
         // login: DISABLED, PREFERRED (its own default), REQUIRED, VERIFY_IDENTITY -
         // in the numbers of an 8.x client. Any other version is told nothing: its
         // own default stands for prefer and disable, and require and verify-full,
         // which must not be claimed without being applied, are refused.
         const unsigned long clientVersion = client->p_mysql_get_client_version != 0 ? client->p_mysql_get_client_version() : 0;
         if (clientVersion < 80000 || clientVersion >= 90000)
         {
            if (!mustApply)
               return true;

            error = Formatter::Format("The MySQL client library loaded is neither MariaDB Connector/C nor Oracle's 8.x client (it reports version {0}), and "
               "this server does not know its TLS option numbers, so TlsMode={1} cannot be applied to it. The connection was not attempted.",
               (__int64) clientVersion, DatabaseTls::ModeName(mode));
            return false;
         }

         unsigned int sslMode = HM_ORACLE_SSL_MODE_PREFERRED;
         if (mode == DatabaseTls::ModeDisable)
            sslMode = HM_ORACLE_SSL_MODE_DISABLED;
         else if (mode == DatabaseTls::ModeRequire)
            sslMode = HM_ORACLE_SSL_MODE_REQUIRED;
         else if (mode == DatabaseTls::ModeVerifyFull)
            sslMode = HM_ORACLE_SSL_MODE_VERIFY_IDENTITY;

         if (client->p_mysql_options(dbconn_, HM_ORACLE_OPT_SSL_MODE, &sslMode) != 0)
            refusedOption = HM_ORACLE_OPT_SSL_MODE;
         else if (!caFileName.IsEmpty() && client->p_mysql_options(dbconn_, HM_ORACLE_OPT_SSL_CA, caFileName.c_str()) != 0)
            refusedOption = HM_ORACLE_OPT_SSL_CA;
      }

      if (refusedOption != 0 && mustApply)
      {
         error = Formatter::Format("The MySQL client library refused TLS option {0}, so TlsMode={1} cannot be applied to it. The connection was not attempted.",
            refusedOption, DatabaseTls::ModeName(mode));
         return false;
      }

      return true;
   }

   bool
   MySQLConnection::CheckServerVersion(String &errorMessage)
   {
      // check server version.
      int serverVersion = MySQLInterface::Instance()->p_mysql_get_server_version(dbconn_);
      if (serverVersion < RequiredVersion)
      {
         errorMessage = "hMailServer requires MySQL 4.1.18 or newer. If you are using the internal MySQL database, please upgrade to the latest 4.x version prior to upgrading to version 5 or later.";
         return false;
      }

      return true;
   }

   bool
   MySQLConnection::Disconnect()
   {
      if (dbconn_)
      {
         MySQLInterface::Instance()->p_mysql_close(dbconn_);
         dbconn_ = 0;
      }

      // Was left true, as PGConnection's was, so IsConnected() went on claiming a
      // connection that had been closed. Connect sets it again on the way back up.
      is_connected_ = false;

      return true;
   }

   DALConnection::ExecutionResult
   MySQLConnection::TryExecute(const SQLCommand &command, String &sErrorMessage, __int64 *iInsertID, int iIgnoreErrors, __int64 *rowsAffected)
   {
      if (rowsAffected != nullptr)
         *rowsAffected = -1;

      String SQL = command.GetQueryString();
      try
      {
         // mysql_query-doc:
         // Zero if the query was successful. Non-zero if an error occurred.
         //
         AnsiString sQuery;
         if (!Unicode::WideToMultiByte(SQL, sQuery))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5105, "MySQLConnection::TryExecute", "Could not convert string into multi-byte.");
            return DALConnection::DALUnknown;
         }

         // No handle at all: Connect refused this connection - a TlsMode it could
         // not honour, a TlsCaFile it could not read, TLS required and not offered -
         // and closed what it had. A reconnection made by DALConnection::Execute
         // ends here too until Connect succeeds. A dropped connection, so Execute
         // tries to reconnect, rather than a query handed a null handle.
         if (dbconn_ == 0)
         {
            sErrorMessage = "Source: MySQLConnection::TryExecute, Description: the database connection is not open - the last attempt to connect was refused.";
            return DALConnection::DALConnectionProblem;
         }

         bool failureIgnored = false;

         if (MySQLInterface::Instance()->p_mysql_query(dbconn_, sQuery))
         {
            // Classified first, because whether the marker may discard this
            // failure depends on what the failure is. CheckError is the only
            // thing on this backend that recognises a dropped connection (2006
            // "server has gone away", 2013 "lost connection", and the TLS and
            // idle-timeout forms of the same); GetErrorType_
            // does not, so it cannot be used for that test. Its own error text
            // goes into a local so that a discarded failure does not leave a
            // message behind in the caller's buffer.
            String checkErrorMessage;
            DALConnection::ExecutionResult result = CheckError(dbconn_, SQL, checkErrorMessage);

            if (result != DALConnection::DALSuccess)
            {
               // "This object may already exist" - never "the server did not
               // hear the statement". See DALConnection::HasIgnoreErrorsMarker.
               bool ignoreByMarker = result != DALConnection::DALConnectionProblem &&
                                     HasIgnoreErrorsMarker(SQL, GetBackslashEscapesInLiterals());

               // A caller that passes DALErrorInSQL forgives a row the unique index
               // refused (1062) and nothing else (DALConnection::TryExecute), and
               // never a lost connection, which CheckError has said is one. This
               // was "GetErrorType_ & iIgnoreErrors": ExecutionResult is not a set
               // of bits - DALUnknown, 3, has DALErrorInSQL's bit - so every failure
               // but 1062 was forgiven too, a lost connection among them, and the
               // statement was answered as done, with no reconnect and no row
               // written (measured on MySQL 8.4 and MariaDB 11.8, 13 October 2026).
               bool ignoreByCaller = result != DALConnection::DALConnectionProblem &&
                                     (iIgnoreErrors & DALConnection::DALErrorInSQL) != 0 &&
                                     GetErrorType_(dbconn_) == DALConnection::DALErrorInSQL;

               if (!ignoreByMarker && !ignoreByCaller)
               {
                  sErrorMessage = checkErrorMessage;
                  return result;
               }

               failureIgnored = true;
            }
         }

         // The count, before mysql_store_result, which for a statement with a result
         // set puts that set's row count in its place. A failure the caller asked to
         // ignore changed no row (DALConnection.h); mysql_affected_rows says -1 after
         // one.
         __int64 count = -1;
         if (failureIgnored)
         {
            count = 0;
         }
         else if (MySQLInterface::Instance()->p_mysql_affected_rows != nullptr)
         {
            const unsigned long long affected = MySQLInterface::Instance()->p_mysql_affected_rows(dbconn_);
            if (affected != ~0ull)
               count = (__int64) affected;
         }

         hm_MYSQL_RES *pRes = MySQLInterface::Instance()->p_mysql_store_result(dbconn_); // should always be called after mysql_query

         if (pRes)
            MySQLInterface::Instance()->p_mysql_free_result(pRes);

         if (rowsAffected != nullptr)
            *rowsAffected = count;

         // Fetch insert id.
         //
         // iInsertID is a pointer and this is its null check. "> 0" is an ORDERED
         // comparison against a null pointer constant, which MSVC accepts and a
         // conforming compiler rejects; != nullptr is the same test spelt in a way
         // both accept, and answers the same for every value a caller can pass.
         //
         // None after a statement whose failure was ignored: MySQL documents
         // mysql_insert_id as undefined after an error, and libmariadb 3 hands
         // back the id of the connection's last successful insert - measured on
         // 1 October 2026 against MariaDB 11, an insert into another table, then
         // a duplicate refused with 1062: the duplicate's id read as the other
         // table's row's. A caller that ignores DALErrorInSQL reads an id of 0 as
         // "the row was there already" (PersistentGreyList::AddObject,
         // PersistentComplaint, DomainReports), as it reads it on the other three
         // backends.
         if (iInsertID != nullptr)
         {
            *iInsertID = failureIgnored ? 0 : MySQLInterface::Instance()->p_mysql_insert_id(dbconn_);
         }
      }
      catch (const boost::thread_interrupted&)
      {
         // A stop's interruption, not a fault: see WorkQueue::Stop. Met in the report
         // of a statement that could not be converted (HM5105), whose OnError script's
         // call to the server can meet the stop. Passed on, where the catch below
         // answered it as a failure of the statement that DALConnection::Execute then
         // reported as HM5032; nothing has been sent. DatabaseConnectionManager's
         // HeldConnection_ gives the connection back.
         boost::this_thread::disable_interruption disabled;
         if (rowsAffected != nullptr)
            *rowsAffected = -1;
         LOG_DEBUG("MySQLConnection::TryExecute: a stop of the servers interrupted a statement.");
         throw;
      }
      catch (...)
      {
         if (rowsAffected != nullptr)
            *rowsAffected = -1;

         sErrorMessage = "Source: MySQLConnection::TryExecute, Code: HM10048, Description: An unhandled error occurred while executing: " + SQL;
         return DALConnection::DALUnknown;
      }

      return DALConnection::DALSuccess;
   }

   bool
   MySQLConnection::IsConnected() const
   {
      return is_connected_;
   }

   hm_MYSQL*
   MySQLConnection::GetConnection() const
   {
      return dbconn_;
   }

   DALConnection::ExecutionResult
   MySQLConnection::GetErrorType_(hm_MYSQL *pSQL)
   {
      try
      {
         if (pSQL==NULL)
            return DALSuccess;

         int iErrNo = MySQLInterface::Instance()->p_mysql_errno(pSQL);

         switch (iErrNo)
         {
         case 0:
            return DALSuccess;
         case 1062: // ER_DUP_ENTRY - Message: Duplicate entry '%s' for key %d
            return DALErrorInSQL;
         default:
            return DALUnknown;
         }

         HM_ASSERT(0); // Should never get here
         return DALSuccess;
      }
      catch (...)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 4373, "MySQLConnection::_GetErrorNumber", "An error occurred while trying to retrieve error code from MySQL.");
         return DALErrorInSQL;
      }

      return DALSuccess;

   }

   DALConnection::ExecutionResult
   MySQLConnection::CheckError(hm_MYSQL *pSQL, const String &sAdditionalInfo, String &sOutputErrorMessage) const
   {
      try
      {
         if (pSQL==NULL)
            return DALConnection::DALSuccess;

         const char *pError = MySQLInterface::Instance()->p_mysql_error(pSQL);
         // An empty error string means the last call succeeded. This used to read
         // "!pError[0] != '\0'", which by precedence compares the negation
         // with zero and happened to say the same thing; GCC's warning was right
         // that nobody could tell.
         if (pError[0] == '\0')
            return DALConnection::DALSuccess;


         DALConnection::ExecutionResult result = DALConnection::DALUnknown;

         int errorCode = MySQLInterface::Instance()->p_mysql_errno(pSQL);
         switch (errorCode)
         {
         case 2006: // MySQL server has gone away
         case 2013: // Lost connection to MySQL server during query
         // A session the server has ended - killed, restarted, failed over - answers
         // the next statement over TLS with the TLS layer's end of file rather than
         // 2013 (measured through libmariadb on MySQL 8.4 and MariaDB 11.8, 13
         // October 2026). It means the connection is gone, as 2013 does, so the
         // statement is run again on a new one (DALConnection::Execute) where it
         // used to fail. CheckError is reached only for a statement or a connection
         // already made, so 2026 here is never the TLS refusal of a connect, which
         // ConnectOnce_ answers itself.
         case HM_MYSQL_CR_SSL_CONNECTION_ERROR:
            result = DALConnection::DALConnectionProblem;
            break;
         case 4031:
            // MySQL 8.0.24 and later, and not MariaDB: ER_CLIENT_INTERACTION_TIMEOUT,
            // which MySQL answers the first statement after closing an idle session
            // with (wait_timeout), TLS or not; the statement after that reads 2006.
            // The same, so the statement is run again on a new connection. On
            // MariaDB 4031 is ER_REFERENCED_TRG_DOES_NOT_EXIST, a statement's own
            // error (a CREATE TRIGGER that FOLLOWS a trigger there is not), which
            // must fail at once and not be run again six times on eight new
            // connections - so the server's fork, read from VERSION() at connect
            // (LoadTimeoutMechanism_), decides, and a server not known to be MySQL
            // has 4031 taken as the statement's failure.
            if (server_is_mysql_)
               result = DALConnection::DALConnectionProblem;
            break;
         }

         // A failure that has ended the transaction BeginTransaction began, which
         // IsTransactionFailed says from here on: read here, where the statements
         // TryExecute runs and the queries MySQLRecordset runs alike are answered,
         // and whether or not the caller or the [IGNORE-ERRORS] marker then forgives
         // the failure, since the transaction is gone either way. A lock wait that
         // timed out where innodb_rollback_on_timeout is on may have ended it or
         // not - a row lock's has, a metadata lock's has not - and IsTransactionFailed
         // asks the session which, when it is next asked.
         if (IsTransactionOpen_())
         {
            if (FailureEndsTransaction_((unsigned int) errorCode, result))
               transaction_ended_ = true;
            else if (errorCode == 1205 && rollback_on_timeout_)
               lock_wait_timed_out_ = true;
         }

         AnsiString sMySqlErrorAnsi = pError;
         String sMySQLErrorUnicode = sMySqlErrorAnsi;

         String sErrorMessage;
         sErrorMessage.Format(_T("MySQL: %s (Additional info: %s)"), sMySQLErrorUnicode.c_str(), sAdditionalInfo.c_str());

         sOutputErrorMessage = sErrorMessage;

         return result;
      }
      catch (...)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5009, "MySQLConnection::CheckError", "An unhandled error occurred while checking for errors.");
         return DALConnection::DALUnknown;
      }
   }

   void
   MySQLConnection::OnConnected()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // This would need refactoring some day. This is the place
   // where the internal MySQL database structure is managed.
   // The update of the data tables is taken care of by the
   // installation program, but the mysql.* tables are updated
   // here.
   //---------------------------------------------------------------------------()
   {
      // Check if the user is using the internal database. We don't rely
      // entirely on the [Database]->Internal setting in hMailServer.ini so
      // we check a few other properties as well.
      if (IniFileSettings::Instance()->GetDatabasePort() != 3307 ||
          IniFileSettings::Instance()->GetUsername().CompareNoCase(_T("root")) != 0 &&
          IniFileSettings::Instance()->GetUsername().CompareNoCase(_T("hmailserver")) != 0 &&
          IniFileSettings::Instance()->GetIsInternalDatabase())
      {
         // The user is not using the internal database.
         return;
      }

      // Remove dummy user created after installation.
      UpdatePassword_();

      // Run the scripts file
      String sScriptsFile = FileUtilities::Combine(FileUtilities::Combine(IniFileSettings::Instance()->GetDBScriptDirectory(), "Internal MySQL"), "HMS4.3-MySQL4.1.18.sql");
      RunScriptFile_(sScriptsFile);

      RunCommand_("FLUSH PRIVILEGES");
   }

   void
   MySQLConnection::UpdatePassword_()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Remoevs any user that lacks user name. Used to tighten security on the internal
   // database.
   //---------------------------------------------------------------------------()
   {
      // Remove the dummy user.
      RunCommand_("DELETE FROM mysql.user WHERE User = ''");
   }

   void
   MySQLConnection::RunScriptFile_(const String &sFile)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Runs a SQL script which contains commands separated with semicolons. This
   // function will always succeed, so should only be used for non-important
   // SQL epressions
   //---------------------------------------------------------------------------()
   {
#ifndef _DISABLE_MYSQL_AUTOUPGRADE
      String sContents = FileUtilities::ReadCompleteTextFile(sFile);

      std::vector<String> vecCommands = StringParser::SplitString(sContents, ";");

      auto iterCommand = vecCommands.begin();
      auto iterEnd = vecCommands.end();
      for (; iterCommand != iterEnd; iterCommand++)
      {
         String sSQL = (*iterCommand);

         sSQL.TrimLeft(_T("\r\n "));
         sSQL.TrimRight(_T("\r\n "));

         if (!sSQL.IsEmpty())
            RunCommand_(sSQL);
      }
#endif
   }

   void
   MySQLConnection::RunCommand_(const String &sCommand)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Runs a single SQL command without any error handling.
   //---------------------------------------------------------------------------()
   {
      String sError;

      TryExecute(SQLCommand(sCommand), sError, 0);
   }

   bool
   MySQLConnection::BeginTransaction(String &sErrorMessage)
   {
      if (supports_transactions_)
      {
         SetTransactionOpen_(false);
         transaction_ended_ = false;
         lock_wait_timed_out_ = false;

         const bool begun = TryExecute(SQLCommand("BEGIN"), sErrorMessage, 0) == DALSuccess;
         SetTransactionOpen_(begun);
         return begun;
      }

      return true;
   }

   bool
   MySQLConnection::CommitTransaction(String &sErrorMessage)
   {
      if (supports_transactions_)
      {
         if (IsTransactionFailed())
         {
            // The database has rolled the transaction back already, at the failure
            // that ended it (IsTransactionFailed), and anything run on the session
            // since was committed by itself, in autocommit. MySQL and MariaDB answer
            // a COMMIT with nothing open with success, which every caller - COM's
            // Database.CommitTransaction, SchemaUpgrader - read as everything saved.
            // As PGConnection::CommitTransaction does: rolled back, so that the
            // session holds nothing whatever was open on it, and said to be. A
            // session that replaced the transaction's has nothing open either.
            const bool lost = IsTransactionSessionLost();
            SetTransactionOpen_(false);
            transaction_ended_ = false;
            lock_wait_timed_out_ = false;

            String rollbackError;
            TryExecute(SQLCommand("ROLLBACK"), rollbackError, 0);

            sErrorMessage = lost ?
               "MySQL: the transaction was rolled back, not committed: the connection to the database was lost inside it, and the database rolled back what it held with the session." :
               "MySQL: the transaction was rolled back, not committed: a statement in it failed in a way that ends the transaction - a deadlock, for one - and the database rolled back what it held.";
            return false;
         }

         const bool committed = TryExecute(SQLCommand("COMMIT"), sErrorMessage, 0) == DALSuccess;
         SetTransactionOpen_(false);
         transaction_ended_ = false;
         lock_wait_timed_out_ = false;
         return committed;
      }


      return true;
   }

   bool
   MySQLConnection::RollbackTransaction(String &sErrorMessage)
   {
      if (supports_transactions_)
      {
         const bool rolledBack = TryExecute(SQLCommand("ROLLBACK"), sErrorMessage, 0) == DALSuccess;
         SetTransactionOpen_(false);
         transaction_ended_ = false;
         lock_wait_timed_out_ = false;
         return rolledBack;
      }
      else
      {
         sErrorMessage = "Rollback of MySQL statements failed. You may need to restore the latest database backup to ensure database integrity";
         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5104, "MySQLConnection::RollbackTransaction", sErrorMessage);

         return false;
      }
   }

   DALConnection::LastFailure
   MySQLConnection::ClassifyLastFailure(ExecutionResult result)
   {
      if (result == DALConnection::DALConnectionProblem || dbconn_ == 0)
         return LastFailureConnection;

      switch (MySQLInterface::Instance()->p_mysql_errno(dbconn_))
      {
      case 2006: // CR_SERVER_GONE_ERROR, MySQL server has gone away
      case 2013: // CR_SERVER_LOST, lost connection during the query
      case 2055: // CR_SERVER_LOST_EXTENDED
      case 1053: // ER_SERVER_SHUTDOWN
      case 1927: // ER_CONNECTION_KILLED (MariaDB)
      // A session the server ended answers the next statement over TLS with the TLS
      // layer's end of file, not 2013 (measured through libmariadb on MySQL 8.4 and
      // MariaDB 11.8, 13 October 2026, a session killed inside a transaction). Here
      // only for a statement on a connection already made: ConnectOnce_ answers a
      // TLS refusal at connect itself.
      case HM_MYSQL_CR_SSL_CONNECTION_ERROR:
         return LastFailureConnection;
      case 4031:
         // ER_CLIENT_INTERACTION_TIMEOUT on MySQL 8.0.24 and later, an idle session
         // the server closed; on MariaDB ER_REFERENCED_TRG_DOES_NOT_EXIST, a
         // statement's own refusal, which a unit of work run again would meet
         // again. The server's fork decides, as in CheckError.
         return server_is_mysql_ ? LastFailureConnection : LastFailureOther;
      case 1213: // ER_LOCK_DEADLOCK: the transaction was rolled back
      case 1205: // ER_LOCK_WAIT_TIMEOUT: the statement was rolled back
      case 1020: // ER_CHECKREAD: MariaDB's snapshot isolation (11.6 and later)
         return LastFailureRetryable;
      case 1062: // ER_DUP_ENTRY, the failure GetErrorType_ calls DALErrorInSQL
         return LastFailureDuplicate;
      default:
         return LastFailureOther;
      }
   }

   void
   MySQLConnection::LoadSupportsTransactions_(const String &database)
   {
      supports_transactions_ = false;

      if (database.GetLength() == 0)
         return;

      MySQLRecordset rec;
      if (!rec.Open(shared_from_this(), SQLCommand("SHOW TABLE STATUS in " + QuoteIdentifier(database))))
         return;

      int tableCount = 0;

      while (!rec.IsEOF())
      {
         String sEngine = rec.GetStringValue("Engine");
         if (sEngine.CompareNoCase(_T("InnoDB")) != 0)
         {
            return;
         }

         tableCount++;

         rec.MoveNext();
      }

      if (tableCount > 0)
      {
         // Only InnoDB tables in this database. Enable transactions.
         supports_transactions_ = true;
      }
   }

   void
   MySQLConnection::LoadRollbackOnTimeout_()
   {
      // A global variable the server reads at its start alone, on MySQL and MariaDB
      // alike (off by default), which every account may read: 0 or 1. Not read, or
      // read as anything else, it is taken as on (rollback_on_timeout_). TryOpen, so
      // that a server without it reports nothing at every connection.
      rollback_on_timeout_ = true;

      MySQLRecordset rec;
      String error;
      if (rec.TryOpen(shared_from_this(), SQLCommand("SELECT @@innodb_rollback_on_timeout AS rollbackontimeout"), error) == DALConnection::DALSuccess && !rec.IsEOF())
      {
         String value = rec.GetStringValue("rollbackontimeout");
         value.Trim();
         rollback_on_timeout_ = !(value == _T("0") || value.CompareNoCase(_T("OFF")) == 0);
      }
   }

   bool
   MySQLConnection::FailureEndsTransaction_(unsigned int errorCode, ExecutionResult result) const
   {
      // The session is gone, and the database has rolled back what it had not
      // committed: the client's lost connections CheckError names, and those
      // ClassifyLastFailure names beside them.
      if (result == DALConnection::DALConnectionProblem)
         return true;

      // The rest of the session lost, as ClassifyLastFailure names it: 2055, 1053,
      // 1927. And the failures at which InnoDB rolls the whole transaction back,
      // where at any other it takes back the failed statement alone, and the
      // session goes on in autocommit: 1213, measured on MySQL 8.4.11 and MariaDB
      // 11.8.9, and 1020 on MariaDB 11.8.9 (14 and 15 October 2026, with the
      // databases' own clients and through this code, DatabaseTransactionTester:
      // the whole transaction rolled back, the next statement committed on its
      // own, a COMMIT after it answered with success); 1206 by InnoDB's own code,
      // which rolls the whole transaction back at a lock table that is full as
      // at a deadlock (row_mysql_handle_errors), and not measured. Not 1205, a
      // lock wait that timed out: a row lock's ends the transaction where
      // innodb_rollback_on_timeout is on, a metadata lock's never does, and the
      // error is the same - so CheckError leaves it to IsTransactionFailed, which
      // asks the session.
      switch (errorCode)
      {
      case 2055: // CR_SERVER_LOST_EXTENDED
      case 1053: // ER_SERVER_SHUTDOWN
      case 1927: // ER_CONNECTION_KILLED (MariaDB)
      case 1213: // ER_LOCK_DEADLOCK
      case 1020: // ER_CHECKREAD: MariaDB's snapshot isolation
      case 1206: // ER_LOCK_TABLE_FULL
         return true;
      default:
         return false;
      }
   }

   bool
   MySQLConnection::IsTransactionFailed() const
   {
      if (IsTransactionSessionLost())
         return true;

      if (!IsTransactionOpen_())
         return false;

      // A lock wait that timed out where innodb_rollback_on_timeout is on: ended if
      // it was a row lock's, not if it was a metadata lock's, which the error does
      // not tell apart - the session does. Asked once, the first time this is
      // asked after it.
      if (lock_wait_timed_out_ && !transaction_ended_)
      {
         lock_wait_timed_out_ = false;
         transaction_ended_ = !SessionHoldsTransaction_();
      }

      return transaction_ended_;
   }

   bool
   MySQLConnection::SessionHoldsTransaction_() const
   {
      if (dbconn_ == 0)
         return false;

      MySQLInterface *client = MySQLInterface::Instance();

      // Each statement's answer read and let go of, as TryExecute does, so that
      // the connection is ready for the next.
      auto run = [&](const char *statement) -> bool
      {
         const bool succeeded = client->p_mysql_query(dbconn_, statement) == 0;
         hm_MYSQL_RES *result = client->p_mysql_store_result(dbconn_);
         if (result)
            client->p_mysql_free_result(result);
         return succeeded;
      };

      return run("SAVEPOINT hm_transaction_open_probe") && run("RELEASE SAVEPOINT hm_transaction_open_probe");
   }

   void
   MySQLConnection::SetConnectionCharacterSet_()
   {
      std::set<String> utf_character_sets;

      MySQLRecordset rec;
      if (!rec.Open(shared_from_this(), SQLCommand("SHOW CHARACTER SET LIKE 'UTF%'")))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5008, "MySQLConnection::LoadConnectionCharacterSet_", "Unable to find appropriate MySQL character set. Command SHOW CHARACTER SET LIKE 'UTF%' failed.");
         return;
      }


      while (!rec.IsEOF())
      {
         String character_set  = rec.GetStringValue("Charset");
         utf_character_sets.insert(character_set);
         rec.MoveNext();
      }

      String character_set_to_use;

      if (utf_character_sets.find("utf8mb4") != utf_character_sets.end())
         character_set_to_use = "utf8mb4";
      else if (utf_character_sets.find("utf8") != utf_character_sets.end())
         character_set_to_use = "utf8";
      else
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5008, "MySQLConnection::LoadConnectionCharacterSet_", "Unable to find appropriate MySQL character set.");
         return;
      }

      String error_message;
      AnsiString set_names_command = Formatter::Format("SET NAMES {0}", character_set_to_use);

      if (TryExecute(SQLCommand(set_names_command), error_message, 0, 0) != DALConnection::DALSuccess)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5008, "MySQLConnection::LoadConnectionCharacterSet_", set_names_command);
      }
   }

   bool
   MySQLConnection::TrySetTimeoutVariable_(const String &variableName, const String &value)
   {
      String command;
      command.Format(_T("SET SESSION %s = %s"), variableName.c_str(), value.c_str());

      String errorMessage;

      return TryExecute(SQLCommand(command), errorMessage, 0, 0) == DALConnection::DALSuccess;
   }

   void
   MySQLConnection::LoadTimeoutMechanism_()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Decides which session variable this server uses to bound statement execution
   // time, and in which unit. See the comment on MySQLConnection::SetTimeout. The
   // fork it reads for that is also kept for CheckError (server_is_mysql_).
   //---------------------------------------------------------------------------()
   {
      timeout_variable_ = "";
      timeout_variable_is_seconds_ = false;

      // Not MySQL until VERSION() says so: CheckError takes error 4031 for a lost
      // connection on MySQL alone, since on MariaDB the number is another error.
      server_is_mysql_ = false;

      // The fork is read from VERSION() rather than from
      // mysql_get_server_version, on purpose. MariaDB still announces itself in the
      // handshake as "5.5.5-<real version>-MariaDB" for the benefit of very old
      // clients, and a client library that parses that literally reports MariaDB
      // 10.6 as version 50505 - which would send this down the MySQL branch and pick
      // the wrong unit. VERSION() is answered by the server itself and carries no
      // such prefix.
      bool is_maria_db = false;

      MySQLRecordset rec;
      if (rec.Open(shared_from_this(), SQLCommand("SELECT VERSION() AS serverversion")) && !rec.IsEOF())
      {
         String version = rec.GetStringValue("serverversion");
         version.MakeLower();

         is_maria_db = version.Find(_T("mariadb")) >= 0;
         server_is_mysql_ = !is_maria_db;
      }
      else
      {
         // Without an answer there is no safe choice: the two forks disagree about
         // what "30" means for max_statement_time, so guessing is how every
         // statement ends up being aborted after 30 milliseconds. No mechanism is
         // the honest outcome.
         LOG_APPLICATION("The MySQL server version could not be read, so no statement timeout has been "
                         "set on this connection.");
         return;
      }

      if (is_maria_db)
      {
         if (TrySetTimeoutVariable_(_T("max_statement_time"), _T("0")))
         {
            timeout_variable_ = _T("max_statement_time");
            timeout_variable_is_seconds_ = true;
            return;
         }
      }
      else
      {
         if (TrySetTimeoutVariable_(_T("max_execution_time"), _T("0")))
         {
            timeout_variable_ = _T("max_execution_time");
            timeout_variable_is_seconds_ = false;
            return;
         }

         // MySQL 5.7.4 to 5.7.7 only. Milliseconds there too - this branch is never
         // reached on MariaDB, which is the fork where the same name means seconds.
         if (TrySetTimeoutVariable_(_T("max_statement_time"), _T("0")))
         {
            timeout_variable_ = _T("max_statement_time");
            timeout_variable_is_seconds_ = false;
            return;
         }
      }

      // Not an error: MySQL before 5.7.4 and MariaDB before 10.1.1 simply do not
      // have one, and hMailServer still supports them. Said once per connection so
      // that an administrator wondering why DatabaseStatementTimeout has no effect
      // can find out why.
      LOG_APPLICATION("This MySQL server has no statement-timeout variable (MariaDB 10.1.1 or MySQL 5.7.4 "
                      "and later have one), so DatabaseStatementTimeout does not apply to it.");
   }

   void
   MySQLConnection::SetTimeout(int seconds)
   {
      if (dbconn_ == 0 || !is_connected_ || timeout_variable_.IsEmpty())
         return;

      // 0 is "no limit" for both variables, so a caller asking for no timeout gets
      // the server's own behaviour rather than a very small one.
      __int64 value = 0;

      if (seconds > 0)
      {
         if (timeout_variable_is_seconds_)
         {
            value = seconds;
         }
         else
         {
            // Milliseconds, capped at what the variable can hold. MySQL's
            // max_execution_time is an unsigned 32-bit value; 1800 seconds - what
            // SQLScriptRunner asks for - is well inside it, and the cap is here for
            // a nonsense value in the ini rather than for anything this server does.
            const __int64 maximum_milliseconds = 4294967295LL;

            value = (__int64) seconds * 1000;

            if (value > maximum_milliseconds)
               value = maximum_milliseconds;
         }
      }

      String valueText;
      valueText.Format(_T("%I64d"), value);

      if (!TrySetTimeoutVariable_(timeout_variable_, valueText))
      {
         LOG_APPLICATION("The MySQL " + timeout_variable_ + " could not be set on this connection, so "
                         "statements on it are not time-limited.");
      }
   }

   std::shared_ptr<DALRecordset>
   MySQLConnection::CreateRecordset()
   {
      std::shared_ptr<MySQLRecordset> recordset = std::shared_ptr<MySQLRecordset>(new MySQLRecordset());
      return recordset;
   }

   void
   MySQLConnection::EscapeString(String &sInput)
   {
      sInput.Replace(_T("'"), _T("''"));
      sInput.Replace(_T("\\"), _T("\\\\"));
   }

   std::shared_ptr<IMacroExpander>
   MySQLConnection::CreateMacroExpander()
   {
      std::shared_ptr<MySQLMacroExpander> expander = std::shared_ptr<MySQLMacroExpander>(new MySQLMacroExpander());
      return expander;
   }

   String
   MySQLConnection::QuoteIdentifier(const String &name)
   {
      String quoted = name;
      quoted.Replace(_T("`"), _T("``"));
      return _T("`") + quoted + _T("`");
   }

   void
   MySQLConnectionTester::Test()
   {
      struct Quoted
      {
         const wchar_t *name;
         const wchar_t *expected;
      };

      const Quoted quoted[] =
      {
         { L"hmailserver", L"`hmailserver`" },
         { L"hm-mail", L"`hm-mail`" },
         { L"a`b", L"`a``b`" },
         { L"``", L"``````" },
         { L"x`; drop database y; `", L"`x``; drop database y; ```" },
         { L"", L"``" },
      };

      for (const Quoted &item : quoted)
      {
         if (MySQLConnection::QuoteIdentifier(item.name) != String(item.expected))
            throw std::logic_error("MySQLConnectionTester: a database name was not quoted as MySQL reads a quoted identifier: " +
               std::string(Unicode::ToUtf8(String(item.name)).c_str()) + " gave " + std::string(Unicode::ToUtf8(MySQLConnection::QuoteIdentifier(item.name)).c_str()));
      }

      // ASCII as it is, on every platform.
      AnsiString bytes;
      if (!DALConnection::ClientBytes(_T("it's a back\\slash; \"q\""), bytes) || bytes != AnsiString("it's a back\\slash; \"q\""))
         throw std::logic_error("MySQLConnectionTester: an ASCII value was not handed to the client as it is");

      // Beyond U+00FF, UTF-8 on every platform: p, U+00E4, U+65E5. Up to 6.3.5
      // nothing at all, here and on Windows.
      String beyond(L"p");
      beyond += (wchar_t) 0xE4;
      beyond += (wchar_t) 0x65E5;
      if (!DALConnection::ClientBytes(beyond, bytes) || bytes != AnsiString("p\xC3\xA4\xE6\x97\xA5"))
         throw std::logic_error(std::string("MySQLConnectionTester: a value with a character beyond U+00FF was not handed to the client as UTF-8, but as [") + bytes.c_str() + "]");

      // Within Latin-1: UTF-8 on POSIX, where it was nothing up to 6.3.5; Latin-1 on
      // Windows, as it always was there (DALConnection.h says why).
      String latin1(L"p");
      latin1 += (wchar_t) 0xE4;
#ifdef HM_PLATFORM_POSIX
      const AnsiString expected("p\xC3\xA4");
#else
      const AnsiString expected("p\xE4");
#endif
      if (!DALConnection::ClientBytes(latin1, bytes) || bytes != expected)
         throw std::logic_error(std::string("MySQLConnectionTester: a value within Latin-1 was handed to the client as [") + bytes.c_str() + "]");

      // Half of a surrogate pair, refused with nothing written.
      String lone(L"half");
      lone += (wchar_t) 0xD800;
      if (DALConnection::ClientBytes(lone, bytes) || !bytes.IsEmpty())
         throw std::logic_error("MySQLConnectionTester: half of a surrogate pair was not refused");
   }
}