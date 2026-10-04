// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The quadratic-time sweep of 6.3.6 (Common/Util/InputCostTester.h) as a
// program of its own for the POSIX build.
//
// On Windows the same tests run inside the shipping server: ClassTester::
// DoTests calls them, and the regression suite reaches it through
// Utilities.RunTestSuite. That route does not exist here, so the CMake build
// makes this instead and linux-build.sh runs it with every other *-tests
// program. Each test holds a reader of what a remote party sends to what
// 6.3.5's code made of the same generated cases and to a single pass's time
// over the input that was that code's worst. Nothing here touches the network,
// the configuration or a database.
//
// One line a test, and a non-zero exit on the first failure. A test's name as
// the argument runs that test alone.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>

#include "../Common/Mime/MimeCode.h"
#include "../Common/Util/InputCostTester.h"

namespace
{
   struct Test_
   {
      const char *name;
      const char *what;
      std::function<void()> run;
   };
}

int
main(int argc, char *argv[])
{
   // Each line as it is written, so a run cut short still says how far it got.
   (void) setvbuf(stdout, NULL, _IOLBF, 0);

   // What Application::InitInstance sets before the server reads any mail.
   HM::MimeEnvironment::SetAutoFolding(true);

   const Test_ tests[] =
   {
      { "words", "encoded words of another encoding: decoded as 6.3.5 did, and in one pass", []() { HM::InputCostTester::TestEncodedWords(); } },
      { "fields", "header fields deleted by name or position: as 6.3.5 did, and in one pass", []() { HM::InputCostTester::TestFieldDeletion(); } },
      { "dkim", "DKIM and ARC header fields chosen: as 6.3.5 did, and in one pass", []() { HM::InputCostTester::TestDkimHeaderSelection(); } },
      { "sieve", "Sieve :matches: as 6.3.5 matched, in the value's length times the pattern's", []() { HM::InputCostTester::TestSieveMatches(); } },
      { "spf", "SPF names cut to 253 characters: as 6.3.5 cut them, and in one pass", []() { HM::InputCostTester::TestSpfTruncation(); } },
      { "parameters", "MIME parameters removed: as 6.3.5 removed them, and in one pass", []() { HM::InputCostTester::TestParameterRemoval(); } },
      { "list", "LIST lines each once: as 6.3.5 took them, and in one pass", []() { HM::InputCostTester::TestListLines(); } },
      { "json", "JSON members, the first of a name kept: as 6.3.5 kept them, and in one pass", []() { HM::InputCostTester::TestJsonMembers(); } },
      { "calendar", "an organiser's CANCEL over a long RDATE, many overrides and a zone of many transitions: in one pass", []() { HM::InputCostTester::TestCalendarCancel(); } },
   };

   for (const Test_ &test : tests)
   {
      if (argc > 1 && std::strcmp(argv[1], test.name) != 0)
         continue;

      const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
      try
      {
         test.run();
      }
      catch (const std::exception &failure)
      {
         printf("FAIL  %s: %s\n", test.what, failure.what());
         return 1;
      }

      const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      printf("ok    %s (%.1f s)\n", test.what, seconds);
   }

   return 0;
}
