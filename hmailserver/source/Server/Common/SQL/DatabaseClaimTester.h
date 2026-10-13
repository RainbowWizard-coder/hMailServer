// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <functional>

#include "DatabaseSettings.h"

namespace HM
{
   /*
      The database layer's row counts and its UTC clock, held to what ActiveActive.md
      builds on (decisions D1 and D3), on a real database through the server's own
      connections - the engine checks of build/lab/active-active/sqlcheck.py, made
      through DALConnection and DatabaseConnectionManager instead of a client of
      their own:

        counts     ExecuteCount (DatabaseConnectionManager), Execute and TryExecute
                   (DALConnection): an UPDATE matching no row 0, one row 1, a row it
                   matches and leaves as it was 1 (MySQL and MariaDB count it only
                   with CLIENT_FOUND_ROWS), two rows 2; an INSERT 1; a DELETE 1, and 0
                   when there is nothing to delete; an INSERT a unique key refuses,
                   with that failure ignored, 0; a statement that fails -1.
        claims     a conditional UPDATE that also matches its own epoch counts 1 when
                   it is run a second time, as DALConnection::Execute runs it after a
                   reconnect; without that match the second run counts 0 (the
                   negative control); another attempt's epoch counts 0.
        races      two connections of their own, as two nodes are, claim the same
                   rows (... and owner = 0) and take the same named leases by
                   insert-or-claim on names that have no row yet, each judged by its
                   count: every row and every name won exactly once.
        reconnect  a connection closed under the statement: Execute runs it on a new
                   one, counts it and says so, and the claim run again on purpose
                   still counts 1. On PostgreSQL and MySQL the database also ends the
                   pool's own session (pg_terminate_backend, KILL), as a restart or a
                   failover of the database ends it, and then closes it for being
                   idle (idle_session_timeout, wait_timeout), and ExecuteCount says
                   the same each time - over TLS too, where MySQL and MariaDB answer
                   with the TLS layer's error (MySQLConnection::CheckError).
        leases     a named lease taken by insert-or-claim where no row exists yet,
                   refused to another epoch before it expires, renewed by its holder,
                   taken by another once it has expired, and its old holder fenced -
                   on the UTC clock.
        clock      GetCurrentUtcTimestamp written into a column reads back as this
                   machine's own UTC clock, within five minutes, while
                   GetCurrentTimestamp reads back as the session's local time: on
                   PostgreSQL and MySQL the session is put at UTC+13 first, so a UTC
                   function that were local time would be thirteen hours out.
                   GetCurrentUtcTimestampPlusSeconds is that much ahead of it, and the
                   UTC clock moves inside a transaction (PostgreSQL's now(), the
                   transaction's start, stands still there: the negative control).
        Compact    whether SQL Server Compact's provider has a UTC function, said and
                   not judged (GetCurrentUtcTimestamp gives the node's own clock when
                   it has none, as 4.0 has not).

      On a pool and connections of its own, over two tables of its own (aaclaimprobe,
      aaclaimlock) that it makes, empties and removes - never a table of the
      server's - so it can be run against a database a server has made, before the
      server starts on it (build/ci/linux-suite.sh, build/lab/db-suite.sh). It ends
      the sessions of its own pool and none other. Two of it at once on one database
      would share those tables.

      Two callers. On Windows, ClassTester (Utilities.RunTestSuite) through
      TestCompact: a SQL Server Compact database of its own in the temp folder,
      which is what the Windows gate's engine is - and, when the server's own
      database is SQL Server, the same checks on it as well, which is the half of
      SQL Server that waits for a bench on SQL Server (ActiveActive.md, D21). On
      Linux, the program database-claim-tests (platform/database_claim_tests_main.cpp)
      against the database a configuration file names: PostgreSQL in the Linux
      suite, MySQL and MariaDB in the database matrix. It says what failed by
      throwing std::logic_error with a sentence.
   */
   class DatabaseClaimTester
   {
   public:
      typedef std::function<void(const String &line)> Report;

      // Every check on the database the settings name, one line to report for each.
      // provokeErrors: whether a failing statement may also go through ExecuteCount,
      // which reports it to the error log as any failed statement is (HM5032) - not
      // inside the running server, where ClassTester runs and a fixture asserts the
      // error log empty; there a failure is checked through TryExecute, which reports
      // nothing.
      void Test(std::shared_ptr<DatabaseSettings> settings, Report report, bool provokeErrors);

#ifndef HM_PLATFORM_POSIX
      // ClassTester's: a SQL Server Compact database of the tester's own, made in
      // the temp folder and removed again, and the server's own database too when
      // it is SQL Server. What it measures goes to the application log and the
      // debug output.
      void TestCompact();
#endif
   };
}
