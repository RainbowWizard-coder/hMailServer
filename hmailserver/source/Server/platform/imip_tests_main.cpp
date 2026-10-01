// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// iMIP's tester as a program of its own, for the POSIX build
// (Common/Util/Imip.h).
//
// On Windows the same tester runs inside the shipping server: ClassTester::
// DoTests calls ImipTester::Test, and the regression suite reaches it through
// Utilities.RunTestSuite. That route does not exist here, so the CMake build
// makes this instead and linux-build.sh runs it with every other *-tests
// program.
//
// What it holds iMIP on delivery to: the From a header names - one field, one
// mailbox, read alike by the address list parser and the DMARC check - and
// DMARC for exactly that address's domain, which is what vouches that mail
// from outside was sent by an address before its REPLY, CANCEL or updated
// REQUEST changes a stored event; and what a REPLY or a CANCEL changes in a
// stored event given who sent it. With every case that must not change or
// vouch for anything.
// Nothing here touches the network, the configuration or a database.
//
// One line, and a non-zero exit on the first failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/Util/Imip.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   try
   {
      HM::ImipTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  iMIP on delivery: %s\n", failure.what());
      return 1;
   }

   printf("ok    iMIP on delivery: the one From a header names, DMARC for exactly its domain, and what a REPLY or a CANCEL changes given who sent it, with the cases that must not change or vouch for anything\n");
   return 0;
}
