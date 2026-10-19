// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The PostgreSQL connection string (PGConnection::MakeConninfo) held to libpq's
// own reading of it, as a program of its own for the POSIX build - the
// counterpart of spf-tests and backup-tests. On Windows PGConnectionTester runs
// inside the service with the other class tests (ClassTester, which
// Utilities.RunTestSuite reaches), beside ADOConnectionTester for SQL Server's.
//
// Up to 6.3.5 the server wrote the host, the user, the password and the database
// into the conninfo's quotes as they were, so a password with a single quote or a
// backslash in it ended its value, or took the next keyword into it, and DBSetup
// refused such a password rather than pass it on. The tester writes hostile
// values - quotes, backslashes, spaces, an equals sign, a keyword's text, text
// outside ASCII - into every value and has libpq's PQconninfoParse read each back:
// byte for byte, and nothing else moved. And MySQL's: the server, the user, the
// password and the database handed to the client in UTF-8, where up to 6.3.5 a
// value with any character outside ASCII reached it empty on Linux, and the
// database's name quoted in the statements that name it, where a name MySQL takes
// only quoted refused every connection (MySQLConnectionTester). And the CREATE
// DATABASE that --create-database and COM's CreateExternalDatabase send, the name
// quoted in each dialect (SQLStatementTester::TestCreateDatabase). And the two
// readers of a statement's literals, the [IGNORE-ERRORS] marker's test and the
// masking of the slow-query line and the trace, each literal read as each engine
// reads it (SqlLiteralScanTester). No database, configuration or network. One
// line a tester, and a non-zero exit on a failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/SQL/DALConnection.h"
#include "../Common/SQL/PGConnection.h"
#include "../Common/SQL/MySQLConnection.h"
#include "../Common/SQL/SQLStatement.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   int result = 0;

   try
   {
      HM::PGConnectionTester tester;
      tester.Test();
      printf("ok    the PostgreSQL connection string: every hostile value read back by libpq as given, nothing else moved\n");
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  %s\n", failure.what());
      result = 1;
   }

   try
   {
      HM::MySQLConnectionTester tester;
      tester.Test();
      printf("ok    MySQL: every value handed to the client in UTF-8, half a surrogate pair refused, the database's name quoted\n");
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  %s\n", failure.what());
      result = 1;
   }

   try
   {
      HM::SQLStatementTester tester;
      tester.TestCreateDatabase();
      printf("ok    CREATE DATABASE: the name quoted in each dialect, its closing quote doubled\n");
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  %s\n", failure.what());
      result = 1;
   }

   try
   {
      HM::SqlLiteralScanTester tester;
      tester.Test();
      printf("ok    a statement's literals: read as each engine reads them by the marker's test and the masking\n");
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  %s\n", failure.what());
      result = 1;
   }

   return result;
}
