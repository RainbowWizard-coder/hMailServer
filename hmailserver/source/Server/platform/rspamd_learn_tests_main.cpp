// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The request a lesson to Rspamd's controller is, as the server's own client
// sends it: MoveLearningTester::TestStandIn (Common/AntiSpam/MoveLearningTester.h),
// which on Windows runs inside the server through ClassTester. A stand-in
// controller on a loopback port of its own records each request and answers as
// Rspamd does - learned, already learned, a password refused - and one that
// takes the connection and never answers is given up on at the timeout.
//
// A scratch configuration directory of its own under the system's temp
// directory, removed at the end. No database and no network but the loopback,
// so the source packages' builds, which refuse every socket, leave it out of
// their lists, as they leave out http-server-tests. About three seconds, two of
// them the timeout. One line either way; a non-zero exit on failure.

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include <boost/filesystem.hpp>

#include "../Common/Application/IniFileSettings.h"
#include "../Common/AntiSpam/MoveLearningTester.h"

namespace
{
   int Fail(const boost::filesystem::path &scratch, const char *why)
   {
      printf("FAIL  the Rspamd lesson: %s\n", why);
      boost::system::error_code ignored;
      boost::filesystem::remove_all(scratch, ignored);
      return 1;
   }
}

int
main(int /*argc*/, char * /*argv*/[])
{
   boost::system::error_code error;
   const boost::filesystem::path scratch = boost::filesystem::temp_directory_path(error) /
      ("hm-rspamd-learn-tests-" + std::to_string((long long) ::getpid()));
   if (!error)
      boost::filesystem::create_directories(scratch, error);
   if (error)
   {
      printf("FAIL  the Rspamd lesson: no scratch directory could be made under the system's temp directory\n");
      return 1;
   }

   const boost::filesystem::path file = scratch / "hMailServer.ini";
   const std::string directory = scratch.string();
   {
      std::ofstream ini(file.string().c_str(), std::ios::out | std::ios::trunc);
      ini << "[Directories]\n"
          << "ProgramFolder=" << directory << "\n"
          << "DataFolder=" << directory << "\n"
          << "LogFolder=" << directory << "\n"
          << "TempFolder=" << directory << "\n"
          << "EventFolder=" << directory << "\n"
          << "DatabaseFolder=" << directory << "\n";
      if (!ini)
         return Fail(scratch, "the scratch hMailServer.ini could not be written");
   }

   HM::IniFileSettings::Instance()->SetInitializationFile(HM::String(file.string().c_str()));
   HM::IniFileSettings::Instance()->LoadSettings();

   try
   {
      HM::MoveLearningTester tester;
      tester.TestStandIn();
   }
   catch (const std::exception &failure)
   {
      return Fail(scratch, failure.what());
   }

   boost::filesystem::remove_all(scratch, error);

   printf("ok    the Rspamd lesson: POST /learnspam and /learnham to a stand-in controller, with the password, the classifier and the owner as Deliver-To "
          "and Rcpt, the whole message as message/rfc822; learned, already learned and a refused password read as such, the password in no outcome; "
          "a controller that never answers costs the timeout and no more\n");
   return 0;
}
