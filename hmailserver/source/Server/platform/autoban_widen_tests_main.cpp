// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// An auto-ban widened to its reverse-name neighbours (Common/Util/AutoBanWidener.h,
// GitLab work item #55) as a program of its own, for the POSIX build: the
// counterpart of never-ban-tests. On Windows AutoBanWidenerTester runs inside the
// service with the other class tests (ClassTester::DoTests, reached through
// Utilities.RunTestSuite); that route is a COM method and does not exist here, so
// the CMake build makes this and linux-build.sh runs it with every other
// *-tests program.
//
// It needs no database, no configuration, no file and no network: the public
// suffix list is compiled in, and every reverse and forward lookup is answered by
// the tester's own zone.
//
// It prints one line and exits non-zero on the first failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/Util/AutoBanWidener.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   try
   {
      HM::AutoBanWidenerTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  an auto-ban widened to its reverse-name neighbours: %s\n", failure.what());
      return 1;
   }

   printf("ok    an auto-ban widened to its reverse-name neighbours: the run of a shared registrable domain (not one label stripped, a private-section suffix a domain of its own), the stops below and above (a foreign domain, no name, an address for a name, a name the never-ban list covers, AutoBanNeverBan, an administrator's range, the /24's edges, 32 lookups each way), the banned address's own name forward-confirmed, a lookup not answered or the time run out leaving the single address, hostile names kept out of the log, and the shapes the firewall mirrors\n");
   return 0;
}
