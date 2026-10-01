// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The Control Deck's script-src hash's tester as a program of its own, for the
// POSIX build - the counterpart of spf-tests and xml-tree-tests.
//
// On Windows InlineScriptHashTester runs inside the service with the other
// class tests: ClassTester::DoTests calls it and the regression suite reaches
// it through Utilities.RunTestSuite. That route does not exist here - it is a
// COM method, and the Linux suite's shim skips it - so the CMake build makes
// this, and linux-build.sh runs it with every other *-tests program. It needs
// no database, no configuration, no file and no network.
//
// It prints one line either way and exits non-zero on the first failure.
//
// Given a page's path instead - inline-script-hash-tests WebAdmin/index.html -
// it prints the script-src the server sends that page under, so the hash can be
// put beside the one a browser's console names when it refuses a script: a
// page a proxy rewrote on its way through, for one.

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "../Common/Util/InlineScriptHash.h"

int
main(int argc, char *argv[])
{
   if (argc == 2)
   {
      std::ifstream stream(argv[1], std::ios::binary);
      if (!stream)
      {
         printf("cannot read %s\n", argv[1]);
         return 2;
      }
      const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
      const HM::AnsiString source = HM::InlineScriptHash::ScriptSource(HM::AnsiString(bytes));
      printf("script-src %s\n", source.c_str());
      return 0;
   }

   try
   {
      HM::InlineScriptHashTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the Control Deck's script hash: %s\n", failure.what());
      return 1;
   }

   printf("ok    the Control Deck's script hash: the text a browser hashes, CR LF made LF, found as the tokenizer finds it, 'none' for a page with no single inline script, once per distinct page\n");
   return 0;
}
