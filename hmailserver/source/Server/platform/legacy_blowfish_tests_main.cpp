// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The legacy Blowfish scheme's tester as a program of its own, for the POSIX
// build (Common/Util/LegacyBlowfish.h).
//
// On Windows the same tester runs inside the shipping server: ClassTester::
// DoTests calls LegacyBlowfishTester::Test, and the regression suite reaches it
// through Utilities.RunTestSuite. That route does not exist here, so the CMake
// build makes this instead and GitLab CI's linux-build jobs run it
// (build/ci/linux-build.sh) under clang and GCC. Here is where the cipher's
// 32-bit words meet an LP64 machine, whose unsigned long is 64 bits.
//
// What it holds the scheme to: Blowfish against Eric Young's published test
// vectors, and the scheme against values hMailServer 6.3.4 wrote and read.
// Nothing here touches the network, the configuration or a database.
//
// One line per failure, and a non-zero exit when there is any.

#include <cstdio>

#include "../Common/Util/LegacyBlowfish.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   HM::LegacyBlowfishTester tester;
   const std::vector<HM::AnsiString> failures = tester.Run();

   if (failures.empty())
   {
      printf("ok    the legacy Blowfish scheme (Eric Young's vectors, and the values 6.3.4 wrote and read)\n");
      return 0;
   }

   printf("FAIL  the legacy Blowfish scheme: %d\n", static_cast<int>(failures.size()));
   for (size_t i = 0; i < failures.size(); i++)
      printf("        %s\n", failures[i].c_str());

   return 1;
}
