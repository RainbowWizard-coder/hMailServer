// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The JavaScript event-script binding's tester as a program of its own, for the
// POSIX build - the counterpart of spf-tests.
//
// This program is the only thing that runs ScriptBindingTester. ClassTester::DoTests
// is reached through COM (Utilities.RunTestSuite) and the Windows /Test switch, and
// the engine is not on Windows; the Linux regression suite has no COM and does not
// run this program either. So the CMake build makes it, GitLab CI's linux-build job
// runs it beside spf-tests, and on a machine that CI does not reach it is run by
// hand from the build directory: ./script-binding-tests. It needs
// no database, no configuration and no network: it makes the objects a handler
// is given in memory and runs scripts against them, checks every member the
// binding's contract names (Common/Scripting/ScriptBinding.cpp, and the table in
// hmailserver/docs/EventScripts.md), and holds the engine to its limits - the
// timeout, the memory ceiling, the missing std and os modules.
//
// It prints one line either way and exits non-zero on the first failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/Scripting/ScriptBinding.h"
#include "../Common/Scripting/ScriptServer.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   try
   {
      HM::ScriptBindingTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the JavaScript event-script binding: %s\n", failure.what());
      return 1;
   }

   // The Windows engines' watchdog is not on this platform, and the bound its due
   // time is clamped to is the same here: ScriptWatchdogTester, which ClassTester
   // runs on Windows.
   try
   {
      HM::ScriptWatchdogTester watchdog;
      watchdog.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the event-script watchdog's due time: %s\n", failure.what());
      return 1;
   }

   printf("ok    the JavaScript event-script binding: the contract, values both ways, the timeout, the memory ceiling and the sandbox; the watchdog's due time\n");
   return 0;
}
