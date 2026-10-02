// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Who may push whom out of a full table of sign-in state
// (Common/Util/SignInBudget.h) as a program of its own, for the POSIX build: the
// counterpart of never-ban-tests. On Windows SignInBudgetTester runs inside the
// service with the other class tests (ClassTester::DoTests, reached through
// Utilities.RunTestSuite); that route is a COM method and does not exist here,
// so the CMake build makes this and linux-build.sh runs it with every other
// *-tests program.
//
// It needs no database, no configuration, no file and no network: the rule is
// asked of tables built in memory, and the sources of addresses written out.
// PasskeyStoreTester's challenge tables are in memory too and run here as well,
// since 6.3.6's re-check put the registrations under the rule beside the
// sign-ins.
//
// It prints one line and exits non-zero on the first failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/Util/PasskeyStore.h"
#include "../Common/Util/SignInBudget.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   try
   {
      HM::SignInBudgetTester tester;
      tester.Test();

      HM::PasskeyStoreTester passkeys;
      passkeys.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the sign-in tables' room: %s\n", failure.what());
      return 1;
   }

   printf("ok    the sign-in tables' room: a holder at its cap gives up its own oldest, a full table the oldest of the holder with the most, of the largest group among holders tied, and never an administrator's session; floods from one source, from many and from one /48 leave another's pending sign-in, passkey registration and session; sources by address, an IPv6 address by its /64, X-Forwarded-For believed from this machine only, only its last address, and never one on this machine\n");
   return 0;
}
