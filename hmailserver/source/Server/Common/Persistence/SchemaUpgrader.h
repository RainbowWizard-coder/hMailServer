// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <functional>
#include <vector>

#include "../SQL/DatabaseSettings.h"

namespace HM
{
   class DALConnection;

   // The upgrade chain, walked in the server's own process.
   //
   // One unit for the three callers that bring a database forward without
   // DBUpdater: `hmailserver --upgrade-database` on Linux (platform/main.cpp,
   // which used to carry this walk itself) and on Windows (hMailServer.cpp),
   // and Application::OnDatabaseConnected when [Database] UpgradeOnStart=1
   // asks the service to do it at its own start. What it does is what
   // DBUpdater does after the Windows installer, in the same order and with the
   // same code underneath: the backend's ScriptPrerequisites script first where
   // one ships (SQL Server and PostgreSQL define hm_drop_column_objects there,
   // and two steps of each chain call it), then for every step from the
   // database's version to this build's - found by scanning DBScripts for
   // Upgrade<from>to<n><suffix>.sql, because the numbers are not consecutive -
   // PrerequisiteList::Ensure for the step's target and SQLScriptRunner over the
   // script (a prerequisite that rewrites a large table's rows also runs once
   // before the step's transaction, committed as it goes - see
   // IPrerequisite::RunsBeforeTransaction), the same parser, the same macros
   // and the same four dialects
   // (MySQL, PGSQL, MSSQL and MSSQLCE) the COM Database.ExecuteSQLScript runs.
   // Each step is a transaction where the backend has one: a statement that
   // fails leaves the version where the previous step put it, and the outcome
   // names the step. What is NOT here is DBUpdater's schema verification
   // (SchemaVerification.cs, the probes): that is C#, and the installer's path
   // keeps it. A backup is nobody's here either; Upgrading.md says to take one.
   class SchemaUpgrader
   {
   public:
      struct Outcome
      {
         Outcome() : ok(false), version(0), steps(0) {}

         bool ok;
         // Where the database is on return - the target when ok, the last step
         // that completed when not.
         int version;
         int steps;
         // "6086 -> 6087 (Upgrade6086to6087MSSQLCE.sql)" when a step failed, or
         // what stopped the walk before a step could ("no upgrade script leads
         // from 6086"); empty when ok.
         String failed_step;
         String error;
      };

      typedef std::function<void(const String &line)> Report;

      // The suffix the scripts carry for a backend: Upgrade6086to6087<suffix>.sql.
      static String ScriptSuffix(DatabaseSettings::SQLDBType type);

      // ScriptPrerequisites<suffix>.sql, for the two backends that ship one;
      // empty for the others.
      static String PrerequisitesScript(DatabaseSettings::SQLDBType type);

      // The next script in the chain: Upgrade<from>to<something><suffix>.sql in
      // the directory. False when none leads from `from`.
      static bool FindUpgradeScript(const String &directory, int from, const String &suffix, String &fileName, int &to);

      // hm_dbversion's one row, or 0 when there is none (not an hMailServer
      // database, or not yet one).
      static int ReadSchemaVersion(std::shared_ptr<DALConnection> connection);

      // Walks the chain on the connection from the database's version to
      // `required`, reporting one line per step through `report`, and stops at
      // the first failure. A database already at `required` is untouched and ok
      // with zero steps; one past it is refused (a newer build made it).
      static Outcome Upgrade(std::shared_ptr<DALConnection> connection, const String &scriptDirectory, int required, Report report);

      // The connection [Database] in hMailServer.ini names, built exactly as
      // DatabaseConnectionManager builds the pool's, for the command-line
      // callers that have no pool.
      static std::shared_ptr<DatabaseSettings> SettingsFromIni();

      // A line for the application log from an upgrade that runs before the
      // server has read its settings: the service's own start with UpgradeOnStart
      // (Application::OnDatabaseConnected), and a prerequisite DBUpdater runs over
      // COM against a service that refused the database's version. Until
      // Configuration::Load the logger's mask is 0 and LOG_APPLICATION drops
      // every line, so such a line is kept here (a thousand at most) and written
      // by LogNotes, which Application::InitInstance calls once the settings are
      // loaded - for DBUpdater, at the reinitialise it asks for when the upgrade
      // is committed. Where the application log is already on, the line is
      // written at once. Lines kept while application logging stays off are
      // dropped at LogNotes, as any application-log line is.
      static void Note(const String &line);
      static void LogNotes();

      // Note's halves, apart for the tester: whether a line is kept for later
      // under a logger mask (Logger::GetLogMask), keeping one, and the lines kept
      // - handed over in the order they came and forgotten.
      static bool KeepsForLater(int logMask);
      static void Keep(const String &line);
      static std::vector<String> TakeNotes();
      static const size_t MaxNotes = 1000;
   };

   // Holds the walk to examples in ClassTester::DoTests, on a SQL Server
   // Compact database of its own with a chain of its own: see the definition.
   class SchemaUpgraderTester
   {
   public:
      void Test();
   };
}
