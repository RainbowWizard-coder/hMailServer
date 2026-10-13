// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "DatabaseClaimTester.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <numeric>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

#include "DALConnection.h"
#include "DALConnectionFactory.h"
#include "DALRecordset.h"
#include "DatabaseConnectionManager.h"
#include "SQLCommand.h"
#include "SQLStatement.h"
#include "../Application/IniFileSettings.h"
#include "../Util/FileUtilities.h"
#include "../Util/GUIDCreator.h"
#include "../Util/PasswordGenerator.h"

#ifndef HM_PLATFORM_POSIX
#include "SQLCEConnection.h"
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      void Fail(const String &sentence)
      {
         throw std::logic_error(std::string(AnsiString(sentence).c_str()));
      }

      String Number(__int64 value)
      {
         return Formatter::Format(_T("{0}"), value);
      }

      // What differs between the four engines in the statements below.
      class Dialect
      {
      public:
         explicit Dialect(DatabaseSettings::SQLDBType type) : type_(type) {}

         DatabaseSettings::SQLDBType Type() const { return type_; }

         bool Is(DatabaseSettings::SQLDBType type) const { return type_ == type; }

         // A point in time, to the microsecond where the engine keeps it.
         String StampType() const
         {
            switch (type_)
            {
            case DatabaseSettings::TypePGServer: return _T("timestamp");
            case DatabaseSettings::TypeMYSQLServer: return _T("datetime(6)");
            case DatabaseSettings::TypeMSSQLServer: return _T("datetime2");
            default: return _T("datetime");
            }
         }

         String NameType() const
         {
            if (type_ == DatabaseSettings::TypeMSSQLServer || type_ == DatabaseSettings::TypeMSSQLCompactEdition)
               return _T("nvarchar(100)");

            return _T("varchar(100)");
         }

         String TableOptions() const
         {
            return type_ == DatabaseSettings::TypeMYSQLServer ? String(_T(" ENGINE=InnoDB")) : String();
         }

         // A point in time written yyyy-mm-dd hh:mi:ss.mmm, as each engine reads one.
         String Literal(const String &text) const
         {
            switch (type_)
            {
            case DatabaseSettings::TypePGServer: return _T("timestamp '") + text + _T("'");
            case DatabaseSettings::TypeMYSQLServer: return _T("cast('") + text + _T("' as datetime(6))");
            default: return _T("CONVERT(datetime, '") + text + _T("', 121)");
            }
         }

         // Whole seconds, and milliseconds, from one point in time to another.
         String SecondsBetween(const String &from, const String &to) const
         {
            switch (type_)
            {
            case DatabaseSettings::TypePGServer: return _T("cast(round(extract(epoch from (") + to + _T(" - ") + from + _T("))) as bigint)");
            case DatabaseSettings::TypeMYSQLServer: return _T("timestampdiff(second, ") + from + _T(", ") + to + _T(")");
            default: return _T("datediff(second, ") + from + _T(", ") + to + _T(")");
            }
         }

         String MillisecondsBetween(const String &from, const String &to) const
         {
            switch (type_)
            {
            case DatabaseSettings::TypePGServer: return _T("cast(round(extract(epoch from (") + to + _T(" - ") + from + _T(")) * 1000) as bigint)");
            case DatabaseSettings::TypeMYSQLServer: return _T("(timestampdiff(microsecond, ") + from + _T(", ") + to + _T(") div 1000)");
            default: return _T("datediff(millisecond, ") + from + _T(", ") + to + _T(")");
            }
         }

         // What asks the engine its version, and what puts the session's time zone
         // thirteen hours ahead of UTC: empty where there is none, SQL Server's and
         // Compact's local clock being the machine's.
         String VersionQuery() const
         {
            switch (type_)
            {
            case DatabaseSettings::TypePGServer:
            case DatabaseSettings::TypeMYSQLServer: return _T("select version() as engineversion");
            case DatabaseSettings::TypeMSSQLServer: return _T("select cast(serverproperty('productversion') as nvarchar(64)) as engineversion");
            default: return String();
            }
         }

         String ZoneAhead() const
         {
            switch (type_)
            {
            case DatabaseSettings::TypePGServer: return _T("SET TIME ZONE INTERVAL '+13:00' HOUR TO MINUTE");
            case DatabaseSettings::TypeMYSQLServer: return _T("SET time_zone = '+13:00'");
            default: return String();
            }
         }

         // A session's own id, how another session ends it, and whether it is gone:
         // only where an application's own login may end its own sessions.
         String SessionId() const
         {
            switch (type_)
            {
            case DatabaseSettings::TypePGServer: return _T("select pg_backend_pid() as sessionid");
            case DatabaseSettings::TypeMYSQLServer: return _T("select connection_id() as sessionid");
            default: return String();
            }
         }

         String EndSession(__int64 id) const
         {
            if (type_ == DatabaseSettings::TypePGServer)
               return _T("select pg_terminate_backend(") + Number(id) + _T(")");

            return _T("KILL ") + Number(id);
         }

         String SessionsWithId(__int64 id) const
         {
            if (type_ == DatabaseSettings::TypePGServer)
               return _T("select count(*) as sessions from pg_stat_activity where pid = ") + Number(id);

            return _T("select count(*) as sessions from information_schema.processlist where id = ") + Number(id);
         }

         // What has the database close this session once it has been idle for two
         // seconds, as it closes one idle for longer than its own setting allows.
         String IdleTimeout() const
         {
            if (type_ == DatabaseSettings::TypePGServer)
               return _T("SET idle_session_timeout = 2000");

            return _T("SET SESSION wait_timeout = 2");
         }

      private:
         DatabaseSettings::SQLDBType type_;
      };

      // This machine's clock, UTC or local, as yyyy-mm-dd hh:mi:ss.mmm.
      String ClockText(bool utc)
      {
         SYSTEMTIME now;
         if (utc)
            GetSystemTime(&now);
         else
            GetLocalTime(&now);

         String text;
         text.Format(_T("%04d-%02d-%02d %02d:%02d:%02d.%03d"), (int) now.wYear, (int) now.wMonth, (int) now.wDay,
            (int) now.wHour, (int) now.wMinute, (int) now.wSecond, (int) now.wMilliseconds);
         return text;
      }

      // An epoch for one attempt: 62 random bits, never 0, as a claim draws one.
      class Epochs
      {
      public:
         Epochs() : generator_(std::random_device()()) {}

         __int64 Next()
         {
            std::lock_guard<std::mutex> guard(mutex_);
            return (__int64) ((generator_() >> 2) | 1);
         }

      private:
         std::mutex mutex_;
         std::mt19937_64 generator_;
      };

      class Run
      {
      public:
         Run(std::shared_ptr<DatabaseSettings> settings, DatabaseClaimTester::Report report, bool provokeErrors) :
            settings_(settings),
            dialect_(settings->GetType()),
            report_(report),
            provoke_errors_(provokeErrors)
         {
         }

         void All()
         {
            String error;
            if (!pool_.CreateConnections(settings_, 1, error))
               Fail(_T("The tester's pool of one connection could not connect: ") + error);

            side_ = Connect_();

            try
            {
               DropTables_();
               CreateTables_();

               const String versionQuery = dialect_.VersionQuery();
               String version = DatabaseSettings::GetDatabaseTypeName(dialect_.Type());
               if (!versionQuery.IsEmpty())
               {
                  engine_version_ = Text_(versionQuery, "engineversion");
                  version += _T(" ") + engine_version_;
               }

               report_(_T("database: ") + version);

               Counts_();
               Failures_();
               Forgiven_();
               Claims_();
               Races_();
               Reconnect_();

               String lowered = engine_version_;
               lowered.MakeLower();
               if (dialect_.Is(DatabaseSettings::TypeMYSQLServer) && lowered.Find(_T("mariadb")) >= 0)
                  MariaDbError4031_();

               Lease_();
               Clock_();

               if (dialect_.Is(DatabaseSettings::TypeMSSQLCompactEdition))
                  CompactUtcFunction_();
            }
            catch (...)
            {
               Close_();
               throw;
            }

            // A run whose checks all held still fails when its tables stay behind:
            // the suites that run it promise a database with nothing of its own left.
            const String left = Close_();
            if (!left.IsEmpty())
               Fail(_T("The tester's tables aaclaimprobe and aaclaimlock could not be dropped at the end: ") + left);
         }

      private:

         std::shared_ptr<DALConnection> Connect_()
         {
            String error;
            std::shared_ptr<DALConnection> connection = DALConnectionFactory::CreateConnection(settings_);
            if (!connection || connection->Connect(error) != DALConnection::Connected)
               Fail(_T("A connection of the tester's own could not connect: ") + error);

            return connection;
         }

         // The tables go, whatever happened before: each drop checked, and what was
         // refused said, and answered (empty when both went). Nothing here throws but
         // by a fault of its own, and such a fault must not take the place of the
         // failure that brought a run here, so it is said instead.
         String Close_()
         {
            String left;
            try
            {
               if (side_)
               {
                  String error;
                  if (!side_->IsConnected() && side_->Connect(error) != DALConnection::Connected)
                  {
                     left = _T("the tester's own connection could not connect again: ") + error;
                  }
                  else
                  {
                     // Execute, which reconnects a connection the database has ended
                     // under it and runs the drop again: on MySQL IsConnected still
                     // says true of such a connection.
                     const String tables[] = { _T("aaclaimprobe"), _T("aaclaimlock") };
                     for (const String &table : tables)
                     {
                        if (!side_->Execute(SQLCommand(_T("drop table ") + table), error, 0, 0))
                           left += (left.IsEmpty() ? String() : String(_T("; "))) + table + _T(": ") + error;
                     }
                  }
               }
            }
            catch (...)
            {
               left = _T("the clean-up failed in a way of its own");
            }

            if (!left.IsEmpty())
               report_(_T("the tester's tables aaclaimprobe and aaclaimlock may be left on the database (") + left + _T("): drop them by hand"));

            if (side_)
               side_->Disconnect();

            pool_.Disconnect();
            return left;
         }

         // At the start, where either may not exist: what a run cut off before its
         // end left.
         void DropTables_()
         {
            String error;
            side_->TryExecute(SQLCommand(_T("drop table aaclaimprobe")), error, 0, 0);
            side_->TryExecute(SQLCommand(_T("drop table aaclaimlock")), error, 0, 0);
         }

         void CreateTables_()
         {
            Side_(_T("create table aaclaimprobe (probeid int not null primary key, probeowner int not null, probeepoch bigint not null, ")
                  _T("probecounter int not null, probestamp ") + dialect_.StampType() + _T(" null)") + dialect_.TableOptions());
            Side_(_T("create table aaclaimlock (lockname ") + dialect_.NameType() + _T(" not null primary key, locknode int not null, ")
                  _T("lockuntil ") + dialect_.StampType() + _T(" not null, lockepoch bigint not null)") + dialect_.TableOptions());
         }

         // A statement on the side connection, once, that must succeed.
         void Side_(const String &sql)
         {
            String error;
            if (side_->TryExecute(SQLCommand(sql), error, 0, 0) != DALConnection::DALSuccess)
               Fail(_T("A statement of the tester's own failed: ") + sql + _T(": ") + error);
         }

         // A statement through the pool, judged by its count: it must not have needed
         // a reconnect, which only Reconnect_ provokes.
         __int64 Count_(const String &sql, int ignoreErrors = 0)
         {
            bool reconnected = true;
            const __int64 count = pool_.ExecuteCount(SQLCommand(sql), &reconnected, ignoreErrors);
            if (reconnected)
               Fail(_T("ExecuteCount said a reconnect happened where none could have: ") + sql);

            return count;
         }

         std::shared_ptr<DALRecordset> Open_(std::shared_ptr<DALConnection> connection, const String &sql)
         {
            String error;
            std::shared_ptr<DALRecordset> recordset = connection->CreateRecordset();
            if (recordset->TryOpen(connection, SQLCommand(sql), error) != DALConnection::DALSuccess)
               Fail(_T("A query of the tester's own failed: ") + sql + _T(": ") + error);

            if (recordset->IsEOF())
               Fail(_T("A query of the tester's own found no row: ") + sql);

            return recordset;
         }

         __int64 Value_(const String &sql, const AnsiString &column)
         {
            return Open_(side_, sql)->GetInt64Value(column);
         }

         String Text_(const String &sql, const AnsiString &column)
         {
            return Open_(side_, sql)->GetStringValue(column);
         }

         static void Expect_(const String &what, __int64 got, __int64 wanted)
         {
            if (got != wanted)
               Fail(Formatter::Format(_T("{0}: {1}, where {2} was wanted."), what, got, wanted));
         }

         String ProbeRow_(int id, int owner = 0)
         {
            return Formatter::Format(_T("insert into aaclaimprobe (probeid, probeowner, probeepoch, probecounter) values ({0}, {1}, 0, 0)"), id, owner);
         }

         void Counts_()
         {
            Side_(_T("delete from aaclaimprobe"));

            Expect_(_T("An INSERT counted"), Count_(ProbeRow_(1)), 1);
            Expect_(_T("An INSERT counted"), Count_(ProbeRow_(2)), 1);

            const __int64 none = Count_(_T("update aaclaimprobe set probecounter = probecounter + 1 where probeid = 99"));
            const __int64 one = Count_(_T("update aaclaimprobe set probecounter = probecounter + 1 where probeid = 1"));
            const __int64 unchanged = Count_(_T("update aaclaimprobe set probeowner = 0 where probeid = 1"));
            const __int64 two = Count_(_T("update aaclaimprobe set probecounter = probecounter + 1 where probeid in (1, 2)"));
            const __int64 inserted = Count_(ProbeRow_(3));
            const __int64 deleted = Count_(_T("delete from aaclaimprobe where probeid = 3"));
            const __int64 deletedNone = Count_(_T("delete from aaclaimprobe where probeid = 3"));
            const __int64 refused = Count_(ProbeRow_(1), DALConnection::DALErrorInSQL);

            Expect_(_T("An UPDATE that matches no row counted"), none, 0);
            Expect_(_T("An UPDATE that matches one row counted"), one, 1);
            Expect_(_T("An UPDATE that matches one row and leaves it as it was counted"), unchanged, 1);
            Expect_(_T("An UPDATE that matches two rows counted"), two, 2);
            Expect_(_T("An INSERT counted"), inserted, 1);
            Expect_(_T("A DELETE of one row counted"), deleted, 1);
            Expect_(_T("A DELETE of no row counted"), deletedNone, 0);
            Expect_(_T("An INSERT the primary key refused, the refusal ignored, counted"), refused, 0);

            // The same through DALConnection itself: Execute, which runs a statement
            // again after a reconnect, and TryExecute, which runs it once.
            String error;
            __int64 count = -2;
            bool reconnected = true;
            if (!side_->Execute(SQLCommand(_T("update aaclaimprobe set probeowner = 0 where probeid = 1")), error, 0, 0, &count, &reconnected) || reconnected)
               Fail(_T("DALConnection::Execute did not run an UPDATE on a connection that was up: ") + error);
            Expect_(_T("DALConnection::Execute's count of an UPDATE that leaves its row as it was"), count, 1);

            count = -2;
            if (side_->TryExecute(SQLCommand(_T("update aaclaimprobe set probecounter = probecounter + 1 where probeid in (1, 2)")), error, 0, 0, &count) != DALConnection::DALSuccess)
               Fail(_T("DALConnection::TryExecute did not run an UPDATE: ") + error);
            Expect_(_T("DALConnection::TryExecute's count of an UPDATE of two rows"), count, 2);

            report_(Formatter::Format(_T("counts: an UPDATE matching no row {0}, one row {1}, one row it leaves as it was {2}, two rows {3}; "),
               none, one, unchanged, two) +
               Formatter::Format(_T("an INSERT {0}; a DELETE {1}, of nothing {2}; an INSERT the key refused, ignored, {3}"), inserted, deleted, deletedNone, refused));
         }

         void Failures_()
         {
            const String failing = _T("update aaclaimnosuchtable set probecounter = 1");

            String error;
            __int64 count = -2;
            if (side_->TryExecute(SQLCommand(failing), error, 0, 0, &count) == DALConnection::DALSuccess)
               Fail(_T("A statement on a table that does not exist succeeded."));
            Expect_(_T("TryExecute's count of a statement that failed"), count, -1);

            String through = _T("TryExecute");
            if (provoke_errors_)
            {
               // Reports HM5032 to the error log, as any failed statement does.
               bool reconnected = true;
               Expect_(_T("ExecuteCount of a statement that failed"), pool_.ExecuteCount(SQLCommand(failing), &reconnected), -1);
               through += _T(" and ExecuteCount");
            }

            report_(_T("a statement that fails counts -1 (") + through + _T(")"));
         }

         // DALErrorInSQL forgives a row the database's own rules refused, and that
         // only (DALConnection::TryExecute): a duplicate key everywhere (Counts_), a
         // NULL in a NOT NULL column on SQL Server alone, where it is an integrity
         // violation as a duplicate is, and nothing else - an INSERT into a table
         // that does not exist fails as it would without the flag. Until AA-01's
         // repair MySQL, SQL Server and Compact forgave every failure but SQL
         // Server's lost connection, counting 0, since the test was a bitwise AND
         // with an ExecutionResult that is not a set of bits.
         void Forgiven_()
         {
            Side_(_T("delete from aaclaimprobe where probeid = 50"));

            const String missingTable = _T("insert into aaclaimnosuchtable (probeid, probeowner, probeepoch, probecounter) values (50, 0, 0, 0)");
            const String nullOwner = _T("insert into aaclaimprobe (probeid, probeowner, probeepoch, probecounter) values (50, NULL, 0, 0)");
            const bool nullIsIntegrity = dialect_.Is(DatabaseSettings::TypeMSSQLServer);

            String error;
            __int64 count = -2;
            if (side_->TryExecute(SQLCommand(missingTable), error, 0, DALConnection::DALErrorInSQL, &count) == DALConnection::DALSuccess)
               Fail(_T("An INSERT into a table that does not exist, told to forgive a duplicate, was forgiven."));
            Expect_(_T("TryExecute's count of an INSERT into a table that does not exist, told to forgive a duplicate"), count, -1);

            count = -2;
            const bool nullForgiven = side_->TryExecute(SQLCommand(nullOwner), error, 0, DALConnection::DALErrorInSQL, &count) == DALConnection::DALSuccess;
            if (nullForgiven != nullIsIntegrity)
               Fail(nullIsIntegrity
                  ? String(_T("An INSERT of a NULL into a NOT NULL column, told to forgive a duplicate, failed on SQL Server, where that is an integrity violation as a duplicate is: ")) + error
                  : String(_T("An INSERT of a NULL into a NOT NULL column, told to forgive a duplicate, was forgiven.")));
            Expect_(_T("TryExecute's count of an INSERT of a NULL into a NOT NULL column, told to forgive a duplicate"), count, nullIsIntegrity ? 0 : -1);
            Expect_(_T("Rows that INSERT wrote"), Value_(_T("select count(*) as held from aaclaimprobe where probeid = 50"), "held"), 0);

            String through = _T("TryExecute");
            if (provoke_errors_)
            {
               // Each reports HM5032 to the error log, as any failed statement does.
               bool reconnected = true;
               Expect_(_T("ExecuteCount of an INSERT into a table that does not exist, told to forgive a duplicate"),
                  pool_.ExecuteCount(SQLCommand(missingTable), &reconnected, DALConnection::DALErrorInSQL), -1);
               if (reconnected)
                  Fail(_T("ExecuteCount reconnected for an INSERT into a table that does not exist."));

               reconnected = true;
               Expect_(_T("ExecuteCount of an INSERT of a NULL into a NOT NULL column, told to forgive a duplicate"),
                  pool_.ExecuteCount(SQLCommand(nullOwner), &reconnected, DALConnection::DALErrorInSQL), nullIsIntegrity ? 0 : -1);
               if (reconnected)
                  Fail(_T("ExecuteCount reconnected for an INSERT of a NULL into a NOT NULL column."));

               through += _T(" and ExecuteCount");
            }

            report_(Formatter::Format(_T("told to forgive a duplicate (DALErrorInSQL): an INSERT into a table that does not exist -1, of a NULL into a NOT NULL column {0} ({1}), "),
                  nullIsIntegrity ? 0 : -1, nullIsIntegrity ? _T("forgiven: an integrity violation on SQL Server, as a duplicate is") : _T("not forgiven")) +
               _T("each by ") + through);
         }

         String Claim_(int id, int owner, __int64 epoch, bool recognisesItself)
         {
            String sql = Formatter::Format(_T("update aaclaimprobe set probeowner = {0}, probeepoch = {1} where probeid = {2} and "), owner, epoch, id);
            if (recognisesItself)
               sql += Formatter::Format(_T("(probeowner = 0 or probeepoch = {0})"), epoch);
            else
               sql += _T("probeowner = 0");

            return sql;
         }

         void Claims_()
         {
            Side_(_T("delete from aaclaimprobe"));
            Side_(ProbeRow_(1));

            const __int64 epoch = epochs_.Next();
            const __int64 another = epochs_.Next();

            const __int64 first = Count_(Claim_(1, 7, epoch, true));
            const __int64 again = Count_(Claim_(1, 7, epoch, true));
            const __int64 plain = Count_(Claim_(1, 7, epoch, false));
            const __int64 other = Count_(Claim_(1, 8, another, true));

            Expect_(_T("A claim of a free row counted"), first, 1);
            Expect_(_T("The same claim run again, as a reconnect runs it, counted"), again, 1);
            Expect_(_T("A claim that does not match its own epoch, run again, counted"), plain, 0);
            Expect_(_T("Another attempt's claim of the claimed row counted"), other, 0);
            Expect_(_T("The claimed row's epoch"), Value_(_T("select probeepoch from aaclaimprobe where probeid = 1"), "probeepoch"), epoch);

            report_(Formatter::Format(_T("claims: a free row {0}; the same claim run again {1}; run again without matching its epoch {2}; another epoch {3}"),
               first, again, plain, other));
         }

         void Races_()
         {
            const int rows = 200;
            const int names = 50;

            Side_(_T("delete from aaclaimprobe"));
            Side_(_T("delete from aaclaimlock"));
            for (int id = 1; id <= rows; id++)
               Side_(ProbeRow_(id));

            // Two connections of their own, as two nodes are; nothing in this process
            // stands between them.
            std::shared_ptr<DALConnection> connections[2] = { Connect_(), Connect_() };
            std::atomic<int> rowWins[2];
            std::atomic<int> nameWins[2];
            std::atomic<int> failures(0);
            std::mutex failureMutex;
            String firstFailure;

            for (int i = 0; i < 2; i++)
            {
               rowWins[i] = 0;
               nameWins[i] = 0;
            }

            const DatabaseSettings::SQLDBType type = dialect_.Type();
            const unsigned seed = std::random_device()();

            auto racer = [&](int me)
            {
               std::shared_ptr<DALConnection> connection = connections[me - 1];
               std::mt19937 shuffle(seed + (unsigned) me);

               const auto note = [&](const String &what)
               {
                  failures++;
                  std::lock_guard<std::mutex> guard(failureMutex);
                  if (firstFailure.IsEmpty())
                     firstFailure = what;
               };

               std::vector<int> order(rows);
               std::iota(order.begin(), order.end(), 1);
               std::shuffle(order.begin(), order.end(), shuffle);

               for (int id : order)
               {
                  String error;
                  __int64 count = -1;
                  const String sql = Formatter::Format(_T("update aaclaimprobe set probeowner = {0}, probeepoch = {1} where probeid = {2} and probeowner = 0"),
                     me, epochs_.Next(), id);
                  if (connection->TryExecute(SQLCommand(sql), error, 0, 0, &count) != DALConnection::DALSuccess)
                     note(error);
                  else if (count == 1)
                     rowWins[me - 1]++;
                  else if (count != 0)
                     note(Formatter::Format(_T("a claim counted {0}"), count));
               }

               std::vector<int> nameOrder(names);
               std::iota(nameOrder.begin(), nameOrder.end(), 1);
               std::shuffle(nameOrder.begin(), nameOrder.end(), shuffle);

               for (int n : nameOrder)
               {
                  // Insert-or-claim: the claim, which counts 0 while the name has no
                  // row, and then the insert, which the unique name lets one win.
                  const __int64 epoch = epochs_.Next();
                  const String name = Formatter::Format(_T("race-{0}"), n);
                  const String claim = Formatter::Format(_T("update aaclaimlock set locknode = {0}, lockuntil = {1}, lockepoch = {2} where lockname = '{3}' and "),
                     me, SQLStatement::GetCurrentUtcTimestampPlusSeconds(type, 600), epoch, name) +
                     Formatter::Format(_T("(lockuntil < {0} or lockepoch = {1})"), SQLStatement::GetCurrentUtcTimestamp(type), epoch);
                  const String insert = Formatter::Format(_T("insert into aaclaimlock (lockname, locknode, lockuntil, lockepoch) values ('{0}', {1}, {2}, {3})"),
                     name, me, SQLStatement::GetCurrentUtcTimestampPlusSeconds(type, 600), epoch);

                  String error;
                  __int64 count = -1;
                  if (connection->TryExecute(SQLCommand(claim), error, 0, 0, &count) != DALConnection::DALSuccess)
                  {
                     note(error);
                     continue;
                  }

                  if (count == 0 && connection->TryExecute(SQLCommand(insert), error, 0, DALConnection::DALErrorInSQL, &count) != DALConnection::DALSuccess)
                  {
                     note(error);
                     continue;
                  }

                  if (count == 1)
                     nameWins[me - 1]++;
                  else if (count != 0)
                     note(Formatter::Format(_T("an insert-or-claim counted {0}"), count));
               }
            };

            std::thread first(racer, 1);
            std::thread second(racer, 2);
            first.join();
            second.join();

            connections[0]->Disconnect();
            connections[1]->Disconnect();

            if (failures > 0)
               Fail(Formatter::Format(_T("{0} statements of the race failed; the first: {1}"), (int) failures, firstFailure));

            Expect_(_T("Rows won by the two connections together, of 200"), rowWins[0] + rowWins[1], rows);
            Expect_(_T("Rows the first connection holds against the ones it counted as won"),
               Value_(_T("select count(*) as held from aaclaimprobe where probeowner = 1"), "held"), rowWins[0]);
            Expect_(_T("Rows the second connection holds against the ones it counted as won"),
               Value_(_T("select count(*) as held from aaclaimprobe where probeowner = 2"), "held"), rowWins[1]);
            Expect_(_T("Names won by the two connections together, of 50"), nameWins[0] + nameWins[1], names);
            Expect_(_T("Names with a row, of 50"), Value_(_T("select count(*) as held from aaclaimlock"), "held"), names);
            Expect_(_T("Names the first connection holds against the ones it counted as won"),
               Value_(_T("select count(*) as held from aaclaimlock where locknode = 1"), "held"), nameWins[0]);

            report_(Formatter::Format(_T("races: {0} rows claimed by two connections at once, each won once ({1} and {2}); "), rows, (int) rowWins[0], (int) rowWins[1]) +
               Formatter::Format(_T("{0} named leases with no row yet taken by insert-or-claim, each won once ({1} and {2})"), names, (int) nameWins[0], (int) nameWins[1]));
         }

         void Reconnect_()
         {
            Side_(_T("delete from aaclaimprobe"));
            Side_(ProbeRow_(1));
            Side_(ProbeRow_(2));
            Side_(ProbeRow_(3));

            // A connection closed under the statement. DALConnection::Execute finds it
            // gone, reconnects, runs the statement on the new connection and says so.
            std::shared_ptr<DALConnection> connection = Connect_();
            connection->Disconnect();

            const __int64 epoch = epochs_.Next();
            String error;
            __int64 count = -2;
            bool reconnected = false;
            if (!connection->Execute(SQLCommand(Claim_(1, 5, epoch, true)), error, 0, 0, &count, &reconnected))
               Fail(_T("DALConnection::Execute did not reconnect a closed connection and run the claim: ") + error);
            if (!reconnected)
               Fail(_T("DALConnection::Execute reconnected a closed connection and did not say so."));
            Expect_(_T("A claim run on a connection Execute had to reconnect counted"), count, 1);

            count = -2;
            reconnected = true;
            if (!connection->Execute(SQLCommand(Claim_(1, 5, epoch, true)), error, 0, 0, &count, &reconnected) || reconnected)
               Fail(_T("DALConnection::Execute did not run the claim again on the connection it had reconnected: ") + error);
            Expect_(_T("The claim run again after the reconnect counted"), count, 1);
            connection->Disconnect();

            String line = _T("reconnect: a claim on a connection closed under it ran on a new one, which Execute said, and counted 1; run again 1");

            // The database ends the pool's own session, as a restart or a failover of
            // the database ends every session: ExecuteCount reconnects, counts the
            // claim and says that it reconnected.
            if (!dialect_.SessionId().IsEmpty())
            {
               EndPoolSession_();

               const __int64 epochTwo = epochs_.Next();
               bool poolReconnected = false;
               const __int64 poolCount = pool_.ExecuteCount(SQLCommand(Claim_(2, 6, epochTwo, true)), &poolReconnected);
               if (!poolReconnected)
                  Fail(_T("ExecuteCount ran a statement on a session the database had ended and did not say it reconnected."));
               Expect_(_T("ExecuteCount's count of a claim after the database ended its session"), poolCount, 1);
               Expect_(_T("The claim run again after that"), Count_(Claim_(2, 6, epochTwo, true)), 1);
               Expect_(_T("The claimed row's epoch"), Value_(_T("select probeepoch from aaclaimprobe where probeid = 2"), "probeepoch"), epochTwo);

               line += Formatter::Format(_T("; after the database ended the pool's session ({0}), ExecuteCount reconnected, said so and counted 1"),
                  dialect_.Is(DatabaseSettings::TypePGServer) ? _T("pg_terminate_backend") : _T("KILL"));

               // An INSERT told to forgive a duplicate, on a session the database has
               // ended: DALErrorInSQL never forgives a lost connection
               // (DALConnection::TryExecute), so it is reconnected and run as the
               // claim was. On MySQL and MariaDB it was forgiven until AA-01's repair:
               // counted 0, not run again, and no row written.
               EndPoolSession_();

               bool insertReconnected = false;
               const __int64 insertCount = pool_.ExecuteCount(SQLCommand(ProbeRow_(4)), &insertReconnected, DALConnection::DALErrorInSQL);
               if (!insertReconnected)
                  Fail(_T("ExecuteCount ran an INSERT told to forgive a duplicate on a session the database had ended and did not say it reconnected."));
               Expect_(_T("ExecuteCount's count of an INSERT told to forgive a duplicate, after the database ended its session"), insertCount, 1);
               Expect_(_T("Rows that INSERT wrote"), Value_(_T("select count(*) as held from aaclaimprobe where probeid = 4"), "held"), 1);

               line += _T("; an INSERT told to forgive a duplicate the same, the lost connection not forgiven");

               // The database closes the pool's session once it has been idle for two
               // seconds (idle_session_timeout, wait_timeout), as it closes any left idle
               // for longer than its own setting allows: MySQL 8.0.24 and later say so
               // with an error of their own, where the others' connection is simply gone.
               // A PostgreSQL before 14, which has no such setting, is said and skipped.
               String error;
               if (!pool_.Execute(SQLCommand(dialect_.IdleTimeout()), 0, 0, error))
               {
                  line += _T("; the session's idle timeout could not be set (") + error + _T("), so a session closed for being idle is not checked");
               }
               else
               {
                  Sleep(4000);

                  const __int64 epochThree = epochs_.Next();
                  bool idleReconnected = false;
                  const __int64 idleCount = pool_.ExecuteCount(SQLCommand(Claim_(3, 6, epochThree, true)), &idleReconnected);
                  if (!idleReconnected)
                     Fail(_T("ExecuteCount ran a statement on a session the database had closed for being idle and did not say it reconnected."));
                  Expect_(_T("ExecuteCount's count of a claim after the database closed its idle session"), idleCount, 1);
                  Expect_(_T("The claimed row's epoch"), Value_(_T("select probeepoch from aaclaimprobe where probeid = 3"), "probeepoch"), epochThree);

                  line += _T("; after the database closed it for being idle, the same");
               }
            }

            report_(line);
         }

         // The database ends the pool's own session, as a restart or a failover of the
         // database ends every session, and the tester waits until it is gone. Only
         // where an application's own login may end its own sessions (Dialect).
         void EndPoolSession_()
         {
            const String sessionIdQuery = dialect_.SessionId();
            std::shared_ptr<DALRecordset> session = pool_.OpenRecordset(SQLCommand(sessionIdQuery));
            if (!session || session->IsEOF())
               Fail(_T("The pool's session id could not be read: ") + sessionIdQuery);
            const __int64 sessionId = session->GetInt64Value("sessionid");
            session.reset();

            Side_(dialect_.EndSession(sessionId));

            bool gone = false;
            for (int wait = 0; wait < 100 && !gone; wait++)
            {
               gone = Value_(dialect_.SessionsWithId(sessionId), "sessions") == 0;
               if (!gone)
                  Sleep(100);
            }
            if (!gone)
               Fail(Formatter::Format(_T("The pool's session {0} was still there ten seconds after it was ended."), sessionId));
         }

         // MariaDB's error 4031 is ER_REFERENCED_TRG_DOES_NOT_EXIST, the refusal of a
         // CREATE TRIGGER that FOLLOWS a trigger there is not, where MySQL's 4031 is
         // the close of an idle session (Reconnect_): a statement MariaDB refuses
         // with it fails at once, counted -1, and is not run again on a new
         // connection (MySQLConnection::CheckError). Which error the server answered
         // is read with GET DIAGNOSTICS, whose number no translation of the message
         // text changes; a server that refuses the trigger for another reason first
         // (a privilege) is said, and the check left out.
         void MariaDbError4031_()
         {
            const String trigger = _T("create trigger aaclaimtrigger before insert on aaclaimprobe for each row follows aaclaimnosuchtrigger set new.probecounter = 1");

            String error;
            __int64 count = -2;
            const DALConnection::ExecutionResult result = side_->TryExecute(SQLCommand(trigger), error, 0, 0, &count);
            if (result == DALConnection::DALSuccess)
            {
               Side_(_T("drop trigger aaclaimtrigger"));
               Fail(_T("MariaDB made a trigger that FOLLOWS a trigger there is not."));
            }

            Side_(_T("get diagnostics condition 1 @aaclaimerror = mysql_errno"));
            const __int64 code = Value_(_T("select @aaclaimerror as errorcode"), "errorcode");
            if (code != 4031)
            {
               report_(Formatter::Format(_T("MariaDB's error 4031 not checked: the server refused the trigger with error {0} first"), code));
               return;
            }

            if (result == DALConnection::DALConnectionProblem)
               Fail(_T("TryExecute took MariaDB's error 4031, a trigger the statement names that does not exist, for a lost connection."));
            Expect_(_T("TryExecute's count of a statement MariaDB refused with error 4031"), count, -1);

            String through = _T("TryExecute");
            if (provoke_errors_)
            {
               // Reports HM5032 to the error log, as any failed statement does. Taken
               // for a lost connection it would be run six times on eight new
               // connections first, about six seconds.
               bool reconnected = true;
               const __int64 executed = pool_.ExecuteCount(SQLCommand(trigger), &reconnected);
               if (reconnected)
                  Fail(_T("ExecuteCount took MariaDB's error 4031 for a lost connection: it reconnected and ran the statement again."));
               Expect_(_T("ExecuteCount's count of a statement MariaDB refused with error 4031"), executed, -1);
               through += _T(" and ExecuteCount");
            }

            report_(_T("MariaDB's error 4031 (a trigger the statement names that does not exist): the statement failed at once, counted -1 and was not run again (") + through + _T(")"));
         }

         void Lease_()
         {
            Side_(_T("delete from aaclaimlock"));

            const DatabaseSettings::SQLDBType type = dialect_.Type();
            const __int64 first = epochs_.Next();
            const __int64 second = epochs_.Next();
            const int seconds = 3;

            // Built when each is run: SQL Server Compact's UTC clock is a literal of
            // this machine's (SQLStatement::GetCurrentUtcTimestamp).
            const auto claim = [&](int node, __int64 epoch)
            {
               return Formatter::Format(_T("update aaclaimlock set locknode = {0}, lockuntil = {1}, lockepoch = {2} where lockname = 'aa-lease' and "),
                  node, SQLStatement::GetCurrentUtcTimestampPlusSeconds(type, seconds), epoch) +
                  Formatter::Format(_T("(lockuntil < {0} or lockepoch = {1})"), SQLStatement::GetCurrentUtcTimestamp(type), epoch);
            };
            const auto insert = [&](int node, __int64 epoch)
            {
               return Formatter::Format(_T("insert into aaclaimlock (lockname, locknode, lockuntil, lockepoch) values ('aa-lease', {0}, {1}, {2})"),
                  node, SQLStatement::GetCurrentUtcTimestampPlusSeconds(type, seconds), epoch);
            };
            const auto renew = [&](__int64 epoch)
            {
               return Formatter::Format(_T("update aaclaimlock set lockuntil = {0} where lockname = 'aa-lease' and lockepoch = {1}"),
                  SQLStatement::GetCurrentUtcTimestampPlusSeconds(type, seconds), epoch);
            };

            const __int64 noRow = Count_(claim(1, first));
            const __int64 inserted = Count_(insert(1, first), DALConnection::DALErrorInSQL);
            const __int64 early = Count_(claim(2, second));
            const __int64 secondInsert = Count_(insert(2, second), DALConnection::DALErrorInSQL);
            const __int64 holderAgain = Count_(claim(1, first));
            const __int64 renewed = Count_(renew(first));

            Expect_(_T("A claim of a lease that has no row yet counted"), noRow, 0);
            Expect_(_T("The insert that takes it counted"), inserted, 1);
            Expect_(_T("Another node's claim before the lease expired counted"), early, 0);
            Expect_(_T("Another node's insert of the taken name, its refusal ignored, counted"), secondInsert, 0);
            Expect_(_T("The holder's own claim again counted"), holderAgain, 1);
            Expect_(_T("The holder's renewal counted"), renewed, 1);

            Sleep((seconds + 2) * 1000);

            const __int64 late = Count_(claim(2, second));
            const __int64 fenced = Count_(renew(first));
            const __int64 back = Count_(claim(1, first));

            Expect_(_T("Another node's claim after the lease expired counted"), late, 1);
            Expect_(_T("The old holder's renewal after that counted"), fenced, 0);
            Expect_(_T("The old holder's claim while the new one holds it counted"), back, 0);
            Expect_(_T("The lease's epoch"), Value_(_T("select lockepoch from aaclaimlock where lockname = 'aa-lease'"), "lockepoch"), second);

            report_(Formatter::Format(_T("lease on the UTC clock: claimed with no row {0}, inserted {1}; "), noRow, inserted) +
               Formatter::Format(_T("before it expired another's claim {0}, insert {1}, the holder's claim {2}, renewal {3}; "), early, secondInsert, holderAgain, renewed) +
               Formatter::Format(_T("after it expired another's claim {0}, the old holder's renewal {1} and claim {2}"), late, fenced, back));
         }

         // Seconds from this machine's UTC clock, given as text, to the row's stamp.
         __int64 SecondsFrom_(const String &utcText, int id)
         {
            return Value_(Formatter::Format(_T("select {0} as seconds from aaclaimprobe where probeid = {1}"),
               dialect_.SecondsBetween(dialect_.Literal(utcText), _T("probestamp")), id), "seconds");
         }

         void Clock_()
         {
            const DatabaseSettings::SQLDBType type = dialect_.Type();
            const int margin = 300;

            Side_(_T("delete from aaclaimprobe"));

            // The pool has one connection, so the zone set here is the zone of the
            // statements that follow.
            const String zoneAhead = dialect_.ZoneAhead();
            if (!zoneAhead.IsEmpty())
            {
               String error;
               if (!pool_.Execute(SQLCommand(zoneAhead), 0, 0, error))
                  Fail(_T("The session's time zone could not be set: ") + error);
            }

            const String stamp = _T("insert into aaclaimprobe (probeid, probeowner, probeepoch, probecounter, probestamp) values ({0}, 0, 0, 0, {1})");
            Expect_(_T("The insert of the UTC clock counted"), Count_(Formatter::Format(stamp, 10, SQLStatement::GetCurrentUtcTimestamp(type))), 1);
            Expect_(_T("The insert of the local clock counted"), Count_(Formatter::Format(stamp, 11, SQLStatement::GetCurrentTimestamp(type))), 1);
            Expect_(_T("The insert of the UTC clock an hour ahead counted"), Count_(Formatter::Format(stamp, 12, SQLStatement::GetCurrentUtcTimestampPlusSeconds(type, 3600))), 1);

            const String utcNow = ClockText(true);
            const String localNow = ClockText(false);

            const __int64 utc = SecondsFrom_(utcNow, 10);
            const __int64 local = SecondsFrom_(utcNow, 11);
            const __int64 ahead = SecondsFrom_(utcNow, 12);

            if (utc < -margin || utc > margin)
               Fail(Formatter::Format(_T("GetCurrentUtcTimestamp wrote a time {0} seconds from this machine's UTC clock."), utc));

            if (ahead - utc < 3600 - 5 || ahead - utc > 3600 + 5)
               Fail(Formatter::Format(_T("GetCurrentUtcTimestampPlusSeconds(3600) wrote a time {0} seconds after GetCurrentUtcTimestamp's."), ahead - utc));

            String localSaid;
            if (!zoneAhead.IsEmpty())
            {
               // The session at UTC+13: its local clock thirteen hours ahead of its UTC one.
               if (local < 13 * 3600 - margin || local > 13 * 3600 + margin)
                  Fail(Formatter::Format(_T("GetCurrentTimestamp wrote a time {0} seconds from this machine's UTC clock, where the session's zone is UTC+13."), local));

               localSaid = Formatter::Format(_T("local {0} s ahead, the session's zone being UTC+13"), local);
            }
            else if (dialect_.Is(DatabaseSettings::TypeMSSQLCompactEdition))
            {
               // Compact's GETDATE() is this machine's local clock, in this process.
               const __int64 offset = Value_(_T("select ") + dialect_.SecondsBetween(dialect_.Literal(utcNow), dialect_.Literal(localNow)) + _T(" as seconds from aaclaimprobe where probeid = 10"), "seconds");
               if (local < offset - margin || local > offset + margin)
                  Fail(Formatter::Format(_T("GetCurrentTimestamp wrote a time {0} seconds from this machine's UTC clock, where its local clock is {1} seconds from it."), local, offset));

               localSaid = Formatter::Format(_T("local {0} s ahead, as this machine's local clock is ({1} s)"), local, offset);
            }
            else
            {
               // SQL Server's GETDATE() is its own machine's local clock.
               localSaid = Formatter::Format(_T("local {0} s ahead (the database machine's own zone)"), local);
            }

            // Inside a transaction: the UTC clock is the statement's, and moves.
            // PostgreSQL's now() is the transaction's start and stands still there,
            // which is why its UTC clock is statement_timestamp()'s.
            std::shared_ptr<DALConnection> connection = Connect_();
            String error;
            if (!connection->BeginTransaction(error))
               Fail(_T("A transaction could not be begun: ") + error);

            const String stampAgain = _T("insert into aaclaimprobe (probeid, probeowner, probeepoch, probecounter, probestamp) values ({0}, 0, 0, 0, {1})");
            bool inserted = connection->TryExecute(SQLCommand(Formatter::Format(stampAgain, 13, SQLStatement::GetCurrentUtcTimestamp(type))), error, 0, 0) == DALConnection::DALSuccess;
            if (dialect_.Is(DatabaseSettings::TypePGServer))
               inserted = inserted && connection->TryExecute(SQLCommand(Formatter::Format(stampAgain, 15, _T("(now() at time zone 'utc')"))), error, 0, 0) == DALConnection::DALSuccess;

            Sleep(1500);

            inserted = inserted && connection->TryExecute(SQLCommand(Formatter::Format(stampAgain, 14, SQLStatement::GetCurrentUtcTimestamp(type))), error, 0, 0) == DALConnection::DALSuccess;
            if (dialect_.Is(DatabaseSettings::TypePGServer))
               inserted = inserted && connection->TryExecute(SQLCommand(Formatter::Format(stampAgain, 16, _T("(now() at time zone 'utc')"))), error, 0, 0) == DALConnection::DALSuccess;

            if (!inserted)
            {
               connection->RollbackTransaction(error);
               connection->Disconnect();
               Fail(_T("A stamp inside a transaction could not be written: ") + error);
            }

            if (!connection->CommitTransaction(error))
               Fail(_T("A transaction could not be committed: ") + error);
            connection->Disconnect();

            const String between = _T("select {0} as milliseconds from aaclaimprobe a, aaclaimprobe b where a.probeid = {1} and b.probeid = {2}");
            const __int64 moved = Value_(Formatter::Format(between, dialect_.MillisecondsBetween(_T("a.probestamp"), _T("b.probestamp")), 13, 14), "milliseconds");
            if (moved < 1000)
               Fail(Formatter::Format(_T("GetCurrentUtcTimestamp moved {0} ms in a transaction that took 1500 ms between two statements."), moved));

            String transactionSaid = Formatter::Format(_T("inside a transaction it moved {0} ms in 1500"), moved);
            if (dialect_.Is(DatabaseSettings::TypePGServer))
            {
               const __int64 stood = Value_(Formatter::Format(between, dialect_.MillisecondsBetween(_T("a.probestamp"), _T("b.probestamp")), 15, 16), "milliseconds");
               if (stood != 0)
                  Fail(Formatter::Format(_T("now() at time zone 'utc' moved {0} ms inside a transaction, where it is the transaction's start."), stood));

               transactionSaid += Formatter::Format(_T(", where now() moved {0}"), stood);
            }

            report_(Formatter::Format(_T("clock: UTC {0} s from this machine's UTC clock, an hour ahead {1} s; "), utc, ahead) + localSaid + _T("; ") + transactionSaid);
         }

         void CompactUtcFunction_()
         {
            // Measured, not judged: GetCurrentUtcTimestamp gives this machine's UTC
            // clock as a literal when the provider has no function of its own.
            const String functions[] = { _T("GETUTCDATE()"), _T("SYSUTCDATETIME()"), _T("SYSDATETIME()"), _T("CURRENT_TIMESTAMP") };
            String said;
            for (const String &function : functions)
            {
               String error;
               const bool answered = side_->TryExecute(SQLCommand(_T("select ") + function + _T(" as clockvalue from aaclaimlock")), error, 0, 0) == DALConnection::DALSuccess;
               said += (said.IsEmpty() ? String() : String(_T(", "))) + function + (answered ? _T(" answered") : _T(" refused"));
            }

            report_(_T("SQL Server Compact's own clock functions: ") + said + _T(" (GetCurrentUtcTimestamp writes this machine's UTC clock)"));
         }

         std::shared_ptr<DatabaseSettings> settings_;
         Dialect dialect_;
         DatabaseClaimTester::Report report_;
         bool provoke_errors_;
         String engine_version_;
         DatabaseConnectionManager pool_;
         std::shared_ptr<DALConnection> side_;
         Epochs epochs_;
      };
   }

   void
   DatabaseClaimTester::Test(std::shared_ptr<DatabaseSettings> settings, Report report, bool provokeErrors)
   {
      if (!settings)
         Fail(_T("No database was given."));

      switch (settings->GetType())
      {
      case DatabaseSettings::TypePGServer:
      case DatabaseSettings::TypeMYSQLServer:
      case DatabaseSettings::TypeMSSQLServer:
      case DatabaseSettings::TypeMSSQLCompactEdition:
         break;
      default:
         Fail(_T("The database's type is none of the four the server knows."));
      }

      Run run(settings, report, provokeErrors);
      run.All();
   }

#ifndef HM_PLATFORM_POSIX
   void
   DatabaseClaimTester::TestCompact()
   {
      const Report report = [](const String &line)
      {
         LOG_APPLICATION(_T("DatabaseClaimTester: ") + line);
         OutputDebugString((_T("hMailServer: DatabaseClaimTester: ") + line + _T("\n")).c_str());
      };

      const String directory = IniFileSettings::Instance()->GetTempDirectory() + FileUtilities::PathSeparator + _T("DatabaseClaimTest-") + GUIDCreator::GetGUID();
      if (!FileUtilities::CreateDirectory(directory))
         Fail(_T("The tester's directory could not be created under the temp folder."));

      try
      {
         // A Compact database of the tester's own, made as the server makes the
         // internal one, in this directory and never the bench's.
         const String name = _T("DatabaseClaimTest");
         const String password = PasswordGenerator::Generate();
         String error;
         if (!SQLCEConnection::CreateDatabase(directory, name, password, error))
            Fail(_T("The tester's Compact database could not be created: ") + error);

         const String empty;
         std::shared_ptr<DatabaseSettings> compact(new DatabaseSettings(empty, empty, name, empty, password, directory, empty, DatabaseSettings::TypeMSSQLCompactEdition, 0));
         Test(compact, report, false);
      }
      catch (...)
      {
         FileUtilities::DeleteDirectory(directory, true);
         throw;
      }

      FileUtilities::DeleteDirectory(directory, true);

      // The server's own database when it is SQL Server: the half of SQL Server that
      // waits for a bench on it (ActiveActive.md, D21). Tables of the tester's own.
      IniFileSettings *ini = IniFileSettings::Instance();
      if (ini->GetDatabaseType() == DatabaseSettings::TypeMSSQLServer)
      {
         std::shared_ptr<DatabaseSettings> server(new DatabaseSettings(ini->GetDatabaseProvider(), ini->GetDatabaseServer(), ini->GetDatabaseName(),
            ini->GetUsername(), ini->GetPassword(), ini->GetDatabaseDirectory(), ini->GetDatabaseServerFailoverPartner(), ini->GetDatabaseType(), ini->GetDatabasePort()));
         Test(server, report, false);
      }
   }
#endif
}
