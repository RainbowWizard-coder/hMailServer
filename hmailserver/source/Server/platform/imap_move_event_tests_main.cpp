// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// What a committed IMAP MOVE hands on, as a program of its own, for the reason
// spf-tests is one: IMAPMoveDispatchTester (IMAP/IMAPMoveDispatch.h), which on
// Windows runs inside the server through ClassTester, which nothing reaches
// here. It holds the rules of OnIMAPMessageMoved - raised for a MOVE or a UID
// MOVE between two of the session account's own folders, with the setting,
// scripting and the handler all on, once for each message whose original the
// expunge removed, and never for a COPY, a failed MOVE, a public folder or
// another account's folders - the bounded queue the events wait in (the newest
// dropped when it is full, by count or by bytes, a command's fields shared by its
// events and charged once, one drainer at a time), the call text the Windows
// engines are handed, and the values a handler is given; and here, where the
// event scripts are JavaScript, a handler called as the event calls it, reading
// every one of them.
//
// A scratch configuration directory of its own under the system's temp
// directory (the Logger and the ini singletons want one), made new by mkdtemp,
// never one that was there, and removed at the end; and the rule that makes it
// so held (tester_scratch.h: a directory, or a link to one, prepared where it is
// to be made is refused).
// No database and no network. One line either way; a non-zero exit on failure.
// Well under a second.

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include <boost/filesystem.hpp>

#include "tester_scratch.h"

#include "../Common/Application/IniFileSettings.h"
#include "../Common/Util/FileUtilities.h"
#include "../IMAP/IMAPMoveDispatch.h"

namespace
{
   int Fail(const boost::filesystem::path &scratch, const char *why)
   {
      printf("FAIL  the IMAP move event: %s\n", why);
      boost::system::error_code ignored;
      boost::filesystem::remove_all(scratch, ignored);
      return 1;
   }
}

int
main(int /*argc*/, char * /*argv*/[])
{
   boost::system::error_code error;
   const boost::filesystem::path scratch = HMTesterScratch::Make("hm-imap-move-event-tests", error);
   if (error)
   {
      printf("FAIL  the IMAP move event: no scratch directory could be made under the system's temp directory\n");
      return 1;
   }

   // The scratch directory is new and the tester's own: one prepared where it
   // is made is refused (tester_scratch.h).
   const std::string scratchRule = HMTesterScratch::Check();
   if (!scratchRule.empty())
      return Fail(scratch, scratchRule.c_str());

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

   HM::String iniName;
   (void) HM::FileUtilities::WidePath(file.string().c_str(), iniName);
   HM::IniFileSettings::Instance()->SetInitializationFile(iniName);
   HM::IniFileSettings::Instance()->LoadSettings();

   try
   {
      HM::IMAPMoveDispatchTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      return Fail(scratch, failure.what());
   }

   boost::filesystem::remove_all(scratch, error);

   printf("ok    the IMAP move event: raised for a committed MOVE or UID MOVE between the session account's own folders, once for each message whose original was removed, "
          "and not for a COPY, a failed MOVE, a public folder, another account's folders or any switch off; a command's events share one copy of its fields and are no more "
          "than the queue has room for; the queue holds its bounds of events and bytes and drops the newest; "
          "the VBScript and JScript calls quote every text; a JavaScript handler is given the account and the thirteen values\n");
   return 0;
}
