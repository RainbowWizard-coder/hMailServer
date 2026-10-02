// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The never-ban list by reverse name (Common/Util/NeverBanNameList.h, GitLab
// work item #56) as a program of its own, for the POSIX build: the counterpart
// of public-suffix-tests. On Windows NeverBanNameListTester runs inside the
// service with the other class tests (ClassTester::DoTests, reached through
// Utilities.RunTestSuite); that route is a COM method and does not exist here,
// so the CMake build makes this and linux-build.sh runs it with every other
// *-tests program.
//
// It needs no database, no configuration, no file and no network: the public
// suffix list is compiled in, and every forward lookup is answered by the
// tester's own zone.
//
// It prints one line and exits non-zero on the first failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/Util/NeverBanNameList.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   try
   {
      HM::NeverBanNameListTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the never-ban list by reverse name: %s\n", failure.what());
      return 1;
   }

   printf("ok    the never-ban list by reverse name: patterns held and refused (public suffixes, addresses, stars), label boundaries, usable reverse names, the most specific pattern, forward confirmation in both families and for a mapped address, failed lookups never taken as answers, the bound on lookups, and the verdicts kept and forgotten\n");
   return 0;
}
