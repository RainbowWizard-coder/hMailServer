// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The readers of the reports other servers send back - delivery status
// notifications (RFC 3464) and abuse feedback (RFC 5965) - as a program of
// their own for the POSIX build.
//
// On Windows the same testers run inside the shipping server: ClassTester::
// DoTests calls them, and the regression suite reaches it through
// Utilities.RunTestSuite. That route does not exist here, so the CMake build
// makes this instead and linux-build.sh runs it with every other *-tests
// program. Each reads real reports and malformed ones, and a report whose
// headers part begins with a great many blank lines within the time a pass
// over it takes. With them, how the MIME decoder they read a header field
// through unfolds its value - as 6.3.5 did, and in one pass - which on Windows
// MimeTester holds it to. Nothing here touches the network, the configuration
// or a database.
//
// One line a tester, and a non-zero exit on the first failure.

#include <cstdio>
#include <functional>
#include <stdexcept>

#include "../Common/Mime/MimeCode.h"
#include "../Common/Mime/MimeTester.h"
#include "../SMTP/DeliveryReportParser.h"
#include "../SMTP/FeedbackReportParser.h"

namespace
{
   bool Run_(const char *what, const std::function<void()> &test)
   {
      try
      {
         test();
      }
      catch (const std::exception &failure)
      {
         printf("FAIL  %s: %s\n", what, failure.what());
         return false;
      }

      printf("ok    %s\n", what);
      return true;
   }
}

int
main(int /*argc*/, char * /*argv*/[])
{
   // What Application::InitInstance sets before the server reads any mail: a
   // decoded header field is unfolded. Without it the field decoder would skip
   // the unfolding the abuse report tester holds to one pass.
   HM::MimeEnvironment::SetAutoFolding(true);

   bool passed =
      Run_("header field values: unfolded as 6.3.5 did, and in one pass", []() { HM::MimeTester::TestFieldUnfolding(); }) &&
      Run_("delivery status notifications: read, and in one pass", []() { HM::DeliveryReportParserTester().Test(); }) &&
      Run_("abuse feedback reports: read, and in one pass", []() { HM::FeedbackReportParserTester().Test(); });

   return passed ? 0 : 1;
}
