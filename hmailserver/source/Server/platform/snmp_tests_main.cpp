// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The SNMP agent's tester as a program of its own, for the POSIX build - the
// counterpart of spf-tests and script-binding-tests.
//
// On Windows SnmpTester runs inside the service with the other class tests:
// ClassTester::DoTests calls it and the regression suite reaches it through
// Utilities.RunTestSuite. That route does not exist here - it is a COM method,
// and the Linux suite's shim skips it - so the CMake build makes this, and
// GitLab CI's linux-build job runs it beside the other two. It needs no
// database, no configuration, no socket and no network: it makes an engine in
// memory, sets its clock, and feeds it requests it built and packets it broke -
// truncated at every length, cut short one element at a time inside true
// lengths, malformed in each way the decoder refuses, nested two hundred deep,
// too long, and turned over byte by byte thousands of times, the SNMPv3
// plaintext among them encrypted and signed so that the decoder behind the
// digest is reached. Built with -DHM_SANITIZE=ON (CMakeLists.txt) it runs under
// AddressSanitizer and UndefinedBehaviorSanitizer.
//
// It prints one line either way and exits non-zero on the first failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/Util/SnmpTester.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   try
   {
      HM::SnmpTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the SNMP agent: %s\n", failure.what());
      return 1;
   }

   printf("ok    the SNMP agent: BER and its bounds, the USM cryptography, SNMPv2c and SNMPv3 exchanges, malformed, cut and turned-over packets\n");
   return 0;
}
