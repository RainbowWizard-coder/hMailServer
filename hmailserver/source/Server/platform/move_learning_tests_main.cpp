// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Learning from the Junk folder, its rules, as a program of its own, for the
// reason spf-tests is one: MoveLearningTester::TestRules
// (Common/AntiSpam/MoveLearningTester.h), which on Windows runs inside the server
// through ClassTester, which nothing reaches here. Which copies are lessons and
// which way - into Junk spam, out of it ham, nothing between two ordinary
// folders or two Junk folders, nothing for a public folder, another account's
// folder or a copy with no owner - for SpamAssassin and for Rspamd, each switch
// off, a COPY with MoveLearningFromCopy on and off, a delegate's move; which
// folder is Junk beside a Spam folder; the request a lesson to Rspamd makes and
// the settings that refuse one; what each of the controller's answers means;
// and the lesson queue's bound.
//
// A scratch configuration directory of its own under the system's temp
// directory (the Logger and the ini singletons want one), removed at the end.
// No database, no socket and no network, so the source packages' builds run it.
// One line either way; a non-zero exit on failure. Well under a second.

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
      printf("FAIL  learning from the Junk folder: %s\n", why);
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
      ("hm-move-learning-tests-" + std::to_string((long long) ::getpid()));
   if (!error)
      boost::filesystem::create_directories(scratch, error);
   if (error)
   {
      printf("FAIL  learning from the Junk folder: no scratch directory could be made under the system's temp directory\n");
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
      tester.TestRules();
   }
   catch (const std::exception &failure)
   {
      return Fail(scratch, failure.what());
   }

   boost::filesystem::remove_all(scratch, error);

   printf("ok    learning from the Junk folder: into Junk spam and out of it ham, for SpamAssassin and Rspamd; nothing between two ordinary or two Junk folders, "
          "for a public folder, another account's folder, a switch off, or a COPY while MoveLearningFromCopy is off; a delegate's move teaches SpamAssassin as before "
          "and Rspamd nothing; Junk beside Spam is the Junk folder unless Spam is designated; the Rspamd request carries the password, the classifier and the owner "
          "alone; each answer of the controller read; the lesson queue holds its bound\n");
   return 0;
}
