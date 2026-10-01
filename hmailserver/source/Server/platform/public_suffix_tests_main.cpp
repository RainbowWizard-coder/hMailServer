// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The Public Suffix List's cut of a host name - the name the SURBL lookups ask
// about and the MX-record spam test falls back to - as a program of its own, for
// the POSIX build: the counterpart of spf-tests and xml-tree-tests. With it, the
// SURBL test's reading of the links in a message, which hands the cut its hosts:
// the host after a link's userinfo, the forms of an address, the code points a
// browser deletes from a host, links with no scheme, HTML's character references,
// and the answers the lists publish as errors.
//
// On Windows PublicSuffixListTester and SURBLTester run inside the service with
// the other class tests: ClassTester::DoTests calls them and the regression suite
// reaches them through Utilities.RunTestSuite. That route does not exist here - it
// is a COM method, and the Linux suite's shim skips it - so the CMake build makes
// this, and linux-build.sh runs it with every other *-tests program. It needs no
// database, no configuration, no file and no network: the list is compiled in,
// and nothing is looked up.
//
// It prints one line for each tester and exits non-zero on the first failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/AntiSpam/DMARC/PublicSuffixList.h"
#include "../Common/AntiSpam/SURBL.h"

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

   printf("ok    the Public Suffix List's cut: hosts, addresses, suffixes, wildcard and exception rules, PRIVATE names, DMARC's answer for each, and the cut by the ICANN section alone\n");

   try
   {
      HM::SURBLTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the SURBL test's links: %s\n", failure.what());
      return 1;
   }

   printf("ok    the SURBL test's links: the host after the userinfo, ports, IPv6 and the IPv4 addresses it carries, percent-encoding as UTF-8, the code points UTS 46 ignores, the forms of an address, links with no scheme, HTML's references and quotes, the names asked about, the lists' error codes, and the sender lists' URL entries read the same way and matched against every link\n");
   return 0;
}
