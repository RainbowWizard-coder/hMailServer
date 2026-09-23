// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The POSIX service host.
//
// On Windows the server is an ATL out-of-process COM server that registers
// itself with the Service Control Manager, and hMailServer.cpp is 500 lines of
// registration, COM plumbing and SCM callbacks. None of that has a meaning here:
// systemd starts a process, expects it to keep running, and asks it to stop with
// a signal. So this file is what is left when all of that is taken away - start
// the application, wait, stop it - and it is deliberately short enough to read in
// one go, because everything that could go wrong in it takes the mail server
// with it.
//
// It is not built on Windows and is not in hMailServer.vcxproj.
//
// WHAT THIS DELIBERATELY DOES NOT DO
//   - It does not daemonise. systemd's Type=simple wants the process in the
//     foreground; forking would hide the exit code and confuse the supervisor.
//   - It does not write a pid file. systemd tracks the process itself.
//   - It does not serve COM. There is none here: administration on this platform
//     is the REST API and the configuration file, which is what the roadmap's
//     "Administration without COM" row is about.
#include "StdAfx.h"

#include <csignal>
#include <termios.h>
#include <unistd.h>
#include <iostream>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <cstdlib>
#include <map>
#include <string>

#include "../Common/Application/Application.h"
#include "../Common/Application/Version.h"
#include "../Common/Application/IniFileSettings.h"
#include "../Common/Application/IniSettingStore.h"
#include "../Common/Application/EnvironmentSettings.h"
#include "../Common/Application/Profiles.h"
#include "../Common/Application/SampleDomain.h"
#include "../Common/Application/Logger.h"
#include "../Common/Util/Utilities.h"
#include "../Common/Application/Constants.h"
#include "../Common/Util/FileUtilities.h"
#include "../Common/SQL/DatabaseSettings.h"
#include "../Common/SQL/DALConnectionFactory.h"
#include "../Common/SQL/DALConnection.h"
#include "../Common/SQL/DALRecordset.h"
#include "../Common/SQL/SQLStatement.h"
#include "../Common/SQL/SQLCommand.h"
#include "../Common/SQL/SQLScriptRunner.h"
#include "../Common/Persistence/SchemaUpgrader.h"
#include "../Common/Util/CrashOracle.h"
#include "../Common/Util/SigstoreVerifier.h"
#include "../Common/Util/UpdateChecker.h"

namespace
{
   // Set by the signal handler and read by the main thread. sig_atomic_t is the
   // only type a handler may touch, and the condition variable it is paired with
   // is notified from the main loop's own wake-up rather than from the handler:
   // pthread_cond_signal is not async-signal-safe, and a mail server that
   // deadlocks in its own shutdown is worse than one that takes a second longer.
   volatile sig_atomic_t stop_requested = 0;
   volatile sig_atomic_t reload_requested = 0;

   extern "C" void HandleSignal(int number)
   {
      switch (number)
      {
      case SIGTERM:
      case SIGINT:
         stop_requested = 1;
         break;
      case SIGHUP:
         reload_requested = 1;
         break;
      default:
         break;
      }
   }

   void InstallHandlers()
   {
      struct sigaction action;
      std::memset(&action, 0, sizeof(action));
      action.sa_handler = HandleSignal;
      sigemptyset(&action.sa_mask);
      // No SA_RESTART: a blocking read in a worker should come back with EINTR
      // when we are stopping rather than hold the shutdown open.
      action.sa_flags = 0;

      sigaction(SIGTERM, &action, nullptr);
      sigaction(SIGINT, &action, nullptr);
      sigaction(SIGHUP, &action, nullptr);

      // A peer that closes a socket mid-write must not kill the process. The
      // Windows build never sees this because Winsock reports it as an error
      // code; here the default disposition is death.
      struct sigaction ignore;
      std::memset(&ignore, 0, sizeof(ignore));
      ignore.sa_handler = SIG_IGN;
      sigaction(SIGPIPE, &ignore, nullptr);
   }

   void PrintUsage(const char *program)
   {
      std::printf(
         "hMailServer %s\n"
         "\n"
         "Usage: %s [options]\n"
         "\n"
         "  --config <file>   the configuration file to read\n"
         "                    (default: hMailServer.ini beside this executable,\n"
         "                    then /etc/hmailserver/hMailServer.ini)\n"
         "  --foreground      run in the foreground and log to standard error as\n"
         "                    well as to the log directory. This is the default;\n"
         "                    the option exists so that a unit file can say so.\n"
         "  --check-config    read the configuration, report what it says, and\n"
         "                    exit without opening a listener or the database\n"
         "  --set-admin-password\n"
         "                    read a new administrator password from standard\n"
         "                    input, hash it with PBKDF2 and write it to the\n"
         "                    configuration file, then exit. This is how the\n"
         "                    password is set where there is no Control Panel.\n"
         "  --create-database create the database [Database] names, on the server it\n"
         "                    names, and run the create script for its type. What\n"
         "                    DBSetupQuick does on Windows; there is no COM here.\n"
         "                    A database that already holds this build's schema, or\n"
         "                    an older one, is left as it is with exit status 0, so\n"
         "                    it can run at every start; a newer one exits 1.\n"
         "  --upgrade-database\n"
         "                    run every upgrade script from the database's version\n"
         "                    to this build's, in order. What DBUpdater does on\n"
         "                    Windows. The installer's post-install step runs it.\n"
         "  --set-setting NAME=VALUE\n"
         "  --set-setting NAME\n"
         "                    store one [Settings] value in the database, which is\n"
         "                    the settings store, write the server's copy of it into\n"
         "                    the configuration file, and exit. For the settings the\n"
         "                    server has to have before its REST API can be reached\n"
         "                    - RestApiPort above all. Editing the file instead sets\n"
         "                    nothing: the stored value wins at the next start. The\n"
         "                    name must be one this server reads; the value applies\n"
         "                    when the server next starts or is reloaded. With NAME\n"
         "                    alone the value is one line of standard input, not\n"
         "                    echoed at a terminal for a secret; a secret's value is\n"
         "                    refused on the command line, where the shell history,\n"
         "                    the process list and sudo's log would keep it. A port\n"
         "                    must be 0-65535 and RestApiBindAddress an IP address\n"
         "                    or localhost. Run it as root, as --set-admin-password\n"
         "                    is: the file is root's.\n"
         "  --apply-environment\n"
         "                    store every setting the environment names -\n"
         "                    HM_SETTING_<key> for a [Settings] key, HM_PROPERTY_<name>\n"
         "                    for an hm_settings row, either with _FILE to read the\n"
         "                    value from a file (a Kubernetes Secret), and the\n"
         "                    container's HM_REST_* variables - checked first and\n"
         "                    stored only when every one is right; one line each,\n"
         "                    set, unchanged or refused, and never a value from a\n"
         "                    file or a secret. Exits 1 on any refusal, having\n"
         "                    changed nothing. An administrator's change to a\n"
         "                    setting named is refused over REST and COM until a\n"
         "                    run without it. The container's entrypoint runs it\n"
         "                    when there is anything to apply; run it as the user\n"
         "                    the service runs as.\n"
         "  --apply-profile NAME [--confirm] [--once]\n"
         "                    apply one of the setting profiles - office,\n"
         "                    gateway, relay or sender (docs/Profiles.md) - as\n"
         "                    the REST API applies it: every entry checked, the\n"
         "                    values it changes saved as the way back, then\n"
         "                    each written, one line each. --confirm is needed\n"
         "                    where an entry would close a listener, trust an\n"
         "                    address range or turn a filter off; --once does\n"
         "                    nothing on a server a profile was applied to\n"
         "                    before, which is what the container's HM_PROFILE\n"
         "                    runs. Exit 3 when a --confirm is needed. Run it as\n"
         "                    the user the service runs as.\n"
         "  --sample-domain create\n"
         "  --sample-domain remove\n"
         "                    make a throwaway example.test - three mailboxes\n"
         "                    with one documented password, a distribution list\n"
         "                    of the three, one rule and eight delivered\n"
         "                    messages - so that every page of an administration\n"
         "                    interface has something in it while somebody is\n"
         "                    learning, and take all of it away again, saying\n"
         "                    what went. A domain named example.test that this\n"
         "                    did not make is never created over and never\n"
         "                    removed. Run it as the user the service runs as:\n"
         "                    it reads the configuration, opens the database and\n"
         "                    writes to the log directory as the server does.\n"
         "                    Remove the sample before this server takes real\n"
         "                    mail - the password is in the documentation.\n"
         "  --verify-update <file>\n"
         "                    check a downloaded release file - a package or an\n"
         "                    AppImage - against the Sigstore bundle beside it\n"
         "                    (<file>.cosign.bundle), the way the server checks\n"
         "                    one before it is applied, and exit 0 when it is the\n"
         "                    release workflow's file and 1 when it is not. The\n"
         "                    trust is the configuration's (UpdateTrustRootsFile\n"
         "                    and the settings beside it) or the public Sigstore\n"
         "                    instance. hmailserver-update runs this as root\n"
         "                    before it installs anything.\n"
         "  --version         print the version and exit\n"
         "  --help            print this and exit\n"
         "\n"
         "Signals: SIGTERM and SIGINT stop the server; SIGHUP re-reads the\n"
         "configuration the way the Control Panel's Reinitialize does.\n",
         HMAILSERVER_VERSION, program);
   }
}


namespace
{
   // The two database commands. Both read [Database] from the configuration the
   // way the server does, and both refuse the two backends this platform does
   // not have by name rather than by a connection error.

   const char *DescribeType(HM::DatabaseSettings::SQLDBType type)
   {
      switch (type)
      {
      case HM::DatabaseSettings::TypeMYSQLServer:            return "MySQL";
      case HM::DatabaseSettings::TypePGServer:               return "PostgreSQL";
      case HM::DatabaseSettings::TypeMSSQLServer:            return "SQL Server";
      case HM::DatabaseSettings::TypeMSSQLCompactEdition:    return "SQL Server Compact";
      default:                                               return "unknown";
      }
   }

   bool ReadDatabaseSettings(std::shared_ptr<HM::DatabaseSettings> &withDatabase,
                             std::shared_ptr<HM::DatabaseSettings> &serverOnly)
   {
      HM::IniFileSettings *ini = HM::IniFileSettings::Instance();
      ini->LoadSettings();

      const HM::DatabaseSettings::SQLDBType type = ini->GetDatabaseType();
      const HM::String name = ini->GetDatabaseName();

      if (type == HM::DatabaseSettings::TypeUnknown || name.IsEmpty())
      {
         std::fprintf(stderr,
            "[Database] in %s does not name a Type and a Database. Set Type=PostgreSQL or\n"
            "Type=MySQL, the Server, Port, Username, Password and Database, and try again.\n",
            HM::AnsiString(HM::IniFileSettings::GetInitializationFile()).c_str());
         return false;
      }

      if (type == HM::DatabaseSettings::TypeMSSQLServer || type == HM::DatabaseSettings::TypeMSSQLCompactEdition)
      {
         std::fprintf(stderr,
            "[Database] Type is %s, which this build does not have: ADO and SQL Server Compact are\n"
            "Windows. PostgreSQL and MySQL are the backends here.\n", DescribeType(type));
         return false;
      }

      const HM::String empty;
      withDatabase = std::make_shared<HM::DatabaseSettings>(
         ini->GetDatabaseProvider(), ini->GetDatabaseServer(), name, ini->GetUsername(), ini->GetPassword(),
         empty, empty, type, ini->GetDatabasePort());
      serverOnly = std::make_shared<HM::DatabaseSettings>(
         ini->GetDatabaseProvider(), ini->GetDatabaseServer(), empty, ini->GetUsername(), ini->GetPassword(),
         empty, empty, type, ini->GetDatabasePort());
      return true;
   }

   std::shared_ptr<HM::DALConnection> Connect(std::shared_ptr<HM::DatabaseSettings> settings, const char *what, bool quietly = false)
   {
      std::shared_ptr<HM::DALConnection> connection = HM::DALConnectionFactory::CreateConnection(settings);
      HM::String error;
      if (!connection || connection->Connect(error) != HM::DALConnection::Connected)
      {
         if (!quietly)
            std::fprintf(stderr, "Could not connect to %s: %s\n", what, HM::AnsiString(error).c_str());
         return std::shared_ptr<HM::DALConnection>();
      }
      return connection;
   }

   int ReadSchemaVersion(std::shared_ptr<HM::DALConnection> connection)
   {
      std::shared_ptr<HM::DALRecordset> recordset = connection->CreateRecordset();
      if (!recordset || !recordset->Open(connection, HM::SQLCommand("select * from hm_dbversion")))
         return 0;
      if (recordset->IsEOF())
         return 0;
      return (int) recordset->GetLongValue("value");
   }

   int CreateDatabase()
   {
      std::shared_ptr<HM::DatabaseSettings> withDatabase, serverOnly;
      if (!ReadDatabaseSettings(withDatabase, serverOnly))
         return 2;

      // Same sequence as the COM CreateExternalDatabase: connect to the server
      // with no database named, CREATE DATABASE in the backend's own dialect,
      // reconnect to the new database, run the create script - which produces
      // the current schema outright, so a fresh database needs no upgrade.
      //
      // Unless the database is already there. An administrator who ran createdb
      // themselves - the usual shape when the service role is not allowed to
      // create databases - has an empty database that this should fill rather
      // than a CREATE DATABASE failure to read; and one that already holds an
      // hMailServer schema must not have the create script run over it.
      //
      // That last case is not a failure, and until 15 September 2026 it exited 1.
      // The container's entrypoint - and so the Helm chart - runs this at EVERY
      // start, under `set -e` and before --upgrade-database, on the understanding
      // that it does nothing once the database exists; so every start after the
      // first stopped the container here, before the server or the REST settings
      // the entrypoint stores after it. Nothing is created and the exit status is
      // 0 for this build's schema or an older one, which --upgrade-database brings
      // forward. A schema from a newer build is the one refusal, because nothing
      // that runs after this can make it usable.
      HM::String error;
      std::shared_ptr<HM::DALConnection> database = Connect(withDatabase, "the database", true);
      if (database)
      {
         const int existing = ReadSchemaVersion(database);
         if (existing > 0)
         {
            const HM::AnsiString databaseName(withDatabase->GetDatabaseName());
            if (existing > REQUIRED_DB_VERSION)
            {
               std::fprintf(stderr, "Database %s already exists and is an hMailServer database at schema version %d, from a NEWER build than this one, which needs %d. Nothing was changed, and this build will not run against it.\n",
                  databaseName.c_str(), existing, REQUIRED_DB_VERSION);
               return 1;
            }
            if (existing == REQUIRED_DB_VERSION)
            {
               std::printf("Database %s already exists and is an hMailServer database at schema version %d, which is what this build needs. Nothing was created.\n",
                  databaseName.c_str(), existing);
            }
            else
            {
               std::printf("Database %s already exists and is an hMailServer database at schema version %d; this build needs %d. Nothing was created: --upgrade-database brings it forward.\n",
                  databaseName.c_str(), existing, REQUIRED_DB_VERSION);
            }
            return 0;
         }
         std::printf("Database %s already exists and holds no hMailServer schema; creating the schema in it.\n",
            HM::AnsiString(withDatabase->GetDatabaseName()).c_str());
      }
      else
      {
         std::shared_ptr<HM::DALConnection> server = Connect(serverOnly, "the database server");
         if (!server)
            return 1;

         const HM::String create = HM::SQLStatement::GetCreateDatabase(serverOnly, withDatabase->GetDatabaseName());
         if (!server->Execute(HM::SQLCommand(create), error, 0, 0))
         {
            std::fprintf(stderr, "CREATE DATABASE failed: %s\n", HM::AnsiString(error).c_str());
            return 1;
         }

         database = Connect(withDatabase, "the new database");
         if (!database)
            return 1;
      }

      const HM::String script = withDatabase->GetDefaultScript();
      if (!HM::FileUtilities::Exists(script))
      {
         std::fprintf(stderr, "The create script is not where the configuration says: %s\n"
            "ProgramFolder in [Directories] should be the directory that holds DBScripts.\n",
            HM::AnsiString(script).c_str());
         return 1;
      }

      HM::SQLScriptRunner runner;
      if (!runner.ExecuteScript(database, script, error))
      {
         std::fprintf(stderr, "The create script failed: %s\n", HM::AnsiString(error).c_str());
         return 1;
      }

      std::printf("Database %s created on %s at schema version %d, from %s.\n",
         HM::AnsiString(withDatabase->GetDatabaseName()).c_str(), DescribeType(withDatabase->GetType()),
         ReadSchemaVersion(database), HM::AnsiString(script).c_str());
      return 0;
   }

   int UpgradeDatabase()
   {
      std::shared_ptr<HM::DatabaseSettings> withDatabase, serverOnly;
      if (!ReadDatabaseSettings(withDatabase, serverOnly))
         return 2;

      std::shared_ptr<HM::DALConnection> database = Connect(withDatabase, "the database");
      if (!database)
         return 1;

      if (ReadSchemaVersion(database) == 0)
      {
         std::fprintf(stderr, "The database has no hm_dbversion row, so it is not an hMailServer database or was never created. --create-database makes one.\n");
         return 1;
      }

      // The walk itself is SchemaUpgrader's - the unit the service runs at its
      // own start under [Database] UpgradeOnStart, and hMailServer.exe runs on
      // Windows under this same option - so that the three cannot drift. It
      // scans DBScripts for the next step rather than guessing the number, runs
      // the backend's prerequisites script first as DBUpdater does, and takes
      // each step in a transaction. This file used to carry the walk itself,
      // without the prerequisites script that two of the PostgreSQL steps call.
      const HM::String directory = HM::IniFileSettings::Instance()->GetDBScriptDirectory();
      const HM::SchemaUpgrader::Outcome outcome = HM::SchemaUpgrader::Upgrade(database, directory, REQUIRED_DB_VERSION,
         [](const HM::String &line) { std::printf("%s\n", HM::AnsiString(line).c_str()); });

      if (!outcome.ok)
      {
         std::fprintf(stderr, "%s\nStopped at %s. The database is at %d; the steps before that one have been applied, and a second run resumes there.\n",
            HM::AnsiString(outcome.error).c_str(), HM::AnsiString(outcome.failed_step).c_str(), outcome.version);
         return 1;
      }

      if (outcome.steps == 0)
      {
         std::printf("The database is at schema version %d, which is what this build needs. Nothing to do.\n", outcome.version);
         return 0;
      }

      std::printf("Upgraded in %d step%s; the database is at schema version %d.\n", outcome.steps, outcome.steps == 1 ? "" : "s", outcome.version);
      return 0;
   }

   // The checks a value gets beyond what the store can hold, for the keys where a
   // value the store takes happily is one the server cannot use: the loader reads a
   // port and every other whole-number key with GetPrivateProfileInt, so "abc" and
   // "" are 0 - a port's listener off - and 99999 is no port at all; the REST
   // listener's address is parsed only when the listener starts, and a bad one is a
   // line in the application log. Stored and reported as a success, either would
   // leave the administrator who ran this to turn the REST listener on with no
   // listener, no hmctl and nothing on the terminal - the situation this verb exists
   // for. The checks are the ones hmailserver --apply-environment makes, in its
   // words (EnvironmentSettings::CheckIniValue), whose sentences never repeat the
   // value; every other key is held only to what COM SetIniSetting and PUT
   // /api/v1/settings/ini hold it to.
   bool CheckValue(const HM::String &name, const HM::String &value)
   {
      const HM::String problem = HM::EnvironmentSettings::CheckIniValue(name, value);
      if (problem.IsEmpty())
         return true;

      const HM::AnsiString sentence(problem);
      std::fprintf(stderr, "%s Nothing was changed.\n", sentence.c_str());
      return false;
   }

   // One line of standard input, for a value that is not to be on the command
   // line. At a terminal it asks, and a secret is not echoed while it is typed -
   // the way --set-admin-password reads a password. False when nothing could be
   // read at all.
   bool ReadValueFromInput(const HM::AnsiString &shown, bool secret, std::string &line)
   {
      const bool interactive = ::isatty(STDIN_FILENO) != 0;
      struct termios original;
      bool echo_disabled = false;
      if (interactive)
      {
         std::printf("Value for %s: ", shown.c_str());
         std::fflush(stdout);
         if (secret && ::tcgetattr(STDIN_FILENO, &original) == 0)
         {
            struct termios quiet = original;
            quiet.c_lflag &= ~(tcflag_t) ECHO;
            echo_disabled = ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet) == 0;
         }
      }

      const bool got = static_cast<bool>(std::getline(std::cin, line));

      if (echo_disabled)
      {
         ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
         std::printf("\n");
      }

      while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
         line.pop_back();

      return got;
   }

   // --set-setting NAME=VALUE, or --set-setting NAME with the value on standard
   // input.
   //
   // From schema 6042 the database is the settings store and the [Settings]
   // section of the configuration file is the server's copy of it: a value
   // edited into the file is named in the error log (HM5804) and put back at the
   // next start. That closes the one door a Linux administrator had to the
   // setting they need FIRST - the packaged file ships RestApiPort=0, the first
   // start adopts it as stored, and hmctl cannot turn the listener on because
   // it needs the listener. So this is the door: the same write the Control
   // Panel and PUT /api/v1/settings/ini/{name} make, the row first and the file
   // second (IniSettingStore::WriteSetting says why that order), with the
   // database opened the way the server opens it and closed again.
   //
   // Refused: a name the store could not hold or a value it could not (the
   // sentences SetIniSetting uses over COM); a name this build does not read,
   // since storing it would change nothing and say it had; a port or a REST
   // listener address the server could not use (CheckValue); and a secret's
   // value on the command line (below). The file must be writable by this user:
   // on the packaged layout it is root's, 0640, and a row stored without its
   // line would be reported at every start (HM5803) until somebody could write
   // the line - so the refusal comes first and says to use sudo, as
   // --set-admin-password does.
   int SetSetting(const std::string &assignment)
   {
      const std::string::size_type equals = assignment.find('=');
      if (assignment.empty() || equals == 0)
      {
         std::fprintf(stderr, "--set-setting takes NAME=VALUE, or NAME alone with the value on standard input; for example --set-setting RestApiPort=8080.\n");
         return 2;
      }

      // NAME alone: the value is one line of standard input, read once every
      // check that needs no value has passed, so nobody types a secret only to be
      // told the command has to be run as root.
      const bool fromInput = equals == std::string::npos;

      HM::String name((fromInput ? assignment : assignment.substr(0, equals)).c_str());
      name.Trim();

      HM::String value;
      if (!fromInput)
      {
         value = HM::String(assignment.substr(equals + 1).c_str());
         value.Trim();
      }

      if (!HM::IniSettingStore::IsStorableName(name))
      {
         std::fprintf(stderr, "'%s' cannot be a setting name: it is empty, longer than 100 characters, or contains one of = [ ] or a line break.\n",
            HM::AnsiString(name).c_str());
         return 2;
      }

      HM::String spelling;
      if (!HM::IniSettingStore::IsSettingTheServerReads(name, spelling))
      {
         std::fprintf(stderr, "'%s' is not one of the [Settings] keys the server's settings loader reads, so storing it here would change nothing. "
            "README.md's Configuration reference and the wiki's Settings Reference describe them. The webmail's branding and file-link "
            "settings, and a setting of your own that an event script reads, are stored over the REST API once the listener is on: "
            "PUT /api/v1/settings/ini/{name} takes any name.\n",
            HM::AnsiString(name).c_str());
         return 2;
      }
      name = spelling;

      const HM::AnsiString shown(name);

      // A secret's value is not taken from the command line at all: there it is
      // in the shell's history, in the process list for as long as this runs, and
      // in the COMMAND= line sudo writes to the system log - the reasons
      // --set-admin-password reads its password from standard input. By the time
      // this runs it has been disclosed, and all a refusal can do is say so and
      // not print it again. An empty value discloses nothing and is how a secret
      // is cleared.
      const bool secret = HM::IniSettingStore::IsSecretName(name);
      if (!fromInput && secret && !value.IsEmpty())
      {
         std::fprintf(stderr, "%s holds a secret, so its value is not taken from the command line, where the shell's history, the process list and sudo's log keep it. Nothing was changed. "
            "Give the name alone - sudo hmailserver --set-setting %s - and the value on standard input: typed at the prompt, which does not echo it, or piped in as one line. "
            "If the value just given was the real one, treat it as disclosed. %s= with nothing after it clears the setting.\n",
            shown.c_str(), shown.c_str(), shown.c_str());
         return 2;
      }

      if (!fromInput && !CheckValue(name, value))
         return 2;

      // Reads the file, and refuses the two Windows-only backends by name before
      // a connection is tried.
      std::shared_ptr<HM::DatabaseSettings> withDatabase, serverOnly;
      if (!ReadDatabaseSettings(withDatabase, serverOnly))
         return 2;

      const HM::AnsiString iniFile(HM::IniFileSettings::GetInitializationFile());
      if (::access(iniFile.c_str(), W_OK) != 0)
      {
         // The name only. A value given on the command line is not repeated: it
         // may be one nobody wants printed again.
         std::fprintf(stderr, "%s is not writable by this user, and the server's copy of the setting is written there as well as into the database. "
            "Run this as root (sudo hmailserver --set-setting %s%s), as --set-admin-password is run: the file keeps its owner and mode.\n",
            iniFile.c_str(), shown.c_str(), fromInput ? "" : "=...");
         return 1;
      }

      // One plain connection first, as --upgrade-database makes it, to say why
      // the database cannot be reached and what version it is at. The server's
      // own OpenDatabase below reports a failure through ErrorManager into the
      // error log - which, with this running as root, would leave today's error
      // log owned by root and unwritable by the service. A wrong password in
      // [Database] is the ordinary failure here, and it is answered on standard
      // error before anything can touch the log directory.
      {
         std::shared_ptr<HM::DALConnection> probe = Connect(withDatabase, "the database");
         if (!probe)
         {
            std::fprintf(stderr, "Nothing was changed.\n");
            return 1;
         }

         const int version = ReadSchemaVersion(probe);
         if (version == 0)
         {
            std::fprintf(stderr, "The database has no hm_dbversion row, so it is not an hMailServer database or was never created. --create-database makes one; nothing was changed.\n");
            return 1;
         }
         if (version < REQUIRED_DB_VERSION)
         {
            std::fprintf(stderr, "The database is at schema version %d and this build needs %d. Run --upgrade-database first; nothing was changed.\n",
               version, REQUIRED_DB_VERSION);
            return 1;
         }
         if (version > REQUIRED_DB_VERSION)
         {
            std::fprintf(stderr, "The database is at schema version %d and this build needs %d: it was created by a NEWER hMailServer, and this build will not write to it. Nothing was changed.\n",
               version, REQUIRED_DB_VERSION);
            return 1;
         }
      }

      // The value from standard input, now that the file and the database have
      // been found ready for it.
      if (fromInput)
      {
         std::string line;
         const bool got = ReadValueFromInput(shown, secret, line);
         value = HM::String(line.c_str());
         value.Trim();

         // Overwrite this process's narrow copy, as --set-admin-password does.
         std::fill(line.begin(), line.end(), '\0');

         if (!got || value.IsEmpty())
         {
            std::fprintf(stderr, "No value was read from standard input, so nothing was changed. An empty value is stored by giving %s= on the command line.\n",
               shown.c_str());
            return 2;
         }

         if (!CheckValue(name, value))
            return 2;
      }

      // The store writes through the connection manager the server runs on, so
      // the database is opened the way the server opens it.
      HM::String error;
      if (!HM::Application::Instance()->OpenDatabase(error))
      {
         std::fprintf(stderr, "Could not open the database: %s\nNothing was changed.\n", HM::AnsiString(error).c_str());
         return 1;
      }

      int result = 1;
      {
         // What the store held, for the line printed below. A key kept in the
         // file only - the password pepper - has no row on purpose and is written
         // to the file alone; WriteSetting knows which is which.
         const bool fileOnly = HM::IniSettingStore::IsFileOnlyName(name);
         std::map<HM::String, std::pair<HM::String, HM::String> > rows;
         HM::IniSettingStore::ReadAllRows(rows);
         const bool had = rows.find(name) != rows.end();
         const HM::String previousValue(had ? rows[name].first : HM::String());

         // The pepper as the file holds it before the write, for the warning
         // below. A file-only key is never in the store or in the overlay, so this
         // reads the file's line.
         const HM::String previousPepper(fileOnly ? HM::IniFileSettings::Instance()->GetSettingsValue(name) : HM::String());

         if (HM::IniFileSettings::Instance()->WriteSettingsValue(name, value))
         {
            // A secret is not echoed: not the value stored, not the one it
            // replaced, and not a value [SettingsOverride] forces over it.
            const HM::AnsiString stored(secret && !value.IsEmpty() ? HM::String(_T("(set)")) : value);
            const HM::AnsiString previous(secret && !previousValue.IsEmpty() ? HM::String(_T("(set)")) : previousValue);

            if (fileOnly)
            {
               std::printf("%s=%s written to %s. This key is kept in the file only, never in the database and never in a backup archive, and applies when the server next starts or is reloaded.\n",
                  shown.c_str(), stored.c_str(), iniFile.c_str());

               // The pepper is the one setting whose change locks accounts out.
               // Crypt::ApplyPepper_ mixes it into every Argon2id and scrypt hash
               // as the hash is made and as it is checked, and a check does not
               // retry without it.
               if (previousPepper.Compare(value) != 0)
               {
                  const char *change = previousPepper.IsEmpty() ? "The pepper was empty and is now set"
                                     : value.IsEmpty() ? "The pepper was set and is now cleared"
                                     : "The pepper is now a different value from the one the file held";
                  std::fprintf(stderr, "Warning: %s. Every Argon2id and scrypt password hash is made under the pepper in force when its password was set, and verifies under no other: once the server runs with this value, an account whose password is one of those hashes will not sign in until its password is set again. "
                     "PBKDF2, SHA-256 and MD5 hashes, and the administrator password, do not use the pepper. Keep a copy of the value apart from the backups, which do not carry it.\n",
                     change);
               }
            }
            else if (had)
            {
               std::printf("%s=%s stored in the database (hm_inisettings), where it was %s, and written to %s. It applies when the server next starts or is reloaded.\n",
                  shown.c_str(), stored.c_str(), previous.IsEmpty() ? "empty" : previous.c_str(), iniFile.c_str());
            }
            else
            {
               std::printf("%s=%s stored in the database (hm_inisettings) as a new row and written to %s. It applies when the server next starts or is reloaded.\n",
                  shown.c_str(), stored.c_str(), iniFile.c_str());
            }

            // The one thing that would make the line above untrue, said now
            // rather than found in the error log at the next start.
            std::map<HM::String, HM::String> overrides;
            HM::IniSettingStore::ReadOverrides(overrides);
            std::map<HM::String, HM::String>::const_iterator forced = overrides.find(name);
            if (forced != overrides.end())
            {
               const HM::AnsiString forcedShown(secret && !forced->second.IsEmpty() ? HM::String(_T("(set)")) : forced->second);
               std::printf("Note: [SettingsOverride] in %s forces %s=%s, which is applied over the stored value at every start (HM5807). Remove it from that section to let the value stored now take effect.\n",
                  iniFile.c_str(), shown.c_str(), forcedShown.c_str());
            }

            // The other thing that would: a key the environment sets is written back
            // by the next --apply-environment, which the container runs at every start
            // (docs/EnvironmentConfiguration.md). The REST API and COM refuse such a
            // change; this is the operator's own command line, so it is stored and
            // said.
            HM::EnvironmentSettings::Entry environment;
            if (HM::EnvironmentSettings::IsManagedInDatabase(HM::EnvironmentSettings::StoreIni, name, environment))
            {
               std::printf("Note: %s sets %s from this server's environment, and hmailserver --apply-environment writes it back at every start; the value stored now lasts until the next one. Change the variable instead.\n",
                  HM::AnsiString(environment.variable).c_str(), shown.c_str());
            }

            result = 0;
         }
         else
         {
            std::fprintf(stderr, "The setting could not be stored: the database could not be written. Nothing has been changed.\n");
         }
      }

      HM::Application::Instance()->CloseDatabase();
      return result;
   }
}

namespace
{
   // --sample-domain create|remove: a throwaway example.test, made and removed
   // from the command line. The one place this platform has no other door to it:
   // the REST API answers the same two calls, but the listener may not be on yet
   // and this is the row's "one command".
   //
   // Unlike --set-setting, which needs the settings store and nothing else, this
   // needs the whole configuration - the rule machinery, the caches and the
   // recipient parser that expands the distribution list - so it starts the
   // application the way the service starts it and stops short of opening a
   // listener. Nothing is accepted and nothing is delivered by this process.
   //
   // The messages go into the delivery queue. A server running beside this picks
   // them up on its next pass, which is within the minute; a stopped one delivers
   // them when it starts. That is said below rather than left to be discovered.
   //
   // InitInstance is the whole of start-up short of the listeners, so it also
   // runs AutoBanFirewall::Synchronise. That does nothing at all unless
   // AutoBanFirewallEnabled is on, which it is not on a stock server; where it IS
   // on, this re-synchronises the firewall as a start would. It is idempotent -
   // the rules it writes are the rules it would have written anyway - but a
   // command that says "sample domain" and touches nft is worth writing down.
   int SampleDomainCommand(const std::string &verb)
   {
      const bool creating = verb == "create";
      if (!creating && verb != "remove")
      {
         std::fprintf(stderr, "--sample-domain takes create or remove; for example --sample-domain create.\n");
         return 2;
      }

      HM::String error;
      if (!HM::Application::Instance()->InitInstance(error))
      {
         std::fprintf(stderr, "hMailServer could not read its configuration and open its database: %s\nNothing was changed.\n",
            HM::AnsiString(error).c_str());
         return 1;
      }

      HM::AnsiString report;
      HM::String sentence;
      const HM::SampleDomain::Outcome outcome = creating
         ? HM::SampleDomain::Create(report, sentence)
         : HM::SampleDomain::Remove(report, sentence);

      int result = 1;
      if (outcome == HM::SampleDomain::OutcomeDone)
      {
         std::printf("%s\n", HM::AnsiString(sentence).c_str());

         if (creating)
         {
            // The password is in the sentence printed above, and that is the one
            // time anything says it: it is minted per creation and nothing keeps
            // the clear text, so it cannot be looked up afterwards.
            std::printf("The messages are in the delivery queue: a running server delivers them within a minute, and a stopped one when it starts.\n");
            std::printf("Remove the sample - hmailserver --sample-domain remove - before this server takes real mail.\n");
         }

         result = 0;
      }
      else
      {
         std::fprintf(stderr, "%s\n", HM::AnsiString(sentence).c_str());
         result = outcome == HM::SampleDomain::OutcomeRefused ? 2 : 1;
      }

      HM::Application::Instance()->ExitInstance();
      return result;
   }
}

namespace
{
   // --verify-update: the live update's verification, on its own, for the
   // helper that applies the update as root. hmailserver-update has already
   // been handed a package the SERVER verified; this is root's own check of the
   // same file before root installs it, so that a service that has been taken
   // over cannot have root install something the release workflow never signed.
   // No database, no listener: the configuration file (for a trust root of the
   // administrator's own, if one is set) and the file. Exit 0 when the file is
   // the release's, 1 when it is not, 2 when the question could not be asked.
   int VerifyUpdate(const std::string &file)
   {
      HM::IniFileSettings::Instance()->LoadSettings();

      const HM::String path(file.c_str());
      const HM::String bundlePath = path + _T(".cosign.bundle");
      if (!HM::FileUtilities::Exists(path))
      {
         std::fprintf(stderr, "%s does not exist.\n", file.c_str());
         return 2;
      }
      if (!HM::FileUtilities::Exists(bundlePath))
      {
         std::fprintf(stderr, "%s has no Sigstore bundle beside it (%s.cosign.bundle), so it cannot be verified.\n", file.c_str(), file.c_str());
         return 1;
      }

      HM::SigstoreTrust trust;
      HM::String error;
      if (!HM::SigstoreVerifier::ConfiguredTrust(trust, error))
      {
         std::fprintf(stderr, "The trust could not be read: %s\n", HM::AnsiString(error).c_str());
         return 2;
      }

      HM::AnsiString bundleJson;
      if (!HM::SigstoreVerifier::ReadBundle(bundlePath, bundleJson))
      {
         std::fprintf(stderr, "%s.cosign.bundle could not be read.\n", file.c_str());
         return 2;
      }
      HM::SigstoreVerdict verdict;
      if (!HM::SigstoreVerifier::VerifyFile(path, bundleJson, trust, verdict))
      {
         std::fprintf(stderr, "%s did not verify against its Sigstore bundle: %s\n", file.c_str(), HM::AnsiString(verdict.error).c_str());
         return 1;
      }

      std::printf("%s verifies: signed by %s, recorded by the transparency log at %s.\n",
         file.c_str(), verdict.identity.c_str(), HM::AnsiString(HM::UpdateChecker::FormatUnixTime(verdict.integrated_time)).c_str());
      return 0;
   }
}

int main(int argc, char *argv[])
{
   std::string configuration;
   std::string set_setting;
   std::string sample_domain;
   std::string verify_update;
   bool check_only = false;
   bool set_password = false;
   bool create_database = false;
   bool upgrade_database = false;
   bool apply_environment = false;
   std::string apply_profile;
   bool profile_confirm = false;
   bool profile_once = false;

   for (int index = 1; index < argc; index++)
   {
      const std::string argument = argv[index];
      if (argument == "--help" || argument == "-h")
      {
         PrintUsage(argv[0]);
         return 0;
      }
      if (argument == "--version")
      {
         std::printf("hMailServer %s build %d\n", HMAILSERVER_VERSION, HMAILSERVER_BUILD);
         return 0;
      }
      if (argument == "--foreground")
         continue;
      if (argument == "--check-config")
      {
         check_only = true;
         continue;
      }
      if (argument == "--set-admin-password")
      {
         set_password = true;
         continue;
      }
      if (argument == "--create-database")
      {
         create_database = true;
         continue;
      }
      if (argument == "--upgrade-database")
      {
         upgrade_database = true;
         continue;
      }
      if (argument == "--apply-environment")
      {
         apply_environment = true;
         continue;
      }
      if (argument == "--apply-profile")
      {
         if (index + 1 >= argc)
         {
            std::fprintf(stderr, "--apply-profile needs the profile's name: office, gateway, relay or sender.\n");
            return 2;
         }
         apply_profile = argv[++index];
         continue;
      }
      if (argument == "--confirm")
      {
         profile_confirm = true;
         continue;
      }
      if (argument == "--once")
      {
         profile_once = true;
         continue;
      }
      if (argument == "--set-setting")
      {
         if (index + 1 >= argc)
         {
            std::fprintf(stderr, "--set-setting needs NAME=VALUE, or NAME with the value on standard input; for example --set-setting RestApiPort=8080.\n");
            return 2;
         }
         set_setting = argv[++index];
         continue;
      }
      if (argument == "--sample-domain")
      {
         if (index + 1 >= argc)
         {
            std::fprintf(stderr, "--sample-domain needs create or remove; for example --sample-domain create.\n");
            return 2;
         }
         sample_domain = argv[++index];
         continue;
      }
      if (argument == "--config")
      {
         if (index + 1 >= argc)
         {
            std::fprintf(stderr, "--config needs a file name.\n");
            return 2;
         }
         configuration = argv[++index];
         continue;
      }
      if (argument == "--verify-update")
      {
         if (index + 1 >= argc)
         {
            std::fprintf(stderr, "--verify-update needs the file to check; its bundle is <file>.cosign.bundle beside it.\n");
            return 2;
         }
         verify_update = argv[++index];
         continue;
      }
      std::fprintf(stderr, "Unknown option: %s\nTry --help.\n", argument.c_str());
      return 2;
   }

   InstallHandlers();

   if (!configuration.empty())
      HM::IniFileSettings::Instance()->SetInitializationFile(HM::String(configuration.c_str()));

   HM::String error;

   if (set_password)
   {
      // The administrator password is the credential the REST API and the
      // Control Panel authenticate against, and until this existed there was no
      // way to set it on a machine with no COM: a packaged installation would
      // have had to carry one in plain text, which is exactly what the packaged
      // configuration's own comment tells an administrator not to do. The hash
      // written here is the same PBKDF2 the Control Panel writes, produced by
      // the same code.
      //
      // The password is read from standard input rather than from an argument,
      // so it is not in the process list or in the shell history, and a
      // terminal's echo is turned off while it is typed.
      HM::IniFileSettings::Instance()->LoadSettings();

      const bool interactive = ::isatty(STDIN_FILENO) != 0;
      struct termios original;
      bool echo_disabled = false;
      if (interactive)
      {
         std::printf("New administrator password: ");
         std::fflush(stdout);
         if (::tcgetattr(STDIN_FILENO, &original) == 0)
         {
            struct termios quiet = original;
            quiet.c_lflag &= ~(tcflag_t) ECHO;
            echo_disabled = ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet) == 0;
         }
      }

      std::string password;
      std::getline(std::cin, password);

      if (echo_disabled)
         ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
      if (interactive)
         std::printf("\n");

      while (!password.empty() && (password.back() == '\r' || password.back() == '\n'))
         password.pop_back();

      if (password.empty())
      {
         std::fprintf(stderr, "No password was given; nothing was changed.\n");
         return 2;
      }

      HM::IniFileSettings::Instance()->SetAdministratorPassword(HM::String(password.c_str()));

      // Overwrite this process's copy. It costs nothing, and the alternative is
      // a password sitting in a heap block until the process exits.
      std::fill(password.begin(), password.end(), '\0');

      std::printf("The administrator password was written to %s as a PBKDF2 hash.\n",
         HM::AnsiString(HM::IniFileSettings::Instance()->GetInitializationFile()).c_str());
      return 0;
   }

   if (!verify_update.empty())
      return VerifyUpdate(verify_update);

   if (create_database)
      return CreateDatabase();

   if (upgrade_database)
      return UpgradeDatabase();

   if (!set_setting.empty())
      return SetSetting(set_setting);

   // Every setting from the environment (Common/Application/EnvironmentSettings,
   // docs/EnvironmentConfiguration.md): what the container's entrypoint runs after
   // the create and upgrade steps, at every start.
   if (apply_environment)
      return HM::EnvironmentSettings::RunCommand();

   // A setting profile (Common/Application/Profiles, docs/Profiles.md): what the
   // container's entrypoint runs with --once when HM_PROFILE names one, after the
   // environment's own settings are stored, so that a setting the environment
   // manages is skipped with the variable named.
   if (!apply_profile.empty())
      return HM::Profiles::RunCommand(HM::String(apply_profile.c_str()), profile_confirm, profile_once);

   if (!sample_domain.empty())
      return SampleDomainCommand(sample_domain);

   if (check_only)
   {
      // Reads the file and reports what it found, without opening the database
      // or a listener. This is what a package's post-install step runs, and what
      // an administrator runs after editing the file, so it must never be the
      // thing that starts a half-configured server.
      HM::IniFileSettings::Instance()->LoadSettings();
      std::printf("Configuration file: %s\n",
         HM::AnsiString(HM::IniFileSettings::Instance()->GetInitializationFile()).c_str());
      std::printf("Program directory:  %s\n",
         HM::AnsiString(HM::IniFileSettings::Instance()->GetProgramDirectory()).c_str());
      std::printf("Data directory:     %s\n",
         HM::AnsiString(HM::IniFileSettings::Instance()->GetDataDirectory()).c_str());
      std::printf("Log directory:      %s\n",
         HM::AnsiString(HM::IniFileSettings::Instance()->GetLogDirectory()).c_str());
      std::printf("Database type:      %d\n",
         (int) HM::IniFileSettings::Instance()->GetDatabaseType());
      return 0;
   }

   // The crash oracle first, before the application touches the heap in anger:
   // a fault during start-up is exactly the kind that otherwise disappears. It
   // installs signal handlers for the memory-safety signals and SIGABRT, and
   // says where its records go once the log is up (below).
   HM::CrashOracle::Install();

   if (!HM::Application::Instance()->InitInstance(error))
   {
      // Nothing is listening and nothing has been accepted, so this is the one
      // place a failure can be reported plainly and exited on. It goes to
      // standard error as well as the log because at this point the log
      // directory may be exactly what is wrong.
      std::fprintf(stderr, "hMailServer could not start: %s\n", HM::AnsiString(error).c_str());
      return 1;
   }

   if (!HM::Application::Instance()->StartServers())
   {
      std::fprintf(stderr, "hMailServer started but could not open its listeners; see the error log.\n");
      HM::Application::Instance()->ExitInstance();
      return 1;
   }

   HM::CrashOracle::LogInstallationStatus();
   HM::Logger::Instance()->LogApplication(HM::String(_T("hMailServer ")) + HMAILSERVER_VERSION + _T(" is running. Send SIGTERM to stop it."));

   // The wait loop. The application's work happens on its own threads; this
   // thread exists to hold the process open and to notice a signal. A second of
   // latency on a stop is not worth a self-pipe here: systemd's default
   // TimeoutStopSec is 90 seconds and the drain below is what actually takes the
   // time.
   while (!stop_requested)
   {
      if (reload_requested)
      {
         reload_requested = 0;
         HM::Logger::Instance()->LogApplication(_T("SIGHUP: re-reading the configuration."));
         const HM::String result = HM::Application::Instance()->Reinitialize();
         if (!result.IsEmpty())
            HM::Logger::Instance()->LogApplication(HM::String(_T("SIGHUP: the configuration was not re-read: ")) + result);
      }

      struct timespec pause;
      pause.tv_sec = 1;
      pause.tv_nsec = 0;
      nanosleep(&pause, nullptr);
   }

   HM::Logger::Instance()->LogApplication(_T("Stopping: a signal was received."));

   // The same order the Windows service uses, and for the same reason: stop
   // accepting first so nothing new arrives, then let the application finish what
   // it holds. ShutdownDrainSeconds decides how long that is allowed to take.
   // A fault from here on is recorded as one during shutdown, which is a
   // different finding from one in service.
   HM::CrashOracle::NotifyShutdownStarted();
   HM::Application::Instance()->StopServers();
   HM::Application::Instance()->ExitInstance();

   HM::Logger::Instance()->LogApplication(_T("Stopped."));
   return 0;
}
