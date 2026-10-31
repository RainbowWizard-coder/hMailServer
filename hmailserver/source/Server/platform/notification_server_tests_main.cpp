// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// An IMAP session's subscription to the notification server, given back with
// the servers not running, as a program of its own, for the reason spf-tests is
// one and built the same way: NotificationServerLifetimeTester
// (Common/Application/Application.h), which on Windows runs inside the server
// through ClassTester, which nothing reaches here.
//
// A session with a folder selected subscribes to Application's notification
// server, and gives the subscription back as it ends (IMAPConnection::
// CloseCurrentFolder). A session can end after a stop of the servers has
// returned: one whose I/O service a queue the stop gave up on kept ends when
// that queue is freed, which a later sweep does - the next start's first new
// queue, or the removal of the maintenance queue before the process ends. Until
// 6.3.8 the stop let go of the notification server and the start made the next,
// so that such a session's unsubscribe read through a null pointer: on Windows
// an access violation the server's catch (...) took (HM4308), here a SIGSEGV
// that ends the process. Here the servers have never started, which is the
// state such a session ends in: the program holds that the notification server
// exists, that a subscriber is told of a change to its folder, and that a
// session's unsubscribe and its notification client's end give its own
// subscription back and nobody else's. The round across a stop and a start
// needs a running server, and runs on Windows alone.
//
// A scratch configuration directory of its own under the system's temp
// directory (the Logger and the ini singletons want one), removed at the end.
// No database and no network. One line either way; a non-zero exit on failure.
// Well under a second.

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include <boost/filesystem.hpp>

#include "tester_scratch.h"

#include "../Common/Application/Application.h"
#include "../Common/Application/IniFileSettings.h"
#include "../Common/Util/FileUtilities.h"

namespace
{
   int Fail(const boost::filesystem::path &scratch, const char *why)
   {
      printf("FAIL  the notification server with the servers not running: %s\n", why);
      boost::system::error_code ignored;
      boost::filesystem::remove_all(scratch, ignored);
      return 1;
   }
}

int
main(int /*argc*/, char * /*argv*/[])
{
   boost::system::error_code error;
   const boost::filesystem::path scratch = HMTesterScratch::Make("hm-notification-server-tests", error);
   if (error)
   {
      printf("FAIL  the notification server with the servers not running: no scratch directory could be made under the system's temp directory\n");
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

   HM::String iniName;
   (void) HM::FileUtilities::WidePath(file.string().c_str(), iniName);
   HM::IniFileSettings::Instance()->SetInitializationFile(iniName);
   HM::IniFileSettings::Instance()->LoadSettings();

   try
   {
      HM::NotificationServerLifetimeTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      return Fail(scratch, failure.what());
   }

   boost::filesystem::remove_all(scratch, error);

   printf("ok    the notification server with the servers not running: it exists, a subscriber is told of a change to its folder, and a session's unsubscribe and its client's end give back its own subscription and nobody else's\n");
   return 0;
}
