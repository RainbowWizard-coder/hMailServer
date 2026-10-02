// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The DEFLATE streams' tester as a program of its own, for the POSIX build - the
// counterpart of spf-tests and signature-html-sanitizer-tests.
//
// On Windows DeflateStreamsTester runs inside the service with the other class
// tests: ClassTester::DoTests calls it and the regression suite reaches it through
// Utilities.RunTestSuite. That route does not exist here - it is a COM method, and
// the Linux suite's shim skips it - so the CMake build makes this, and
// linux-build.sh runs it with every other *-tests program. It needs no database,
// no configuration, no file and no network.

#include <cstdio>
#include <stdexcept>

#include "../Common/TCPIP/DeflateStreams.h"

int
main()
{
   try
   {
      HM::DeflateStreamsTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the DEFLATE streams: %s\n", failure.what());
      return 1;
   }

   printf("ok    the DEFLATE streams: a round trip, a stream hundreds of times its size inflated in pieces no larger than the limit and put back whole, what zlib holds after the input is all taken, a limit of nothing, a corrupt stream refused\n");
   return 0;
}
