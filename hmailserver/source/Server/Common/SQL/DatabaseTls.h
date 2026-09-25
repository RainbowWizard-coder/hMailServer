// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DatabaseSettings.h"

namespace HM
{
   // TLS between this server and its database, the same four words for every
   // backend: [Database] TlsMode and TlsCaFile in hMailServer.ini
   // (docs/DatabaseTls.md). The file and not the settings store, because they
   // say how the store is reached - they are read before it is open, as Server,
   // Username and Password are.
   //
   //   disable      no TLS. SQL Server's providers still encrypt the login.
   //   prefer       TLS when the server offers it, and the connection without it
   //                when the server offers none or the TLS cannot be agreed; the
   //                certificate is not checked. The default (empty) is this on
   //                every backend - with one difference each for PostgreSQL and
   //                SQL Server, below.
   //   require      TLS or no connection, the certificate not checked. Through
   //                MariaDB Connector/C a server without TLS is refused after the
   //                login exchange rather than before it (MySQLConnection::Connect).
   //   verify-full  TLS, the certificate's chain checked against TlsCaFile (or the
   //                system's trust store when that is empty) and its name against
   //                the server's name - or its address, when the server is named by
   //                one.
   //
   // What each backend does with them:
   //
   //   MySQL and MariaDB   the client library's own options, set explicitly so that
   //                       no build's compiled-in default decides - MariaDB
   //                       Connector/C (MYSQL_OPT_SSL_ENFORCE, _VERIFY_SERVER_CERT,
   //                       _SSL_CA) or the Oracle client (MYSQL_OPT_SSL_MODE,
   //                       _SSL_CA), whose option numbers differ. Before this, both
   //                       the Windows client and Ubuntu's connected in plaintext
   //                       when asked nothing, which was measured on 25 September
   //                       2026 and is why the default is now prefer.
   //   PostgreSQL          libpq's sslmode and sslrootcert. PostgreSQLSslMode and
   //                       PostgreSQLSslRootCert, which came first, win when set;
   //                       with neither key set nothing is added and libpq's own
   //                       default (prefer, or PGSSLMODE) stands, as before.
   //   SQL Server          the OLE DB provider's "Use Encryption for Data" and "Trust
   //                       Server Certificate". The certificate is checked against
   //                       the Windows certificate store; TlsCaFile is not read.
   //                       ConnectionStringOptions that already names encryption
   //                       wins when TlsMode is not set, and is refused beside it.
   //   SQL Server Compact  a file on this machine: nothing to encrypt.
   class DatabaseTls
   {
   public:

      enum Mode
      {
         ModeDefault = 0,      // TlsMode not set
         ModeDisable = 1,
         ModePrefer = 2,
         ModeRequire = 3,
         ModeVerifyFull = 4
      };

      // How the OLE DB provider named for SQL Server takes the two keywords.
      enum ProviderKind
      {
         ProviderModern = 0,   // MSOLEDBSQL (18), SQLNCLI, SQLNCLI10, SQLNCLI11
         ProviderModern19 = 1, // MSOLEDBSQL19, which encrypts and verifies by default
         ProviderLegacy = 2,   // SQLOLEDB, which cannot encrypt without verifying
         ProviderOther = 3
      };

      // A TlsMode value, trimmed and without regard to case. Empty is ModeDefault.
      // False for anything else, verify-ca included (see UnknownModeMessage).
      static bool ParseMode(const String &text, Mode &mode);

      // disable, prefer, require, verify-full; "prefer, the default" for
      // ModeDefault.
      static String ModeName(Mode mode);

      // The mode a backend runs in when TlsMode is not set: prefer.
      static Mode Effective(Mode mode) { return mode == ModeDefault ? ModePrefer : mode; }

      // The mode and CA file from hMailServer.ini, checked for the backend about to
      // connect. False with the reason in error - and reported, HM7530 or HM7531 -
      // for a mode this server does not know and for a TlsCaFile that verify-full
      // would read and that is not there; the connection is not attempted. The CA
      // file is returned only for verify-full, the one mode that reads it. For SQL
      // Server a TlsCaFile is not read at all, which is said once (HM7532).
      static bool FromConfiguration(DatabaseSettings::SQLDBType type, Mode &mode, String &caFile, String &error);

      // The sentence for a TlsMode this server does not know.
      static String UnknownModeMessage(const String &value);

      // Whether ConnectionStringOptions already names SQL Server's encryption:
      // Encrypt, Use Encryption for Data, TrustServerCertificate or Trust Server
      // Certificate, in any case and spacing.
      static bool OptionsNameEncryption(const String &options);

      static ProviderKind ClassifyProvider(const String &provider);

      // The keywords appended to the SQL Server connection string for the mode and
      // provider, each ending in ';', or none. False with the reason in error when
      // TlsMode is set and ConnectionStringOptions names encryption too: two
      // settings saying the same thing differently is refused (HM7533) rather than
      // one of them silently losing.
      static bool SqlServerKeywords(Mode mode, const String &provider, const String &options, String &keywords, String &error);

      // Whether a SQL Server connection failure reads as a failure of TLS - the
      // provider's handshake, its certificate check, or encryption not supported -
      // rather than of the address, the login or the database.
      static bool LooksLikeTlsFailure(const String &error);

      // One line about a connection that has been made: whether it is encrypted,
      // what with, whether the certificate was checked, and the mode.
      static String Describe(DatabaseSettings::SQLDBType type, bool encrypted, const String &detail, bool verified, Mode mode);

      // The sentence added to a connection failure that looks like TLS, saying which
      // setting to change.
      static String Guidance(DatabaseSettings::SQLDBType type, Mode mode);

      // What the last connection this process made to its database was: the Control
      // Panel's diagnostics show it, and the application log says it whenever it
      // changes (so once for a pool of connections that are all alike).
      static void NoteConnection(const String &description);
      static String GetLastConnection();

      // The last connection, said in the application log. Application::InitInstance
      // calls it once the log's settings have been read, which is after the pool's
      // connections were made.
      static void AnnounceConnection();

      // hmailserver --check-database (both platforms): connect once to the database
      // [Database] names, with its TLS settings, say how the connection is protected
      // or why there is none, and disconnect. The configuration file is the one the
      // process was pointed at. 0 connected, 1 not connected, 2 not configured.
      static int RunCheckCommand();

   private:

      static String BackendName_(DatabaseSettings::SQLDBType type);

      static void Print_(bool error, const String &line);

      static boost::mutex &StateMutex_();
      static String &LastConnection_();
   };

   class DatabaseTlsTester
   {
   public:
      void Test();
   };
}
