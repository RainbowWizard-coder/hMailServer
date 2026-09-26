// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "SchemaUpgrader.h"

#include <stdexcept>

#include "../Application/IniFileSettings.h"
#include "../SQL/DALConnection.h"
#include "../SQL/DALConnectionFactory.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLScriptRunner.h"
#include "../SQL/Prerequisites/PrerequisiteList.h"
#include "../Util/FileInfo.h"
#include "../Util/FileUtilities.h"
#include "../Util/GUIDCreator.h"
#include "../Util/PasswordGenerator.h"

#ifndef HM_PLATFORM_POSIX
#include "../SQL/SQLCEConnection.h"
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      /*
         MySQL's named lock, which two walkers of one MySQL or MariaDB database
         take for each step. The row lock the other backends serialise on does
         not hold there: every MySQL step opens with DDL, and a CREATE or ALTER
         TABLE commits the transaction and frees the FOR UPDATE lock with it, so
         a second walker blocked on the version row went on while the first was
         still running the step, read the version the step had not yet written,
         and ran the same script beside it - to fail on a table or a column
         already there, or to interleave its rows with the first's. GET_LOCK
         belongs to the session, not the transaction, so no commit releases it;
         the destructor does, at the end of the step, and a walker that dies
         releases it with its connection.

         Four hours for the other walker's step: 6101 to 6102 rebuilds every
         table, which on a large database is minutes or more. The wait is not a
         statement timeout, which is lifted for the call - on MariaDB
         max_statement_time would end it, and on MySQL max_execution_time ends a
         SELECT.
      */
      class NamedUpgradeLock
      {
      public:
         explicit NamedUpgradeLock(std::shared_ptr<DALConnection> connection) :
            connection_(connection),
            held_(false)
         {
         }

         ~NamedUpgradeLock()
         {
            try
            {
               if (held_)
               {
                  String error;
                  connection_->TryExecute(SQLCommand(_T("do release_lock('hmailserver_schema_upgrade')")), error, 0, 0);
               }
            }
            catch (...)
            {
               // Never out of a destructor. The lock goes with the session.
            }
         }

         /*
            False, with the reason, when the step must not run: the other
            walker still holds the lock after four hours, or the server
            answered the call with NULL. When the server refuses the call
            itself - MariaDB Galera answers GET_LOCK with error 1235 while
            wsrep is on - the step runs without it, serialised by the version
            row alone as before, and unavailable says so: a server that has
            no named lock is no reason to leave its database un-upgraded.
         */
         bool Acquire(String &error, bool &unavailable)
         {
            unavailable = false;

            connection_->SetTimeout(0);

            std::shared_ptr<DALRecordset> recordset = connection_->CreateRecordset();
            const bool opened = recordset &&
               recordset->Open(connection_, SQLCommand(_T("select get_lock('hmailserver_schema_upgrade', 14400) as locked")));

            connection_->ApplyDefaultTimeout();

            if (!opened)
            {
               unavailable = true;
               return true;
            }

            if (recordset->IsEOF() || recordset->GetIsNull("locked"))
            {
               error = _T("The schema upgrade's lock (GET_LOCK('hmailserver_schema_upgrade')) could not be taken.");
               return false;
            }

            if (recordset->GetLongValue("locked") != 1)
            {
               error = _T("Another process has held the schema upgrade's lock (GET_LOCK('hmailserver_schema_upgrade')) for four hours while it runs a step. Run the upgrade again when it has finished.");
               return false;
            }

            held_ = true;
            return true;
         }

      private:
         std::shared_ptr<DALConnection> connection_;
         bool held_;
      };
   }

   String
   SchemaUpgrader::ScriptSuffix(DatabaseSettings::SQLDBType type)
   {
      // The same spellings DBUpdater's formMain uses to build the file name.
      switch (type)
      {
      case DatabaseSettings::TypeMYSQLServer:         return _T("MySQL");
      case DatabaseSettings::TypePGServer:            return _T("PGSQL");
      case DatabaseSettings::TypeMSSQLServer:         return _T("MSSQL");
      case DatabaseSettings::TypeMSSQLCompactEdition: return _T("MSSQLCE");
      default:                                        return _T("");
      }
   }

   String
   SchemaUpgrader::PrerequisitesScript(DatabaseSettings::SQLDBType type)
   {
      switch (type)
      {
      case DatabaseSettings::TypeMSSQLServer: return _T("ScriptPrerequisitesMSSQL.sql");
      case DatabaseSettings::TypePGServer:    return _T("ScriptPrerequisitesPGSQL.sql");
      default:                                return _T("");
      }
   }

   bool
   SchemaUpgrader::FindUpgradeScript(const String &directory, int from, const String &suffix, String &fileName, int &to)
   {
      fileName.Empty();
      to = 0;

      // The directory is scanned rather than the number guessed, because the
      // steps are not consecutive - the chain goes 5001, 5002, ... and jumps
      // where a release skipped a number - and DBUpdater builds its path the
      // same way. An exact match on the whole name, so that Upgrade5001to5002
      // MSSQL.sql is not taken for the MSSQLCE chain.
      const String prefix = Formatter::Format(_T("Upgrade{0}to"), from);
      const String pattern = prefix + _T("[0-9]+") + suffix + _T("\\.sql");

      std::vector<FileInfo> files = FileUtilities::GetFilesInDirectory(directory, pattern);
      if (files.empty())
         return false;

      String name = files[0].GetName();
      String middle = name.Mid(prefix.GetLength());
      middle = middle.Left(middle.GetLength() - suffix.GetLength() - 4);
      to = _ttoi(middle.c_str());
      if (to <= 0)
         return false;

      fileName = name;
      return true;
   }

   int
   SchemaUpgrader::ReadSchemaVersion(std::shared_ptr<DALConnection> connection)
   {
      std::shared_ptr<DALRecordset> recordset = connection->CreateRecordset();
      if (!recordset || !recordset->Open(connection, SQLCommand("select * from hm_dbversion")))
         return 0;
      if (recordset->IsEOF())
         return 0;
      return (int) recordset->GetLongValue("value");
   }

   SchemaUpgrader::Outcome
   SchemaUpgrader::Upgrade(std::shared_ptr<DALConnection> connection, const String &scriptDirectory, int required, Report report)
   {
      Outcome outcome;

      int current = ReadSchemaVersion(connection);
      outcome.version = current;

      if (current == 0)
      {
         outcome.failed_step = _T("reading hm_dbversion");
         outcome.error = _T("The database has no hm_dbversion row, so it is not an hMailServer database or was never created.");
         return outcome;
      }

      if (current == required)
      {
         outcome.ok = true;
         return outcome;
      }

      if (current > required)
      {
         outcome.failed_step = _T("comparing versions");
         outcome.error = Formatter::Format(_T("The database is at schema version {0} and this build needs {1}: it was created by a NEWER hMailServer. This build will not run against it."), current, required);
         return outcome;
      }

      const DatabaseSettings::SQLDBType type = connection->GetSettings()->GetType();
      const String suffix = ScriptSuffix(type);
      SQLScriptRunner runner;

      // The stored procedure two of the SQL Server steps and two of the
      // PostgreSQL steps call, made before the chain as DBUpdater makes it.
      // Create-or-replace on both, so running it again changes nothing.
      const String prerequisites = PrerequisitesScript(type);
      if (!prerequisites.IsEmpty())
      {
         const String path = scriptDirectory + FileUtilities::PathSeparator + prerequisites;
         if (FileUtilities::Exists(path))
         {
            report(Formatter::Format(_T("prerequisites: {0}"), path));

            String error;
            if (!runner.ExecuteScript(connection, path, error))
            {
               outcome.failed_step = Formatter::Format(_T("the prerequisites script ({0})"), prerequisites);
               outcome.error = error;
               return outcome;
            }
         }
      }

      while (current < required)
      {
         String fileName;
         int next = 0;
         if (!FindUpgradeScript(scriptDirectory, current, suffix, fileName, next))
         {
            outcome.failed_step = Formatter::Format(_T("{0} -> ?"), current);
            outcome.error = Formatter::Format(_T("No upgrade script leads from schema version {0} in {1}. The chain is broken, or ProgramFolder does not point at the DBScripts this build shipped with."),
               current, scriptDirectory);
            return outcome;
         }

         const String path = scriptDirectory + FileUtilities::PathSeparator + fileName;
         const String step = Formatter::Format(_T("{0} -> {1} ({2})"), current, next, fileName);
         report(step);

         // On MySQL the step is serialised on a named lock, taken before the
         // transaction and released when the step is over (NamedUpgradeLock).
         String error;
         NamedUpgradeLock stepLock(connection);
         if (type == DatabaseSettings::TypeMYSQLServer)
         {
            bool unavailable = false;
            if (!stepLock.Acquire(error, unavailable))
            {
               outcome.failed_step = step;
               outcome.error = error;
               return outcome;
            }

            if (unavailable)
               report(Formatter::Format(_T("{0}: the database server refused GET_LOCK, so this step is not serialised against another process upgrading the same database. Upgrade from one process at a time."), step));
         }

         // One transaction per step, where the backend has one: a statement
         // that fails takes the step's earlier statements with it on SQL Server,
         // PostgreSQL and Compact, and the version row - the script's last
         // statement - is never written for a step that did not finish. MySQL
         // commits DDL as it goes and the transaction protects only the rows;
         // that is the same protection DBUpdater's one transaction gives it.
         if (!connection->BeginTransaction(error))
         {
            outcome.failed_step = step;
            outcome.error = Formatter::Format(_T("A transaction could not be started: {0}"), error);
            return outcome;
         }

         // Two walkers on one database - two nodes of a warm-standby pair both
         // starting with UpgradeOnStart, or a node and the command line - are
         // serialised on the version row: the second blocks on the lock until
         // the first commits, and then finds the step done and moves on. On
         // SQL Server and PostgreSQL the read is locked for the length of the
         // transaction; Compact is one file with one writer and has nothing to
         // lock against. On MySQL the row lock lasts only until the script's
         // first DDL statement commits the transaction, so there the named lock
         // above is what serialises the step, and this read, made under it, is
         // what finds the step already done.
         String lockStatement;
         switch (type)
         {
         case DatabaseSettings::TypePGServer:
         case DatabaseSettings::TypeMYSQLServer:
            lockStatement = _T("select value from hm_dbversion for update");
            break;
         case DatabaseSettings::TypeMSSQLServer:
            lockStatement = _T("select value from hm_dbversion with (updlock, holdlock)");
            break;
         default:
            break;
         }

         if (!lockStatement.IsEmpty())
         {
            std::shared_ptr<DALRecordset> locked = connection->CreateRecordset();
            if (!locked || !locked->Open(connection, SQLCommand(lockStatement)))
            {
               String rollbackError;
               connection->RollbackTransaction(rollbackError);

               outcome.failed_step = step;
               outcome.error = _T("The version row could not be locked for the step.");
               outcome.version = ReadSchemaVersion(connection);
               return outcome;
            }

            const int lockedVersion = locked->IsEOF() ? 0 : (int) locked->GetLongValue("value");
            if (lockedVersion != current)
            {
               // Another walker did this step while this one waited for the lock.
               String rollbackError;
               connection->RollbackTransaction(rollbackError);

               report(Formatter::Format(_T("{0} -> {1}: already applied by another process; the database is at {2}."), current, next, lockedVersion));

               if (lockedVersion <= current)
               {
                  outcome.failed_step = step;
                  outcome.error = Formatter::Format(_T("The version row read {0} under the lock, which is not past {1}. Stopping rather than looping."), lockedVersion, current);
                  outcome.version = lockedVersion;
                  return outcome;
               }

               current = lockedVersion;
               outcome.version = current;
               continue;
            }
         }

         PrerequisiteList prerequisiteList;
         bool applied = prerequisiteList.Ensure(connection, next, error) &&
                        runner.ExecuteScript(connection, path, error);

         if (!applied)
         {
            String rollbackError;
            connection->RollbackTransaction(rollbackError);

            outcome.failed_step = step;
            outcome.error = error;
            outcome.version = ReadSchemaVersion(connection);
            return outcome;
         }

         if (!connection->CommitTransaction(error))
         {
            outcome.failed_step = step;
            outcome.error = Formatter::Format(_T("The step ran but could not be committed: {0}"), error);
            outcome.version = ReadSchemaVersion(connection);
            return outcome;
         }

         const int after = ReadSchemaVersion(connection);
         if (after <= current)
         {
            outcome.failed_step = step;
            outcome.error = Formatter::Format(_T("The script ran but the version row still says {0}. Stopping rather than looping."), after);
            outcome.version = after;
            return outcome;
         }

         current = after;
         outcome.version = current;
         outcome.steps++;
      }

      outcome.ok = true;
      return outcome;
   }

   std::shared_ptr<DatabaseSettings>
   SchemaUpgrader::SettingsFromIni()
   {
      IniFileSettings *ini = IniFileSettings::Instance();

      return std::shared_ptr<DatabaseSettings>(new DatabaseSettings(
         ini->GetDatabaseProvider(), ini->GetDatabaseServer(), ini->GetDatabaseName(), ini->GetUsername(), ini->GetPassword(),
         ini->GetDatabaseDirectory(), ini->GetDatabaseServerFailoverPartner(), ini->GetDatabaseType(), ini->GetDatabasePort()));
   }

   // ---- the tester ----------------------------------------------------------

   namespace
   {
      void Fail_(const String &sentence)
      {
         throw std::logic_error(std::string(AnsiString(sentence).c_str()));
      }

      // The walk's script-finding half, on a directory of empty files with the
      // shapes the chain has and the shapes it must not take for a step.
      void TestFindingTheNextScript_(const String &directory)
      {
         const String names[] =
         {
            _T("Upgrade7001to7002MSSQLCE.sql"),
            _T("Upgrade7001to7002MSSQL.sql"),     // the other dialect's file for the same step
            _T("Upgrade7001to7005MySQL.sql"),
            _T("Upgrade7002to7010MSSQLCE.sql"),   // a jump, as the real chain has
            _T("Upgrade70010to70011MSSQLCE.sql"), // a prefix match that is not this step
            _T("Upgrade7001toXMSSQLCE.sql"),      // not a number
         };
         for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            FileUtilities::WriteToFile(directory + FileUtilities::PathSeparator + names[i], AnsiString("-- empty\n"));

         String file;
         int to = 0;
         if (!SchemaUpgrader::FindUpgradeScript(directory, 7001, _T("MSSQLCE"), file, to) || file != _T("Upgrade7001to7002MSSQLCE.sql") || to != 7002)
            Fail_(_T("FindUpgradeScript did not find the Compact step from 7001, or read its target wrongly."));
         if (!SchemaUpgrader::FindUpgradeScript(directory, 7001, _T("MSSQL"), file, to) || file != _T("Upgrade7001to7002MSSQL.sql"))
            Fail_(_T("FindUpgradeScript took another dialect's file, or missed the SQL Server one."));
         if (!SchemaUpgrader::FindUpgradeScript(directory, 7001, _T("MySQL"), file, to) || to != 7005)
            Fail_(_T("FindUpgradeScript did not read a target that is not the next number."));
         if (!SchemaUpgrader::FindUpgradeScript(directory, 7002, _T("MSSQLCE"), file, to) || to != 7010)
            Fail_(_T("FindUpgradeScript did not follow a jump in the chain."));
         if (SchemaUpgrader::FindUpgradeScript(directory, 7010, _T("MSSQLCE"), file, to))
            Fail_(_T("FindUpgradeScript found a step from 7010, which the directory does not have."));
         if (SchemaUpgrader::FindUpgradeScript(directory, 7001, _T("PGSQL"), file, to))
            Fail_(_T("FindUpgradeScript found a PostgreSQL step that is not there."));
         if (SchemaUpgrader::FindUpgradeScript(directory + _T("-absent"), 7001, _T("MSSQLCE"), file, to))
            Fail_(_T("FindUpgradeScript found a step in a directory that does not exist."));

         for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            FileUtilities::DeleteFile(directory + FileUtilities::PathSeparator + names[i]);
      }
   }

   void
   SchemaUpgraderTester::Test()
   {
      const String directory = IniFileSettings::Instance()->GetTempDirectory() + FileUtilities::PathSeparator + _T("SchemaUpgraderTest-") + GUIDCreator::GetGUID();
      if (!FileUtilities::CreateDirectory(directory))
         Fail_(_T("The tester's directory could not be created under the temp folder."));

      try
      {
         TestFindingTheNextScript_(directory);

#ifndef HM_PLATFORM_POSIX
         // The walk itself, on a SQL Server Compact database of the tester's own -
         // made the way the server makes the internal database, in this directory
         // and never the bench's - with a chain of the tester's own: three steps
         // that this build's DBScripts do not have, so nothing here depends on
         // the real chain and nothing here can touch a real database. The real
         // chain is proved by DBUpdater's probes and build\check-db-scripts.ps1.
         const String name = _T("SchemaUpgraderTest");
         const String password = PasswordGenerator::Generate();
         String error;
         if (!SQLCEConnection::CreateDatabase(directory, name, password, error))
            Fail_(_T("The tester's Compact database could not be created: ") + error);

         const String empty;
         std::shared_ptr<DatabaseSettings> settings(new DatabaseSettings(empty, empty, name, empty, password, directory, empty, DatabaseSettings::TypeMSSQLCompactEdition, 0));
         std::shared_ptr<DALConnection> connection = DALConnectionFactory::CreateConnection(settings);
         if (!connection || connection->Connect(error) != DALConnection::Connected)
            Fail_(_T("The tester's Compact database could not be connected to: ") + error);

         // A schema at 7001: the version row and one table.
         const char *creates[] =
         {
            "create table hm_dbversion (value int)",
            "insert into hm_dbversion (value) values (7001)",
            "create table hm_upgradertest (id int not null)",
         };
         for (size_t i = 0; i < sizeof(creates) / sizeof(creates[0]); i++)
         {
            if (!connection->Execute(SQLCommand(creates[i]), error))
               Fail_(_T("The tester's schema could not be created: ") + error);
         }

         // The chain: two good steps, in the Compact dialect's form - statements
         // separated by a blank line, the version row last - and one that fails
         // on its first statement.
         FileUtilities::WriteToFile(directory + FileUtilities::PathSeparator + _T("Upgrade7001to7002MSSQLCE.sql"),
            AnsiString("alter table hm_upgradertest add name nvarchar(20) null\r\n\r\nupdate hm_dbversion set value = 7002\r\n"));
         FileUtilities::WriteToFile(directory + FileUtilities::PathSeparator + _T("Upgrade7002to7003MSSQLCE.sql"),
            AnsiString("create table hm_upgradertest2 (id int not null)\r\n\r\nupdate hm_dbversion set value = 7003\r\n"));
         FileUtilities::WriteToFile(directory + FileUtilities::PathSeparator + _T("Upgrade7003to7004MSSQLCE.sql"),
            AnsiString("alter table hm_nosuchtable add broken int null\r\n\r\nupdate hm_dbversion set value = 7004\r\n"));

         std::vector<String> reported;
         SchemaUpgrader::Report report = [&reported](const String &line) { reported.push_back(line); };

         // A database at N-1 reaches N, one step at a time, one line per step.
         SchemaUpgrader::Outcome outcome = SchemaUpgrader::Upgrade(connection, directory, 7003, report);
         if (!outcome.ok || outcome.version != 7003 || outcome.steps != 2)
            Fail_(Formatter::Format(_T("A database at 7001 did not reach 7003 in two steps: ok {0}, version {1}, steps {2}, {3} {4}"), outcome.ok ? 1 : 0, outcome.version, outcome.steps, outcome.failed_step, outcome.error));
         if (SchemaUpgrader::ReadSchemaVersion(connection) != 7003)
            Fail_(_T("The version row does not say 7003 after the walk."));
         if (reported.size() != 2 || reported[0].Find(_T("7001 -> 7002")) < 0 || reported[1].Find(_T("7002 -> 7003")) < 0)
            Fail_(_T("The walk did not report one line per step naming the step."));
         if (!connection->Execute(SQLCommand("select name from hm_upgradertest"), error) || !connection->Execute(SQLCommand("select id from hm_upgradertest2"), error))
            Fail_(_T("The two steps' objects are not there after the walk: ") + error);

         // A database at N is untouched.
         reported.clear();
         outcome = SchemaUpgrader::Upgrade(connection, directory, 7003, report);
         if (!outcome.ok || outcome.steps != 0 || outcome.version != 7003 || !reported.empty())
            Fail_(_T("A database already at the required version was not left alone."));

         // A failing statement leaves the version at N-1 and names the step.
         reported.clear();
         outcome = SchemaUpgrader::Upgrade(connection, directory, 7004, report);
         if (outcome.ok)
            Fail_(_T("A step whose first statement fails was reported as applied."));
         if (outcome.failed_step.Find(_T("7003 -> 7004")) < 0 || outcome.failed_step.Find(_T("Upgrade7003to7004MSSQLCE.sql")) < 0)
            Fail_(_T("The failed step was not named: ") + outcome.failed_step);
         if (outcome.error.IsEmpty())
            Fail_(_T("The failed statement's error was not carried."));
         if (outcome.version != 7003 || SchemaUpgrader::ReadSchemaVersion(connection) != 7003)
            Fail_(Formatter::Format(_T("A failed step moved the version row: outcome {0}, row {1}."), outcome.version, SchemaUpgrader::ReadSchemaVersion(connection)));

         // A chain with no script from where the database is names that too.
         outcome = SchemaUpgrader::Upgrade(connection, directory, 7005, report);
         if (outcome.ok)
            Fail_(_T("A chain that cannot reach the required version was reported as applied."));
         // 7003 -> 7004 exists and fails first, so the missing-script case needs a
         // database past it: say so from a version nothing leads from.
         if (!connection->Execute(SQLCommand("update hm_dbversion set value = 7004"), error))
            Fail_(_T("The version row could not be moved for the missing-script case: ") + error);
         outcome = SchemaUpgrader::Upgrade(connection, directory, 7005, report);
         if (outcome.ok || outcome.error.Find(_T("No upgrade script leads from schema version 7004")) < 0)
            Fail_(_T("A missing step was not reported as one: ") + outcome.error);

         // A database from a newer build is refused, not walked.
         outcome = SchemaUpgrader::Upgrade(connection, directory, 7002, report);
         if (outcome.ok || outcome.error.Find(_T("NEWER")) < 0)
            Fail_(_T("A database past the required version was not refused: ") + outcome.error);

         connection->Disconnect();
         connection.reset();
#endif
      }
      catch (...)
      {
         FileUtilities::DeleteDirectory(directory, true);
         throw;
      }

      FileUtilities::DeleteDirectory(directory, true);
   }
}
