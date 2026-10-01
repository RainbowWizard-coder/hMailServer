// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// What the update check keeps of a release's page, as a program of its own for
// the POSIX build.
//
// On Windows UpdateCheckerTester runs inside the server: ClassTester::DoTests
// calls it, and the regression suite reaches it through Utilities.RunTestSuite.
// Nothing reaches ClassTester here, so the CMake build makes this program and
// GitLab CI's linux-build job runs it with the other *-tests programs
// (build/ci/linux-build.sh).
//
// UpdateCheckerTester (Common/Util/UpdateChecker.cpp) reads feed documents
// whose html_url is the project's release page, an https address in capitals,
// the longest address kept and one character longer, and values that are not an
// https address with a host - another scheme, a leading space or tab, a space,
// line break, NUL or DEL inside, no host, a number, none - and requires the
// first kind kept as it is, the rest dropped, and the release read either way.
// No network, no configuration. It throws at its first failed check; this
// prints which, and exits non-zero.

#include <cstdio>
#include <exception>

#include "../Common/Util/UpdateChecker.h"

int main()
{
   try
   {
      HM::UpdateCheckerTester tester;
      tester.Test();
      std::printf("ok    UpdateCheckerTester\n");
      return 0;
   }
   catch (const std::exception &e)
   {
      std::printf("FAIL  UpdateCheckerTester: %s\n", e.what());
      return 1;
   }
}
