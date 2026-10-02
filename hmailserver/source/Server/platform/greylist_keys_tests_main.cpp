// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The greylisting key's tester as a program of its own, for the POSIX build
// (Common/SQL/Prerequisites/PreReqGreyListKeys.h).
//
// On Windows the same tester runs inside the shipping server: ClassTester::
// DoTests calls PreReqGreyListKeysTester::Test, and the regression suite
// reaches it through Utilities.RunTestSuite, where it also runs the pass and
// the 6107 script on a SQL Server Compact database of its own. That route does
// not exist here, so the CMake build makes this instead and linux-build.sh
// runs it with every other *-tests program; the pass against PostgreSQL and
// MariaDB is build/db-upgrade-gate.sh's.
//
// What it holds schema 6107's cutover to: the key every row is given is the
// one every new row is written with (GreyListTriplet::ComputeKey, against MD5
// vectors computed outside this code); a row without one is deleted when an
// address in it is the old columns' 200-character cut, on either side, and
// keyed otherwise; of a triplet's rows the most passed and then the oldest is
// kept; and the statement that writes the keys takes nothing that is not a key.
// Nothing here touches the network, the configuration or a database.
//
// One line, and a non-zero exit on the first failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/SQL/Prerequisites/PreReqGreyListKeys.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   try
   {
      HM::PreReqGreyListKeysTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  greylisting key (schema 6107): %s\n", failure.what());
      return 1;
   }

   printf("ok    greylisting key (schema 6107): the key new rows are written with, a 200-character cut deleted on either side and everything else keyed, the most passed and oldest copy kept, and no value that is not a key in the statement\n");
   return 0;
}
