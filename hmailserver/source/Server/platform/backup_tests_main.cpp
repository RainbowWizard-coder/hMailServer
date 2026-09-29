// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The backup's exclusions' tester as a program of its own, for the POSIX build -
// the counterpart of spf-tests and xml-tree-tests.
//
// On Windows BackupExecuterTester runs inside the service with the other class
// tests: ClassTester::DoTests calls it and the regression suite reaches it
// through Utilities.RunTestSuite. That route does not exist here - it is a COM
// method, and the Linux suite's shim skips it - so the CMake build makes this,
// and linux-build.sh runs it with every other *-tests program. It is the
// platform the defect was found on: the first real Linux live update, on 29
// September 2026, had its backup destination under /var/lib/hmailserver, the
// data directory, and every backup copied itself into itself until HM5014.
//
// It builds a scratch data directory under the system's temp directory and
// removes it again: a destination inside it reached through a symbolic link,
// the temp directory and the live update's folder left out of the copy; a
// restore that keeps the destination's archives, the temp directory and the
// archive being restored, and does not put an older archive's live update
// outcome back. No database, no configuration, no network.
//
// It prints one line either way and exits non-zero on the first failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/Application/BackupExecuter.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   try
   {
      HM::BackupExecuterTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the backup's exclusions: %s\n", failure.what());
      return 1;
   }

   printf("ok    the backup's exclusions: a destination inside the data directory, the temp directory and the live update's folder left out of the copy and left in place by a restore\n");
   return 0;
}
