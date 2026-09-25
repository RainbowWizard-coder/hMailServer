// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include <atomic>

#include "DatabaseTls.h"
#include "DALConnection.h"
#include "DALConnectionFactory.h"
#include "../Util/Assert.h"
#include "../Util/Unicode.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   bool
   DatabaseTls::ParseMode(const String &text, Mode &mode)
   {
      String value = text;
      value.Trim();

      if (value.IsEmpty())
      {
         mode = ModeDefault;
         return true;
      }

      if (value.CompareNoCase(_T("disable")) == 0)
         mode = ModeDisable;
      else if (value.CompareNoCase(_T("prefer")) == 0)
         mode = ModePrefer;
      else if (value.CompareNoCase(_T("require")) == 0)
         mode = ModeRequire;
      else if (value.CompareNoCase(_T("verify-full")) == 0)
         mode = ModeVerifyFull;
      else
         return false;

      return true;
   }

   String
   DatabaseTls::ModeName(Mode mode)
   {
      switch (mode)
      {
      case ModeDisable:
         return _T("disable");
      case ModePrefer:
         return _T("prefer");
      case ModeRequire:
         return _T("require");
      case ModeVerifyFull:
         return _T("verify-full");
      case ModeDefault:
      default:
         return _T("prefer, the default");
      }
   }

   String
   DatabaseTls::UnknownModeMessage(const String &value)
   {
      // verify-ca is named because it is the one an administrator who knows libpq
      // will reach for, and the one that cannot be honoured on two of the three
      // backends: MariaDB Connector/C checks the certificate's name whenever it
      // checks its issuer (MYSQL_OPT_SSL_VERIFY_SERVER_CERT is both), and SQL
      // Server's providers do the same. Offering it would mean offering a mode that
      // is silently verify-full on those two.
      return Formatter::Format("TlsMode under [Database] in hMailServer.ini is '{0}'; it must be disable, prefer, require or verify-full, "
         "or left empty for prefer. (verify-ca is not offered: MySQL's, MariaDB's and SQL Server's clients check the certificate's name whenever "
         "they check who issued it, so use verify-full - or PostgreSQLSslMode=verify-ca for PostgreSQL.) The connection was not attempted.", value);
   }

   bool
   DatabaseTls::FromConfiguration(DatabaseSettings::SQLDBType type, Mode &mode, String &caFile, String &error)
   {
      IniFileSettings *ini = IniFileSettings::Instance();

      mode = ModeDefault;
      caFile.Empty();

      const String text = ini->GetDatabaseTlsMode();
      if (!ParseMode(text, mode))
      {
         error = UnknownModeMessage(text);
         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 7530, "DatabaseTls::FromConfiguration", error);
         return false;
      }

      String configuredCaFile = ini->GetDatabaseTlsCaFile();
      configuredCaFile.Trim();

      if (configuredCaFile.IsEmpty())
         return true;

      if (type == DatabaseSettings::TypeMSSQLServer)
      {
         // The OLE DB providers check a server certificate against the Windows
         // certificate store and take no file. Said once a process rather than at
         // every connection of the pool; nothing is refused, since the store may
         // well hold the CA already.
         static std::atomic<bool> said(false);
         bool expected = false;
         if (said.compare_exchange_strong(expected, true))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7532, "DatabaseTls::FromConfiguration",
               "TlsCaFile under [Database] in hMailServer.ini is set, and SQL Server's provider does not read it: it checks the server's certificate "
               "against the Windows certificate store. Import the CA into the computer's Trusted Root Certification Authorities and remove TlsCaFile.");
         }

         return true;
      }

      // Read by verify-full only: the other modes check no certificate, and passing
      // the file on to PostgreSQL's require would quietly turn it into verify-ca.
      if (mode != ModeVerifyFull)
         return true;

      if (!FileUtilities::Exists(configuredCaFile))
      {
         error = Formatter::Format("TlsCaFile under [Database] in hMailServer.ini names {0}, which does not exist or cannot be read, and TlsMode=verify-full "
            "checks the database server's certificate against it. The connection was not attempted.", configuredCaFile);
         ErrorManager::Instance()->ReportError(ErrorManager::Critical, 7531, "DatabaseTls::FromConfiguration", error);
         return false;
      }

      caFile = configuredCaFile;
      return true;
   }

   bool
   DatabaseTls::OptionsNameEncryption(const String &options)
   {
      std::vector<String> pairs = StringParser::SplitString(options, _T(";"));

      for (const String &pair : pairs)
      {
         const int equals = pair.Find(_T("="));
         String key = equals >= 0 ? pair.Left(equals) : pair;

         // "Use Encryption for Data" and "UseEncryptionForData" are the same word to
         // the providers, which ignore case and, for these keys, spacing.
         key.Remove(_T(' '));
         key.Remove(_T('\t'));
         key.MakeLower();

         if (key == _T("encrypt") || key == _T("useencryptionfordata") || key == _T("trustservercertificate"))
            return true;
      }

      return false;
   }

   DatabaseTls::ProviderKind
   DatabaseTls::ClassifyProvider(const String &provider)
   {
      String name = provider;
      name.Trim();
      name.MakeLower();

      // MSOLEDBSQL19 before MSOLEDBSQL, which is its prefix. A version suffix -
      // MSOLEDBSQL.1, SQLOLEDB.1 - names the same provider.
      if (name.StartsWith(_T("msoledbsql19")))
         return ProviderModern19;
      if (name == _T("msoledbsql") || name.StartsWith(_T("msoledbsql.")))
         return ProviderModern;
      if (name.StartsWith(_T("sqlncli")))
         return ProviderModern;
      if (name.StartsWith(_T("sqloledb")))
         return ProviderLegacy;

      return ProviderOther;
   }

   bool
   DatabaseTls::SqlServerKeywords(Mode mode, const String &provider, const String &options, String &keywords, String &error)
   {
      keywords.Empty();

      if (OptionsNameEncryption(options))
      {
         if (mode == ModeDefault)
         {
            // The administrator's own words, as they always were
            // (ConnectionStringOptions is appended verbatim).
            return true;
         }

         error = Formatter::Format("TlsMode under [Database] in hMailServer.ini is {0}, and ConnectionStringOptions names SQL Server's encryption as well. "
            "Say it once: remove Encrypt, Use Encryption for Data and TrustServerCertificate from ConnectionStringOptions, or remove TlsMode. "
            "The connection was not attempted.", ModeName(mode));
         return false;
      }

      const ProviderKind kind = ClassifyProvider(provider);

      const String encryptNoCheck = _T("Use Encryption for Data=True;Trust Server Certificate=True;");
      const String encryptAndCheck = _T("Use Encryption for Data=True;Trust Server Certificate=False;");

      switch (mode)
      {
      case ModeDefault:
         // The providers this server picks for itself when [Database] Provider is
         // empty - MSOLEDBSQL, and SQLNCLI for a failover partner - encrypt only the
         // login unless asked, and every SQL Server since 2005 can encrypt the rest
         // with the certificate it makes for itself. MSOLEDBSQL19 already encrypts
         // and checks by default, which is stronger and is left alone; SQLOLEDB
         // cannot encrypt without checking the certificate against the Windows
         // store, which a server's own certificate fails, so it is left alone too.
         if (kind == ProviderModern)
            keywords = encryptNoCheck;
         return true;
      case ModeDisable:
         keywords = _T("Use Encryption for Data=False;");
         return true;
      case ModePrefer:
         // SQL Server always offers TLS, so prefer sends what require sends; the
         // difference is ADOConnection's one more attempt without the keywords when
         // the failure reads as TLS. Not on SQLOLEDB, which would check the
         // certificate, and so encrypts only the login as it always did.
         if (kind != ProviderLegacy)
            keywords = encryptNoCheck;
         return true;
      case ModeRequire:
         // SQLOLEDB has no way to encrypt without checking: require is verify-full on
         // it, which docs/DatabaseTls.md says.
         keywords = kind == ProviderLegacy ? String(_T("Use Encryption for Data=True;")) : encryptNoCheck;
         return true;
      case ModeVerifyFull:
         keywords = kind == ProviderLegacy ? String(_T("Use Encryption for Data=True;")) : encryptAndCheck;
         return true;
      }

      return true;
   }

   bool
   DatabaseTls::LooksLikeTlsFailure(const String &error)
   {
      // The provider's own words, without what it quotes: a login or a database
      // named hm_tls or mail_ssl ("Login failed for user 'hm_tls'.") is not a
      // failure of TLS, and must not be tried again without it.
      String lower;
      TCHAR quote = 0;
      for (int i = 0; i < error.GetLength(); i++)
      {
         const TCHAR c = error[i];

         if (quote != 0)
         {
            if (c == quote)
               quote = 0;
            continue;
         }

         // A single quote opens a quotation only where a word starts; inside a word
         // it is an apostrophe ("the server's certificate").
         const bool wordStart = i == 0 || error[i - 1] == _T(' ') || error[i - 1] == _T('(');
         if (c == _T('"') || (c == _T('\'') && wordStart))
         {
            quote = c;
            continue;
         }

         lower += c;
      }

      lower.MakeLower();

      static const TCHAR *const words[] =
      {
         _T("ssl"), _T("tls"), _T("certificate"), _T("encrypt"), _T("handshake"), _T("schannel")
      };

      for (const TCHAR *word : words)
      {
         if (lower.Find(word) >= 0)
            return true;
      }

      return false;
   }

   String
   DatabaseTls::BackendName_(DatabaseSettings::SQLDBType type)
   {
      switch (type)
      {
      case DatabaseSettings::TypeMYSQLServer:
         return _T("MySQL/MariaDB");
      case DatabaseSettings::TypePGServer:
         return _T("PostgreSQL");
      case DatabaseSettings::TypeMSSQLServer:
         return _T("SQL Server");
      case DatabaseSettings::TypeMSSQLCompactEdition:
         return _T("SQL Server Compact");
      default:
         return _T("the database");
      }
   }

   String
   DatabaseTls::Describe(DatabaseSettings::SQLDBType type, bool encrypted, const String &detail, bool verified, Mode mode)
   {
      String text = BackendName_(type) + _T(": ");

      if (encrypted)
      {
         text += _T("encrypted with TLS");
         if (!detail.IsEmpty())
            text += _T(" (") + detail + _T(")");

         text += verified
            ? _T(", the server's certificate checked")
            : _T(", the server's certificate not checked");
      }
      else
      {
         text += _T("NOT encrypted");
         if (!detail.IsEmpty())
            text += _T(": ") + detail;
      }

      text += _T(" (TlsMode ") + ModeName(mode) + _T(")");
      return text;
   }

   String
   DatabaseTls::Guidance(DatabaseSettings::SQLDBType type, Mode mode)
   {
      const Mode effective = Effective(mode);

      if (type == DatabaseSettings::TypeMSSQLServer)
      {
         if (effective == ModeVerifyFull)
         {
            return _T("TlsMode=verify-full under [Database] in hMailServer.ini asks SQL Server's provider to check the server's certificate: it must chain to a "
                      "root in the computer's Trusted Root Certification Authorities and carry the name in Server=. Correct the certificate or the store, or "
                      "set TlsMode=require to encrypt without checking it.");
         }

         return _T("The provider could not encrypt the connection to SQL Server. Set TlsMode=disable under [Database] in hMailServer.ini for a server that "
                   "cannot encrypt, which leaves only the login encrypted, as before this setting existed.");
      }

      if (effective == ModeVerifyFull)
      {
         return _T("TlsMode=verify-full under [Database] in hMailServer.ini requires TLS with a certificate that chains to TlsCaFile (or, when that is empty, "
                   "to the system's trust store) and names the host or address in Server=. Correct the certificate or TlsCaFile, or set TlsMode=require "
                   "to encrypt without checking the certificate.");
      }

      if (effective == ModeRequire)
      {
         return _T("TlsMode=require under [Database] in hMailServer.ini refuses a database server that does not offer TLS. Enable TLS on the database "
                   "server, or set TlsMode=prefer to connect without it when it is not offered.");
      }

      return _T("The TLS the database server offers could not be agreed on - an old server offering only TLS 1.0 or 1.1 is the usual cause. Update "
                "the server's TLS, or set TlsMode=disable under [Database] in hMailServer.ini to connect without it.");
   }

   boost::mutex &
   DatabaseTls::StateMutex_()
   {
      static boost::mutex mutex;
      return mutex;
   }

   String &
   DatabaseTls::LastConnection_()
   {
      static String last;
      return last;
   }

   void
   DatabaseTls::NoteConnection(const String &description)
   {
      bool changed = false;

      {
         boost::lock_guard<boost::mutex> guard(StateMutex_());
         String &last = LastConnection_();
         if (last != description)
         {
            last = description;
            changed = true;
         }
      }

      // The pool's connections are made before the log's settings are read, so the
      // first of these usually goes nowhere; AnnounceConnection says it once the
      // log is set up. This one is for a later connection that differs - a
      // reconnection that fell back to plaintext, say.
      if (changed)
      {
         LOG_APPLICATION(_T("Database connection: ") + description);
      }
   }

   String
   DatabaseTls::GetLastConnection()
   {
      boost::lock_guard<boost::mutex> guard(StateMutex_());
      return LastConnection_();
   }

   void
   DatabaseTls::AnnounceConnection()
   {
      const String last = GetLastConnection();
      if (last.IsEmpty())
         return;

      LOG_APPLICATION(_T("Database connection: ") + last);
   }

   void
   DatabaseTls::Print_(bool error, const String &line)
   {
      AnsiString text;
      Unicode::WideToMultiByte(line, text);
      text += "\n";

#ifdef HM_PLATFORM_POSIX
      std::fputs(text.c_str(), error ? stderr : stdout);
      std::fflush(error ? stderr : stdout);
#else
      // To the handle the process was given, as EnvironmentSettings::Print_ writes:
      // hMailServer.exe is a Windows-subsystem program whose C runtime streams may
      // be attached to nothing, and a caller that redirected the output - a script,
      // a test - reads the handle.
      HANDLE handle = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
      if (handle && handle != INVALID_HANDLE_VALUE)
      {
         DWORD written = 0;
         WriteFile(handle, text.c_str(), (DWORD) text.GetLength(), &written, nullptr);
      }
#endif
   }

   int
   DatabaseTls::RunCheckCommand()
   {
      IniFileSettings *ini = IniFileSettings::Instance();
      ini->LoadSettings();

      Print_(false, Formatter::Format("Configuration: {0}", IniFileSettings::GetInitializationFile()));

      if (!ini->GetDatabaseSettingsExists())
      {
         Print_(true, _T("[Database] does not name a database type (Type=), so there is nothing to connect to. Nothing was checked."));
         return 2;
      }

      const DatabaseSettings::SQLDBType type = ini->GetDatabaseType();

      if (type == DatabaseSettings::TypeMSSQLCompactEdition)
      {
         Print_(false, _T("Database: SQL Server Compact, a file on this machine. Nothing crosses a network, so there is no TLS to check."));
         return 0;
      }

      const String server = ini->GetDatabaseServer();
      const int port = (int) ini->GetDatabasePort();
      const String name = ini->GetDatabaseName();

      String where = server;
      if (port > 0)
         where += Formatter::Format(":{0}", port);

      Print_(false, Formatter::Format("Database: {0} on {1}, database {2}, as {3}", BackendName_(type), where,
         name.IsEmpty() ? String(_T("(none named)")) : name, ini->GetUsername()));

      Mode mode = ModeDefault;
      String caFile;
      String error;
      if (!FromConfiguration(type, mode, caFile, error))
      {
         Print_(true, _T("Not connected: ") + error);
         return 1;
      }

      String settingsLine = _T("TLS: TlsMode ") + ModeName(mode);

      if (type == DatabaseSettings::TypePGServer && !ini->GetDatabasePostgreSQLSslMode().IsEmpty())
      {
         settingsLine = _T("TLS: PostgreSQLSslMode ") + ini->GetDatabasePostgreSQLSslMode() + _T(", which wins over TlsMode for PostgreSQL");
      }
      else if (type == DatabaseSettings::TypePGServer && mode == ModeDefault)
      {
         settingsLine = _T("TLS: libpq's own default, prefer - or PGSSLMODE when this process's environment sets it");
      }
      else if (mode == ModeVerifyFull && type == DatabaseSettings::TypeMSSQLServer)
      {
         settingsLine += _T(", the certificate checked by the provider against the Windows certificate store");
      }
      else if (mode == ModeVerifyFull && !caFile.IsEmpty())
      {
         settingsLine += _T(", the certificate checked against ") + caFile;
      }
      else if (mode == ModeVerifyFull && type == DatabaseSettings::TypePGServer)
      {
#ifdef HM_PLATFORM_POSIX
         settingsLine += _T(", the certificate checked against the system's trust store (sslrootcert=system; TlsCaFile is not set)");
#else
         settingsLine += _T(", the certificate checked against libpq's own root.crt in the service account's %APPDATA%\\postgresql (TlsCaFile is not set)");
#endif
      }
      else if (mode == ModeVerifyFull)
      {
         settingsLine += _T(", the certificate checked against the system's trust store (TlsCaFile is not set)");
      }

      Print_(false, settingsLine);

      std::shared_ptr<DatabaseSettings> settings = std::make_shared<DatabaseSettings>(ini->GetDatabaseProvider(), server, name, ini->GetUsername(),
         ini->GetPassword(), ini->GetDatabaseDirectory(), ini->GetDatabaseServerFailoverPartner(), type, ini->GetDatabasePort());

      std::shared_ptr<DALConnection> connection = DALConnectionFactory::CreateConnection(settings);
      if (!connection)
      {
         Print_(true, Formatter::Format("This build has no {0} backend. Nothing was checked.", BackendName_(type)));
         return 2;
      }

      String connectError;
      if (connection->Connect(connectError) != DALConnection::Connected)
      {
         // libpq ends its messages with a line break of its own.
         connectError.TrimRight();
         Print_(true, _T("Not connected: ") + connectError);
         connection->Disconnect();
         return 1;
      }

      Print_(false, _T("Connected. ") + connection->GetTransportSecurity());
      connection->Disconnect();
      return 0;
   }

   void
   DatabaseTlsTester::Test()
   {
      DatabaseTls::Mode mode = DatabaseTls::ModeRequire;

      // The four words and empty, with space and case as an administrator types
      // them; anything else refused, verify-ca and a libpq-style underscore among
      // them, and the words a boolean key would take.
      Assert::IsTrue(DatabaseTls::ParseMode(_T(""), mode) && mode == DatabaseTls::ModeDefault);
      Assert::IsTrue(DatabaseTls::ParseMode(_T("   "), mode) && mode == DatabaseTls::ModeDefault);
      Assert::IsTrue(DatabaseTls::ParseMode(_T("disable"), mode) && mode == DatabaseTls::ModeDisable);
      Assert::IsTrue(DatabaseTls::ParseMode(_T("PREFER"), mode) && mode == DatabaseTls::ModePrefer);
      Assert::IsTrue(DatabaseTls::ParseMode(_T("require"), mode) && mode == DatabaseTls::ModeRequire);
      Assert::IsTrue(DatabaseTls::ParseMode(_T("  Verify-Full "), mode) && mode == DatabaseTls::ModeVerifyFull);
      Assert::IsFalse(DatabaseTls::ParseMode(_T("verify-ca"), mode));
      Assert::IsFalse(DatabaseTls::ParseMode(_T("verify_full"), mode));
      Assert::IsFalse(DatabaseTls::ParseMode(_T("true"), mode));
      Assert::IsFalse(DatabaseTls::ParseMode(_T("1"), mode));
      Assert::IsTrue(DatabaseTls::UnknownModeMessage(_T("verify-ca")).Find(_T("'verify-ca'")) >= 0);

      Assert::IsTrue(DatabaseTls::Effective(DatabaseTls::ModeDefault) == DatabaseTls::ModePrefer);
      Assert::IsTrue(DatabaseTls::Effective(DatabaseTls::ModeDisable) == DatabaseTls::ModeDisable);

      // SQL Server's encryption keywords, in either of the providers' spellings -
      // and not Always Encrypted's, which is about columns, not the connection.
      Assert::IsTrue(DatabaseTls::OptionsNameEncryption(_T("Encrypt=yes;TrustServerCertificate=no")));
      Assert::IsTrue(DatabaseTls::OptionsNameEncryption(_T("Application Name=hMailServer;Use Encryption for Data=True")));
      Assert::IsTrue(DatabaseTls::OptionsNameEncryption(_T(" trust server certificate = true ;")));
      Assert::IsFalse(DatabaseTls::OptionsNameEncryption(_T("Column Encryption Setting=Enabled")));
      Assert::IsFalse(DatabaseTls::OptionsNameEncryption(_T("Application Name=hMailServer;Packet Size=4096")));
      Assert::IsFalse(DatabaseTls::OptionsNameEncryption(_T("")));

      Assert::IsTrue(DatabaseTls::ClassifyProvider(_T("MSOLEDBSQL")) == DatabaseTls::ProviderModern);
      Assert::IsTrue(DatabaseTls::ClassifyProvider(_T("msoledbsql.1")) == DatabaseTls::ProviderModern);
      Assert::IsTrue(DatabaseTls::ClassifyProvider(_T("MSOLEDBSQL19")) == DatabaseTls::ProviderModern19);
      Assert::IsTrue(DatabaseTls::ClassifyProvider(_T("SQLNCLI11")) == DatabaseTls::ProviderModern);
      Assert::IsTrue(DatabaseTls::ClassifyProvider(_T("sqloledb")) == DatabaseTls::ProviderLegacy);
      Assert::IsTrue(DatabaseTls::ClassifyProvider(_T("SQLOLEDB.1")) == DatabaseTls::ProviderLegacy);
      Assert::IsTrue(DatabaseTls::ClassifyProvider(_T("SomethingElse")) == DatabaseTls::ProviderOther);

      // What each mode asks of each provider.
      const String none;
      const String encryptNoCheck = _T("Use Encryption for Data=True;Trust Server Certificate=True;");
      const String encryptAndCheck = _T("Use Encryption for Data=True;Trust Server Certificate=False;");
      const String encryptLegacy = _T("Use Encryption for Data=True;");
      String keywords;
      String error;

      Assert::IsTrue(DatabaseTls::SqlServerKeywords(DatabaseTls::ModeDefault, _T("MSOLEDBSQL"), _T(""), keywords, error));
      Assert::AreEqual(encryptNoCheck, keywords);
      Assert::IsTrue(DatabaseTls::SqlServerKeywords(DatabaseTls::ModeDefault, _T("SQLNCLI"), _T(""), keywords, error));
      Assert::AreEqual(encryptNoCheck, keywords);
      Assert::IsTrue(DatabaseTls::SqlServerKeywords(DatabaseTls::ModeDefault, _T("MSOLEDBSQL19"), _T(""), keywords, error));
      Assert::AreEqual(none, keywords);
      Assert::IsTrue(DatabaseTls::SqlServerKeywords(DatabaseTls::ModeDefault, _T("sqloledb"), _T(""), keywords, error));
      Assert::AreEqual(none, keywords);
      Assert::IsTrue(DatabaseTls::SqlServerKeywords(DatabaseTls::ModeDefault, _T("MSOLEDBSQL"), _T("Encrypt=no"), keywords, error));
      Assert::AreEqual(none, keywords);
      Assert::IsTrue(DatabaseTls::SqlServerKeywords(DatabaseTls::ModeDisable, _T("MSOLEDBSQL"), _T(""), keywords, error));
      Assert::AreEqual(String(_T("Use Encryption for Data=False;")), keywords);
      Assert::IsTrue(DatabaseTls::SqlServerKeywords(DatabaseTls::ModePrefer, _T("sqloledb"), _T(""), keywords, error));
      Assert::AreEqual(none, keywords);
      Assert::IsTrue(DatabaseTls::SqlServerKeywords(DatabaseTls::ModeRequire, _T("MSOLEDBSQL19"), _T(""), keywords, error));
      Assert::AreEqual(encryptNoCheck, keywords);
      Assert::IsTrue(DatabaseTls::SqlServerKeywords(DatabaseTls::ModeRequire, _T("sqloledb"), _T(""), keywords, error));
      Assert::AreEqual(encryptLegacy, keywords);
      Assert::IsTrue(DatabaseTls::SqlServerKeywords(DatabaseTls::ModeVerifyFull, _T("MSOLEDBSQL"), _T(""), keywords, error));
      Assert::AreEqual(encryptAndCheck, keywords);
      Assert::IsTrue(DatabaseTls::SqlServerKeywords(DatabaseTls::ModeVerifyFull, _T("sqloledb"), _T(""), keywords, error));
      Assert::AreEqual(encryptLegacy, keywords);

      // Two settings saying it: refused, with both named, and nothing appended.
      error.Empty();
      Assert::IsFalse(DatabaseTls::SqlServerKeywords(DatabaseTls::ModeVerifyFull, _T("MSOLEDBSQL"), _T("TrustServerCertificate=yes"), keywords, error));
      Assert::AreEqual(none, keywords);
      Assert::IsTrue(error.Find(_T("ConnectionStringOptions")) >= 0 && error.Find(_T("verify-full")) >= 0);

      // Which SQL Server failures read as TLS, and which do not: a wrong password is
      // not one, so it is never tried a second time.
      Assert::IsTrue(DatabaseTls::LooksLikeTlsFailure(_T("SSL Provider: The certificate chain was issued by an authority that is not trusted.")));
      Assert::IsTrue(DatabaseTls::LooksLikeTlsFailure(_T("[DBNETLIB][ConnectionOpen (SECDoClientHandshake()).]SSL Security error.")));
      Assert::IsTrue(DatabaseTls::LooksLikeTlsFailure(_T("Client unable to establish connection because an error was encountered during handshakes before login.")));
      Assert::IsTrue(DatabaseTls::LooksLikeTlsFailure(_T("Encryption not supported on the client.")));
      Assert::IsFalse(DatabaseTls::LooksLikeTlsFailure(_T("Login failed for user 'hmailserver'.")));
      Assert::IsFalse(DatabaseTls::LooksLikeTlsFailure(_T("Cannot open database \"mail\" requested by the login.")));
      Assert::IsFalse(DatabaseTls::LooksLikeTlsFailure(_T("Login failed for user 'hm_tls'.")));
      Assert::IsTrue(DatabaseTls::LooksLikeTlsFailure(_T("The server's certificate was not trusted.")));
      Assert::IsFalse(DatabaseTls::LooksLikeTlsFailure(_T("Cannot open database \"mail_ssl\" requested by the login. The login failed.")));

      // The line an administrator reads, both ways round.
      const String encrypted = DatabaseTls::Describe(DatabaseSettings::TypeMYSQLServer, true, _T("TLS_AES_256_GCM_SHA384"), false, DatabaseTls::ModeDefault);
      Assert::AreEqual(String(_T("MySQL/MariaDB: encrypted with TLS (TLS_AES_256_GCM_SHA384), the server's certificate not checked (TlsMode prefer, the default)")), encrypted);
      const String plain = DatabaseTls::Describe(DatabaseSettings::TypePGServer, false, _T("the server offered no TLS"), false, DatabaseTls::ModePrefer);
      Assert::AreEqual(String(_T("PostgreSQL: NOT encrypted: the server offered no TLS (TlsMode prefer)")), plain);
      const String checked = DatabaseTls::Describe(DatabaseSettings::TypeMSSQLServer, true, _T(""), true, DatabaseTls::ModeVerifyFull);
      Assert::AreEqual(String(_T("SQL Server: encrypted with TLS, the server's certificate checked (TlsMode verify-full)")), checked);
   }
}
