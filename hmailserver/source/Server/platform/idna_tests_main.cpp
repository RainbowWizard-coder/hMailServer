// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// UTS 46 - the mapping, NFC, validity, bidi and joiner rules and Punycode a
// browser runs on a link's host, and which the SURBL test and the sender lists'
// URL entries read hosts with - held to Unicode's IdnaTestV2.txt, as a program
// of its own for the POSIX build: the counterpart of public-suffix-tests. On
// Windows IdnaTester runs inside the service with the other class tests
// (ClassTester::DoTests, which the regression suite reaches through
// Utilities.RunTestSuite); that route does not exist here, so the CMake build
// makes this and linux-build.sh runs it with every other *-tests program. It
// needs no database, configuration, file or network: the tables and the test
// lines are compiled in.
//
// It prints one line and exits non-zero on the first failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/Idna/Idna.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   HM::IdnaTester tester;
   try
   {
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  UTS 46: %s\n", failure.what());
      return 1;
   }

   printf("ok    UTS 46: IdnaTestV2.txt of Unicode 18.0.0, all %zu test lines - ToUnicode, ToASCII Nontransitional and Transitional with every check on, and ToASCII with the URL standard's flags, and ProcessLabels a label at a time - and Punycode against RFC 3492's samples, NFC, the URL standard's domain parser, a long label, a label as written beside the label it decodes to, and what the mapping writes each code point as, whether a domain may hold that, and its General_Category\n",
          tester.GetLinesRun());
   return 0;
}
