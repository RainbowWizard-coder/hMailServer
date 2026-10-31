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

#include <string>
#include <vector>

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

         // verify-full as the client loaded checks it (VerifyFullCheck): refused
         // here, before anything is attempted, where it would check neither the
         // certificate's issuer nor its name, and
         // otherwise said as it is in the connection's description below.
         String verifyFullHow;
         if (effectiveMode == DatabaseTls::ModeVerifyFull)
         {
            String settingsWords;
            String refusal;
            if (!VerifyFullCheck(MySQLInterface::Instance()->IsMariaDbClient(), sServer, tlsCaFile, settingsWords, verifyFullHow, refusal))
            {
               sErrorMessage = refusal;
               ErrorManager::Instance()->ReportError(ErrorManager::Critical, 7535, "MySQLConnection::Connect", refusal);
               return FatalError;
            }
         }

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
            encrypted && effectiveMode == DatabaseTls::ModeVerifyFull, tlsMode, verifyFullHow));

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
         // In the code page the client's LoadLibraryA reads it in (DALConnection::
         // FileNameBytes).
         AnsiString sPluginDir;
         if (FileNameBytes(client->GetLibraryDirectory() + _T("\\plugin"), sPluginDir) && !sPluginDir.IsEmpty())
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

      // The file's name as the client opens it (CaFileBytes). A name that cannot be
      // written so is refused here, with nothing attempted: a CA option left unset
      // is a certificate checked against the system's trust store instead, which is
      // what Unicode::ToANSI's empty answer did up to 6.3.7 for a name with a
      // character outside ASCII on Linux, or above U+00FF on Windows.
      AnsiString caFileName;
      if (!caFile.IsEmpty() && !CaFileBytes(caFile, client->IsMariaDbClient(), caFileName))
      {
         error = Formatter::Format("TlsCaFile under [Database] in hMailServer.ini names {0}, which cannot be handed to the MySQL client as it is named, "
            "so TlsMode={1} cannot check the server's certificate against it. Move the file to a path without the character and name that. "
            "The connection was not attempted.", caFile, DatabaseTls::ModeName(mode));
         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 7534, "MySQLConnection::ApplyTlsOptions_", error);
         return false;
      }

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
         //                         it, which is MariaDB's design, not a gap. On a
         //                         connection the client takes as local (127.0.0.1,
         //                         ::1, localhost, a socket, or a Server= list
         //                         holding one: ClientTakesAsLocal)
         //                         the name is not checked, and the chain only
         //                         against a CA file, so that Connect refuses
         //                         verify-full there without one (VerifyFullCheck).
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
   MySQLConnection::CaFileBytes(const String &caFile, bool mariaDbClient, AnsiString &bytes)
   {
#ifndef HM_PLATFORM_POSIX
      if (!mariaDbClient)
      {
         // Oracle's client hands the name to OpenSSL (vio/viosslfactories.cc,
         // SSL_CTX_load_verify_locations), whose fopen on Windows reads it as UTF-8
         // first and opens the file by its wide name (crypto/o_fopen.c), so UTF-8
         // names the file whatever the code page. Half of a surrogate pair and a
         // U+0000, which no name holds, are refused.
         bytes.Empty();
         if (!Unicode::ToUtf8Exact(caFile, bytes) || std::string(bytes.c_str(), (size_t) bytes.GetLength()).find('\0') != std::string::npos)
         {
            bytes.Empty();
            return false;
         }

         return true;
      }
#else
      (void) mariaDbClient;
#endif

      // MariaDB Connector/C opens it with CreateFileA on Windows (its Schannel
      // build, the one bundled) and with OpenSSL's or GnuTLS's fopen on POSIX, as
      // does Oracle's client there: DALConnection::FileNameBytes.
      return DALConnection::FileNameBytes(caFile, bytes);
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

   namespace
   {
      // The names MariaDB Connector/C connects to for Server=, in the bytes it is
      // handed (DALConnection::ClientBytes), as mthd_my_real_connect reads them
      // (3.4, libmariadb/mariadb_lib.c): the whole value when it holds no comma,
      // and otherwise a list of servers tried in turn until one answers, each
      // element cut by ma_get_host_list - strtok_r on ",", which skips empty
      // elements; in an element with a "]", the name runs from its second byte to
      // that "]" ("[::1]"), or, where the "]" is its first byte, from there to the
      // element's end; and a ":" after the "]", or anywhere in an element with
      // none, begins a port, which ends a name that runs to the element's end.
      // The element that answers is the name the client keeps (mysql->host) and
      // that my_auth.c's is_local_connection compares.
      std::vector<std::string>
      MariaDbServerNames_(const std::string &server)
      {
         std::vector<std::string> names;
         if (server.find(',') == std::string::npos)
         {
            names.push_back(server);
            return names;
         }

         size_t start = 0;
         while (start <= server.size())
         {
            size_t end = server.find(',', start);
            if (end == std::string::npos)
               end = server.size();

            const std::string element = server.substr(start, end - start);
            start = end + 1;

            if (element.empty())
               continue;

            std::string name;
            const size_t bracket = element.find(']');
            if (bracket != std::string::npos && bracket > 0)
            {
               name = element.substr(1, bracket - 1);
            }
            else
            {
               name = bracket == 0 ? element.substr(1) : element;
               const size_t colon = name.find(':');
               if (colon != std::string::npos)
                  name.erase(colon);
            }

            names.push_back(name);
         }

         return names;
      }

      // Whether MariaDB Connector/C would take a connection to name as one to
      // this machine: my_auth.c's list, compared as it compares it (strcmp:
      // exactly, case and all), and the transports it takes as local whatever
      // the name - a Unix socket, which the client uses on POSIX for localhost
      // (mthd_my_real_connect: LOCAL_HOST with no protocol named), and on
      // Windows a named pipe, which it uses for ".".
      bool
      MariaDbTakesAsLocal_(const std::string &name)
      {
#ifdef HM_PLATFORM_POSIX
         static const char *const localNames[] = { "localhost", "127.0.0.1", "::1" };
#else
         static const char *const localNames[] = { "localhost", "127.0.0.1", "::1", "." };
#endif
         for (const char *local : localNames)
         {
            if (name == local)
               return true;
         }

         return false;
      }

      // Whether some, and whether all, of the servers Server= names are ones
      // MariaDB Connector/C takes as this machine. An empty name is no name to
      // the client (mthd_my_real_connect: "if (!host || !host[0])"), which then
      // connects as to localhost - by the Unix socket on POSIX, over TCP to
      // localhost on Windows - so it is local too; an empty element of a list is
      // a name, "", which the client looks up as one and checks as one. A value
      // the client cannot be handed is taken as local, the stricter reading;
      // Connect refuses it before this is asked. hMailServer names no protocol
      // and reads no option file into the client, so nothing else makes a
      // connection local.
      void
      MariaDbLocalServers_(const String &server, bool &some, bool &all)
      {
         some = true;
         all = true;

         AnsiString bytes;
         if (!DALConnection::ClientBytes(server, bytes))
            return;

         const std::string value(bytes.c_str());
         if (value.empty())
            return;

         some = false;
         for (const std::string &name : MariaDbServerNames_(value))
         {
            if (MariaDbTakesAsLocal_(name))
               some = true;
            else
               all = false;
         }
      }
   }

   bool
   MySQLConnection::ClientTakesAsLocal(const String &server)
   {
      // Which server of a list answers is known only once one has: the
      // connection is taken as local when any of them is.
      bool some = false;
      bool all = false;
      MariaDbLocalServers_(server, some, all);
      return some;
   }

   bool
   MySQLConnection::VerifyFullCheck(bool mariaDbClient, const String &server, const String &caFile,
                                    String &settingsWords, String &checkedHow, String &refusal)
   {
      settingsWords.Empty();
      checkedHow.Empty();
      refusal.Empty();

      if (!mariaDbClient)
      {
         // Oracle's client checks the chain against the CA file and the name
         // against Server= on every connection, and refuses VERIFY_IDENTITY with no
         // CA file before it connects (sql-common/client.cc: "CA certificate is
         // required if ssl-mode is VERIFY_CA or VERIFY_IDENTITY").
         settingsWords = caFile.IsEmpty()
            ? String(_T(", the certificate to be checked against a CA file, which TlsCaFile does not name: Oracle's MySQL client refuses VERIFY_IDENTITY without one"))
            : _T(", the certificate checked against ") + caFile;
         return true;
      }

      // MariaDB Connector/C, from 3.4.1 (plugins/auth/my_auth.c): on a connection it
      // takes as local the certificate's name is never checked, and its issuer only
      // when a CA file is named; elsewhere both, against the CA file or the system's
      // trust store - and with no CA file, a certificate the store does not vouch
      // for is still taken from a MariaDB server that proves it holds the account's
      // password (MariaDB's zero-configuration TLS, for mysql_native_password,
      // ed25519 and parsec accounts), never with a CA file named. Up to 6.3.7 this
      // server said "checked" for all of them, and on a local connection with no CA
      // file connected with neither the issuer nor the name checked, the
      // certificate's dates alone. A Server= that lists
      // servers (MariaDbServerNames_) is local when any of them is, since any may
      // be the one that answers.
      bool someLocal = false;
      bool allLocal = false;
      MariaDbLocalServers_(server, someLocal, allLocal);

      if (!someLocal)
      {
         if (!caFile.IsEmpty())
         {
            settingsWords = _T(", the certificate checked against ") + caFile;
            return true;
         }

         settingsWords = _T(", the certificate checked against the system's trust store (TlsCaFile is not set) - or, for a certificate the store does not ")
            _T("vouch for, by a MariaDB server's proof that it holds the account's password, which MariaDB Connector/C takes in its place");
         checkedHow = _T(" against the system's trust store, or by a MariaDB server's proof of the account's password");
         return true;
      }

      if (!caFile.IsEmpty() && allLocal)
      {
         settingsWords = _T(", the certificate's issuer checked against ") + caFile +
            _T(", and not its name: MariaDB Connector/C checks no name on a connection to this machine (Server=") + server + _T(")");
         checkedHow = _T(" for its issuer alone, not for its name, on a connection to this machine");
         return true;
      }

      if (!caFile.IsEmpty())
      {
         settingsWords = _T(", the certificate's issuer checked against ") + caFile +
            _T(", and its name only where the server that answers is not this machine: Server=") + server +
            _T(" lists servers MariaDB Connector/C tries in turn, among them one it takes as this machine, where it checks no name");
         checkedHow = _T(" for its issuer, and for its name only if the server that answered is not this machine, which Server= also lists");
         return true;
      }

      refusal = Formatter::Format("TlsMode=verify-full under [Database] in hMailServer.ini asks for the database server's certificate to be checked, and "
         "Server={0} is a connection MariaDB Connector/C takes as one to this machine, or lists one among the servers it tries in turn - 127.0.0.1, ::1 "
         "or localhost, alone or as an element of a list such as [::1]:3306, no name at all, a socket or a named pipe - "
         "on which it checks neither the certificate's issuer nor its name unless a CA file is named. Name the CA that issued the database server's certificate "
         "in TlsCaFile, or set TlsMode=require to encrypt without checking it. The connection was not attempted.", server);
      return false;
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

      // TlsCaFile's name as the client opens the file (CaFileBytes), for both
      // clients. Up to 6.3.7 Unicode::ToANSI's bytes, none at all for these names
      // on Linux - the CA option was then not set, and the server's certificate
      // was checked against the system's trust store.
      struct CaName
      {
         const wchar_t *name;
         const char *posix;
      };

      const wchar_t agrave[] = { L'/', L'c', 0x00E0, L'/', L'c', L'a', L'.', L'p', L'e', L'm', 0 };
      const wchar_t idot[] = { L'/', 0x0130, L'/', L'c', L'a', L'.', L'p', L'e', L'm', 0 };
      const wchar_t euro[] = { L'/', 0x20AC, L'/', L'c', L'a', L'.', L'p', L'e', L'm', 0 };
      const CaName caNames[] =
      {
         { L"/etc/hmailserver/db-ca.pem", "/etc/hmailserver/db-ca.pem" },
         { agrave, "/c\xC3\xA0/ca.pem" },
         { idot, "/\xC4\xB0/ca.pem" },
         { euro, "/\xE2\x82\xAC/ca.pem" },
      };

      for (const CaName &caName : caNames)
      {
         for (const bool mariaDb : { true, false })
         {
            AnsiString caBytes;
            const bool written = MySQLConnection::CaFileBytes(String(caName.name), mariaDb, caBytes);
#ifdef HM_PLATFORM_POSIX
            // The file system's bytes, UTF-8, for either client.
            if (!written || caBytes != AnsiString(caName.posix))
               throw std::logic_error(std::string("MySQLConnectionTester: a CA file's name was handed to the ") + (mariaDb ? "MariaDB" : "Oracle") +
                  " client as [" + caBytes.c_str() + "], not as its UTF-8 [" + caName.posix + "]");
#else
            // The Oracle client's OpenSSL reads UTF-8 first; MariaDB's Schannel
            // build opens the file with CreateFileA, in the ANSI code page - and a
            // name that code page cannot carry is refused, never handed over empty.
            AnsiString utf8;
            Unicode::ToUtf8Exact(String(caName.name), utf8);
            AnsiString acp;
            const bool acpWritten = DALConnection::FileNameBytes(String(caName.name), acp);
            if (!mariaDb && (!written || caBytes != utf8))
               throw std::logic_error("MySQLConnectionTester: a CA file's name was not handed to the Oracle client in UTF-8");
            if (mariaDb && (written != acpWritten || caBytes != acp))
               throw std::logic_error("MySQLConnectionTester: a CA file's name was not handed to the MariaDB client in the ANSI code page");
#endif
            if (!written && !caBytes.IsEmpty())
               throw std::logic_error("MySQLConnectionTester: a CA file's name that was refused left bytes written");
            if (written && caBytes.IsEmpty())
               throw std::logic_error("MySQLConnectionTester: a CA file's name was taken and handed over as nothing");
         }
      }

      // Half of a surrogate pair, and a U+0000 in the name: refused for both.
      String loneName(L"/ca");
      loneName += (wchar_t) 0xD800;
      String nulName(L"/ca");
      nulName += (wchar_t) 0;
      nulName += L".pem";
      for (const bool mariaDb : { true, false })
      {
         AnsiString caBytes;
         if (MySQLConnection::CaFileBytes(loneName, mariaDb, caBytes) || !caBytes.IsEmpty())
            throw std::logic_error("MySQLConnectionTester: a CA file's name with half of a surrogate pair was not refused");
      }
      if (DALConnection::FileNameBytes(loneName, bytes) || !bytes.IsEmpty())
         throw std::logic_error("MySQLConnectionTester: FileNameBytes took half of a surrogate pair");
      if (nulName.GetLength() == 8 && (DALConnection::FileNameBytes(nulName, bytes) || !bytes.IsEmpty()))
         throw std::logic_error("MySQLConnectionTester: FileNameBytes took a name with U+0000 in it, which a narrow call would cut short");

#ifndef HM_PLATFORM_POSIX
      // A name the ANSI code page cannot carry - CJK on code page 1252 - is
      // refused for the bundled client rather than given a best-fit letter that
      // names another file.
      if (GetACP() == 1252)
      {
         const wchar_t cjk[] = { L'C', L':', L'\\', 0x65E5, L'\\', L'c', L'a', L'.', L'p', L'e', L'm', 0 };
         AnsiString caBytes;
         if (MySQLConnection::CaFileBytes(String(cjk), true, caBytes) || !caBytes.IsEmpty())
            throw std::logic_error("MySQLConnectionTester: a CA file's name code page 1252 cannot carry was handed to the MariaDB client");
      }
#endif

      // verify-full as each client checks it. MariaDB Connector/C takes 127.0.0.1,
      // ::1 and localhost - exactly so - as this machine, and checks there no name
      // and, with no CA file, nothing: refused (HM7535), where up to 6.3.7 the
      // connection was made and said to be checked. Elsewhere it checks both, and
      // with no CA file takes a MariaDB server's proof of the password for a
      // certificate the system's store does not vouch for, which is said.
      const String caPath(_T("/etc/hmailserver/db-ca.pem"));
      String words;
      String how;
      String refusal;
      const auto failure = [](const std::string &what, const String &said)
      {
         return std::logic_error("MySQLConnectionTester: " + what + ": [" + std::string(Unicode::ToUtf8(said).c_str()) + "]");
      };

      // With a comma, Server= is a list of servers the client tries in turn
      // (mthd_my_real_connect, ma_get_host_list): empty elements skipped, an
      // element's name inside "[...]", and a ":" beginning its port. Each
      // element of these is one the client takes as local.
#ifdef HM_PLATFORM_POSIX
      const wchar_t *const localServers[] = { L"127.0.0.1", L"::1", L"localhost", L"",
         L"127.0.0.1,", L"127.0.0.1,127.0.0.1", L"127.0.0.1:3306,", L"[::1]:3306,", L",,[::1],", L"localhost,127.0.0.1", L"]127.0.0.1:3306,", L"[::1]x:1,localhost:0" };
#else
      const wchar_t *const localServers[] = { L"127.0.0.1", L"::1", L"localhost", L".", L"",
         L"127.0.0.1,", L"127.0.0.1,127.0.0.1", L"127.0.0.1:3306,", L"[::1]:3306,", L",,[::1],", L"localhost,127.0.0.1", L"]127.0.0.1:3306,", L"[::1]x:1,localhost:0",
         L".,", L".,localhost" };
#endif
      for (const wchar_t *server : localServers)
      {
         if (!MySQLConnection::ClientTakesAsLocal(server))
            throw failure("a server the MariaDB client takes as local was not taken so", String(server));

         if (MySQLConnection::VerifyFullCheck(true, server, String(), words, how, refusal))
            throw failure("verify-full with no TlsCaFile on a local connection was not refused, though the MariaDB client checks neither issuer nor name there", words);
         if (refusal.Find(String(_T("Server=")) + server) < 0 || refusal.Find(_T("TlsCaFile")) < 0 || refusal.Find(_T("not attempted")) < 0 || !words.IsEmpty() || !how.IsEmpty())
            throw failure("the refusal of verify-full on a local connection does not name the server, TlsCaFile and that nothing was attempted", refusal);

         if (!MySQLConnection::VerifyFullCheck(true, server, caPath, words, how, refusal) || !refusal.IsEmpty())
            throw failure("verify-full with a TlsCaFile on a local connection was refused", refusal);
         if (words.Find(_T("issuer checked against ") + caPath) < 0 || words.Find(_T("not its name")) < 0)
            throw failure("the settings line of verify-full on a local connection does not say the name goes unchecked", words);
         if (how != String(_T(" for its issuer alone, not for its name, on a connection to this machine")))
            throw failure("the description of verify-full on a local connection does not say the name goes unchecked", how);

         // Oracle's client checks the name wherever it connects, and refuses
         // VERIFY_IDENTITY without a CA file itself.
         if (!MySQLConnection::VerifyFullCheck(false, server, caPath, words, how, refusal) || words != _T(", the certificate checked against ") + caPath || !how.IsEmpty())
            throw failure("verify-full through Oracle's client with a TlsCaFile was not said as checked against it", words);
         if (!MySQLConnection::VerifyFullCheck(false, server, String(), words, how, refusal) || words.Find(_T("refuses VERIFY_IDENTITY")) < 0)
            throw failure("verify-full through Oracle's client with no TlsCaFile was said as checked", words);
      }

      // A list with a server the client takes as local and one it does not:
      // local, since which answers is not known before one has - refused with no
      // CA file, and with one said as the issuer checked, and the name only where
      // the server that answered is not this machine. Up to this change the
      // whole value was compared, and these were taken as servers over the
      // network: with no CA file the connection was made and said to be checked
      // against the system's trust store while neither the issuer nor the name
      // was checked.
      const wchar_t umlautList[] = { 0x00E4, L'.', L'e', L'x', L'a', L'm', L'p', L'l', L'e', L',', L'1', L'2', L'7', L'.', L'0', L'.', L'0', L'.', L'1', 0 };
#ifdef HM_PLATFORM_POSIX
      const wchar_t *const mixedServers[] = { L"x,127.0.0.1", L"nonexistent.invalid,127.0.0.1", L"x,[::1]", L"x,localhost", L"db.example.com,[::1]:3306",
         L"127.0.0.1,::1", L"127.0.0.1,.", umlautList };
#else
      const wchar_t *const mixedServers[] = { L"x,127.0.0.1", L"nonexistent.invalid,127.0.0.1", L"x,[::1]", L"x,localhost", L"db.example.com,[::1]:3306",
         L"127.0.0.1,::1", L"x,.", umlautList };
#endif
      for (const wchar_t *server : mixedServers)
      {
         if (!MySQLConnection::ClientTakesAsLocal(server))
            throw failure("a list of servers with one the MariaDB client takes as local was not taken so", String(server));

         if (MySQLConnection::VerifyFullCheck(true, server, String(), words, how, refusal))
            throw failure("verify-full with no TlsCaFile on a list of servers with one on this machine was not refused", words);
         if (refusal.Find(String(_T("Server=")) + server) < 0 || refusal.Find(_T("lists one")) < 0 || refusal.Find(_T("not attempted")) < 0 || !words.IsEmpty() || !how.IsEmpty())
            throw failure("the refusal of verify-full on a list of servers with one on this machine does not name the server and the list", refusal);

         if (!MySQLConnection::VerifyFullCheck(true, server, caPath, words, how, refusal) || !refusal.IsEmpty())
            throw failure("verify-full with a TlsCaFile on a list of servers with one on this machine was refused", refusal);
         if (words.Find(_T("issuer checked against ") + caPath) < 0 || words.Find(_T("its name only where the server that answers is not this machine")) < 0)
            throw failure("the settings line of verify-full on a list of servers with one on this machine does not say its name may go unchecked", words);
         if (how != String(_T(" for its issuer, and for its name only if the server that answered is not this machine, which Server= also lists")))
            throw failure("the description of verify-full on a list of servers with one on this machine does not say its name may go unchecked", how);
      }

      // Neither the whole value nor a list's element is one the client takes as
      // local: without a comma no port and no "[...]" is read ("127.0.0.1:3306" is
      // a name the client looks up as it is); "x,::1" lists "x" and "" (the
      // element ends at its first ":"), which the client looks up and checks as
      // names; and case, spaces and "." on POSIX count.
#ifdef HM_PLATFORM_POSIX
      const wchar_t *const remoteServers[] = { L"db.example.com", L"192.0.2.20", L"127.0.0.2", L"LOCALHOST", L"::2", L" ", L" 127.0.0.1",
         L"a.example,b.example", L"127.0.0.2,", L"127.0.0.1:3306", L"[::1]", L"x,::1", L"x, 127.0.0.1", L"x,LOCALHOST", L"x,[::2]:3306", L"x,.", L"x,:3306", L"x,a]127.0.0.1" };
#else
      const wchar_t *const remoteServers[] = { L"db.example.com", L"192.0.2.20", L"127.0.0.2", L"LOCALHOST", L"::2", L" ", L" 127.0.0.1",
         L"a.example,b.example", L"127.0.0.2,", L"127.0.0.1:3306", L"[::1]", L"x,::1", L"x, 127.0.0.1", L"x,LOCALHOST", L"x,[::2]:3306", L"x,:3306", L"x,a]127.0.0.1" };
#endif
      for (const wchar_t *server : remoteServers)
      {
         if (MySQLConnection::ClientTakesAsLocal(server))
            throw failure("a server the MariaDB client checks in full was taken as local", String(server));

         if (!MySQLConnection::VerifyFullCheck(true, server, caPath, words, how, refusal) || words != _T(", the certificate checked against ") + caPath || !how.IsEmpty())
            throw failure("verify-full with a TlsCaFile over the network was not said as checked against it", words);
         if (!MySQLConnection::VerifyFullCheck(true, server, String(), words, how, refusal))
            throw failure("verify-full with no TlsCaFile over the network was refused", refusal);
         if (words.Find(_T("system's trust store")) < 0 || words.Find(_T("password")) < 0 ||
             how != String(_T(" against the system's trust store, or by a MariaDB server's proof of the account's password")))
            throw failure("verify-full with no TlsCaFile over the network does not say a MariaDB server's password proof is taken", words + _T(" | ") + how);
      }

      const String local = DatabaseTls::Describe(DatabaseSettings::TypeMYSQLServer, true, _T("TLS_AES_256_GCM_SHA384"), true, DatabaseTls::ModeVerifyFull,
         _T(" for its issuer alone, not for its name, on a connection to this machine"));
      if (local != String(_T("MySQL/MariaDB: encrypted with TLS (TLS_AES_256_GCM_SHA384), the server's certificate checked for its issuer alone, not for its name, on a connection to this machine (TlsMode verify-full)")))
         throw failure("the connection's line for verify-full on a local connection", local);
   }
}