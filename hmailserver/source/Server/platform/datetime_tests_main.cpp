// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The date class's tests, and those of the date parsers built on it, as a program
// of their own for the POSIX build.
//
// On Windows both testers run inside the server: ClassTester::DoTests calls them,
// and the regression suite reaches it through Utilities.RunTestSuite. Nothing
// reaches ClassTester here, so the CMake build makes this program and GitLab CI's
// linux-build job runs it with the other *-tests programs
// (build/ci/linux-build.sh).
//
// DateTimeTests (Common/Util/DateTime.cpp) holds HM::DateTime to known OLE
// Automation dates, the rounding to the second, the range read back, the leap
// years, arithmetic and comparison across day 0 and the seconds since 1970.
// TimeTester (Common/Util/Time.cpp) holds the MIME, IMAP and system-date parsers
// to their examples. Neither reads the clock, the time zone or the configuration.
// Each throws at its first failed check; this prints which, and exits non-zero.

#include <cstdio>
#include <exception>

#include "../Common/Util/VariantDateTime.h"
#include "../Common/Util/Time.h"

namespace
{
   template <class Tester>
   int Run(const char *name)
   {
      try
      {
         Tester tester;
         tester.Test();
         std::printf("ok    %s\n", name);
         return 0;
      }
      catch (const std::exception &e)
      {
         std::printf("FAIL  %s: %s\n", name, e.what());
         return 1;
      }
   }
}

int main()
{
   int failed = 0;
   failed += Run<HM::DateTimeTests>("DateTimeTests");
   failed += Run<HM::TimeTester>("TimeTester");

   return failed == 0 ? 0 : 1;
}
