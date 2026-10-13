// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The quarantine search's testers as a program of their own, for the POSIX
// build (Common/AntiSpam/QuarantineStore.h, Common/SQL/SQLStatement.h).
//
// On Windows the same testers run inside the shipping server: ClassTester::
// DoTests calls QuarantineSearchTester::Test and SQLStatementTester::Test, and
// the regression suite reaches them through Utilities.RunTestSuite. That route
// does not exist here, so the CMake build makes this instead and
// linux-build.sh runs it with every other *-tests program.
//
// What it holds the search to (review finding F261): the text a LIKE is to
// find is written with every wildcard of the four databases escaped and reads
// back as itself (SQLStatement::EscapeLikeText); and the two statements a
// search sends name each parameter once, in the order SQL Server binds them by
// position, none the start of another, lower both sides, leave the recipients
// out of a search confined to one recipient and find that recipient whole, and
// page newest first with the id breaking a tie. Nothing here touches the
// network, the configuration or a database.
//
// One line a tester, and a non-zero exit on a failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/SQL/SQLStatement.h"
#include "../Common/AntiSpam/QuarantineStore.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   int result = 0;

   try
   {
      HM::SQLStatementTester tester;
      tester.TestEscapeLikeText();
      printf("ok    a LIKE's text: %%, _ and [ written after the escape character, which is doubled, and every text read back as itself\n");
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  %s\n", failure.what());
      result = 1;
   }

   try
   {
      HM::QuarantineSearchTester tester;
      tester.Test();
      printf("ok    the quarantine search: each parameter once and in order, the text escaped and lowered, a recipient found whole and not searched in, the page newest first\n");
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  %s\n", failure.what());
      result = 1;
   }

   return result;
}
