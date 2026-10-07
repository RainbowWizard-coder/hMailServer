// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The HTTP listeners' deadlines as a program of its own, for the POSIX build -
// the counterpart of spf-tests and network-deadline-tests, which linux-build.sh
// runs with every other *-tests program.
//
// On Windows HttpServerTester runs inside the service with the other class
// tests: ClassTester::DoTests calls it and the regression suite reaches it
// through Utilities.RunTestSuite. That route does not exist here - it is a COM
// method, and the Linux suite's shim skips it - so the CMake build makes this.
//
// What it holds, each case against an HttpServer of its own on 127.0.0.1 with
// a request deadline of one second (Common/Util/HttpServer.h says why each):
//   - an answer of 300,000 bytes that its handler returns after the request
//     deadline arrives whole - until 7 October 2026 the expiry that came while
//     the handler ran closed the connection behind the first 64 KB, which is
//     how a webmail search that took longer than thirty seconds answered
//     65,413 bytes of its 67,996;
//   - the same when it is the connection's own deadline that runs out while
//     the handler runs, with the connection closed after the answer;
//   - a reader that stops reading part way through a 16 MB answer is reset
//     after the request deadline, never handed an orderly end of the
//     connection it could take for the end of the answer;
//   - a kept-alive connection answers the next request after a late answer.
//
// No database, no configuration, no file, no connection but to 127.0.0.1, no
// root. It needs the loopback, so it cannot run where sockets are refused
// altogether: the source packages' builds run under
// build/ci/packages/offline.c, and their tester lists (debian/rules,
// hmailserver.spec) leave it out. About ten seconds, most of them handlers
// sleeping past their deadline.
//
// It prints one line either way and exits non-zero on the first failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/Util/HttpServer.h"

int
main()
{
   try
   {
      HM::HttpServerTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the HTTP listeners' deadlines: %s\n", failure.what());
      return 1;
   }

   printf("ok    the HTTP listeners' deadlines: an answer its handler returns after the request deadline, or after the connection's, arrives whole; a reader that stops part way is reset; a kept-alive connection answers on after a late answer\n");
   return 0;
}
