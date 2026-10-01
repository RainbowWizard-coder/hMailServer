// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The setting profiles' lock (Common/Application/Profiles.cpp, profiles_mutex)
// held to refusing a verb asked for on the thread that holds it, as a program of
// its own for the POSIX build - the counterpart of spf-tests and backup-tests. On
// Windows ProfilesTester runs inside the service with the other class tests
// (ClassTester, which Utilities.RunTestSuite reaches).
//
// An apply or a revert writes settings while it holds the lock, and a write can run
// an event script on the same thread - an error reported fires OnError, and a
// revert that writes the scripting switch on runs the script's top level. A script
// that then applied, reverted, discarded or declined a profile locked the mutex a
// second time on the thread that held it: glibc's std::mutex waits for ever there,
// and MSVC's throws resource_deadlock_would_occur. Up to 6.3.5;
// from 6.3.6 the second call is refused with the reason. The tester holds the lock
// as such an apply does and calls each verb, which must answer at once; a build
// without the refusal stops at the first call, which linux-build.sh's timeout
// turns into a failure. No database, configuration or network.
// One line either way, and a non-zero exit on a failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/Application/Profiles.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   try
   {
      HM::ProfilesTester tester;
      tester.Test();
      printf("ok    the profiles' lock: each verb asked for on the thread holding it refused at once, another thread not, and the lock free afterwards\n");
      return 0;
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  %s\n", failure.what());
      return 1;
   }
}
