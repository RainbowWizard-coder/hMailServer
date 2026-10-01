// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The account-signature sanitiser's tester as a program of its own, for the
// POSIX build - the counterpart of spf-tests and inline-script-hash-tests.
//
// On Windows SignatureHtmlSanitizerTester runs inside the service with the other
// class tests: ClassTester::DoTests calls it and the regression suite reaches it
// through Utilities.RunTestSuite. That route does not exist here - it is a COM
// method, and the Linux suite's shim skips it - so the CMake build makes this,
// and linux-build.sh runs it with every other *-tests program. It needs no
// database, no configuration, no file and no network.

#include <cstdio>
#include <stdexcept>

#include "../Common/Util/SignatureHtmlSanitizer.h"

int
main()
{
   try
   {
      HM::SignatureHtmlSanitizerTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the account signature sanitiser: %s\n", failure.what());
      return 1;
   }

   printf("ok    the account signature sanitiser: the subset the webmail's cleaner admits, a dangerous element dropped whole, http(s)/mailto and data: image only, a fixed point on the page's own output, idempotent\n");
   return 0;
}
