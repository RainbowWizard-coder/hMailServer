// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The type a file sent as a link and a contact's photo go out under
// (Common/Util/InertMediaType.h), as a program of its own for the POSIX build.
//
// On Windows the same tester runs inside the shipping server: ClassTester::
// DoTests calls InertMediaTypeTester::Test, and the regression suite reaches it
// through Utilities.RunTestSuite. That route does not exist here, so the CMake
// build makes this instead and linux-build.sh runs it with every other *-tests
// program.
//
// What it holds the type to: every type on the list of those no browser runs,
// applies or renders as a page kept as the list writes it, and
// application/octet-stream for every JavaScript name browsers run, the types
// they apply or render, a list of types, whitespace inside a type, a control
// character or a byte outside ASCII, and a parameter other than one charset.
// Nothing here touches the network, the configuration or a database.
//
// One line, and a non-zero exit on the first failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/Util/InertMediaType.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   try
   {
      HM::InertMediaTypeTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the types files and photos go out under: %s\n", failure.what());
      return 1;
   }

   printf("ok    the types files and photos go out under: the list kept as written, and octet-stream for every JavaScript name, the types a browser applies or renders, a list of types, whitespace, control characters and a parameter other than a charset\n");
   return 0;
}
