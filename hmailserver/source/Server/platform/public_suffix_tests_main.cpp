// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The Public Suffix List's cut of a host name - the name the SURBL lookups ask
// about and the MX-record spam test falls back to - as a program of its own, for
// the POSIX build: the counterpart of spf-tests and xml-tree-tests.
//
// On Windows PublicSuffixListTester runs inside the service with the other class
// tests: ClassTester::DoTests calls it and the regression suite reaches it
// through Utilities.RunTestSuite. That route does not exist here - it is a COM
// method, and the Linux suite's shim skips it - so the CMake build makes this,
// and linux-build.sh runs it with every other *-tests program. It needs no
// database, no configuration, no file and no network: the list is compiled in.
//
// It prints one line either way and exits non-zero on the first failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/AntiSpam/DMARC/PublicSuffixList.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   try
   {
      HM::PublicSuffixListTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the Public Suffix List's cut: %s\n", failure.what());
      return 1;
   }

   printf("ok    the Public Suffix List's cut: hosts, addresses, suffixes, wildcard and exception rules, PRIVATE names, and DMARC's answer for each\n");
   return 0;
}
