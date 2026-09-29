// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The backup index's XML tree's tester as a program of its own, for the POSIX
// build - the counterpart of spf-tests and snmp-tests.
//
// On Windows XmlTreeTester runs inside the service with the other class tests:
// ClassTester::DoTests calls it and the regression suite reaches it through
// Utilities.RunTestSuite. That route does not exist here - it is a COM method,
// and the Linux suite's shim skips it - so the CMake build makes this, and
// linux-build.sh runs it with every other *-tests program. It needs no
// database, no configuration, no file and no network: it writes trees and
// reads texts in memory - the index's shape byte for byte, every character
// there is on this platform (wchar_t is 32 bits here, so every code point, not
// only the first 65,535) written and read back, the forms earlier versions
// wrote and read, and nesting a hundred thousand deep refused.
//
// It prints one line either way and exits non-zero on the first failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/Util/XmlTreeTester.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   try
   {
      HM::XmlTreeTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the backup index's XML: %s\n", failure.what());
      return 1;
   }

   printf("ok    the backup index's XML: written as every earlier version wrote it, every character read back, the lenient forms read and the malformed refused\n");
   return 0;
}
