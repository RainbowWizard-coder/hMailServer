// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// What a task or a session that a stop of the servers could not end reaches
// after the stop, as a program of its own, for the reason spf-tests is one and
// built the same way: StoppedServersTester (Common/Application/
// StoppedServersTester.h), which on Windows runs inside the server through
// ClassTester, which nothing reaches here.
//
// Here the servers have never started, so there is no TCP/IP service and no
// database manager - the state a thread a stop gave up on goes on in after the
// stop, and, in a reinitialise, until OpenDatabase. The program holds that a
// session on an I/O service that has stopped is told nothing of a change to its
// folder; that a message's SpamAssassin test, a lesson for SpamAssassin, the
// test of the connection to SpamAssassin, the diagnostics' connection test, a
// fetch from an external account and a delivery to one server each return,
// saying why, and make no connection; that a clear of the delivery queue with
// no delivery manager says the servers are stopped and that it is over; and
// that a folder's messages refreshed, their \Recent removed and claimed, and an
// IMAP session closing its folder, each with no database manager, do so
// without a fault. Until 6.3.8 the second, third and fourth read through an
// empty pointer: here a SIGSEGV, which the handler below names by the case that
// was running, or a lock of no mutex.
//
// A scratch configuration directory of its own under the system's temp
// directory (the Logger and the ini singletons want one), removed at the end.
// No database and no network: nothing here connects. One line either way; a
// non-zero exit on failure. Well under a second.

#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include <boost/filesystem.hpp>

#include "tester_scratch.h"

#include "../Common/Application/Application.h"
#include "../Common/Application/IniFileSettings.h"
#include "../Common/Application/StoppedServersTester.h"
#include "../Common/Util/FileUtilities.h"

namespace
{
   int Fail(const boost::filesystem::path &scratch, const char *why)
   {
      printf("FAIL  what a task or a session reaches after a stop of the servers: %s\n", why);
      boost::system::error_code ignored;
      boost::filesystem::remove_all(scratch, ignored);
      return 1;
   }

   // The most Write sends of one text: the longest it is given, a case's name,
   // is 116 bytes.
   const size_t MostWritten = 1024;

   // Writes text to standard output from the fault handler, and so with nothing
   // but strnlen and write(2), both of which POSIX lists as async-signal-safe
   // (strnlen from its 2016 edition): no stdio, no allocation, no lock, no other
   // call. The length is counted only up to MostWritten bytes, so that a text
   // whose terminating zero has been lost is read no further than MostWritten
   // bytes from its start, where strlen would read on to whatever zero follows;
   // what follows such a text, up to a zero or that bound, is written with it.
   void Write(const char *text)
   {
      if (text == nullptr)
         return;

      ssize_t ignored = write(1, text, strnlen(text, MostWritten));
      (void) ignored;
   }

   // A fault in a case: said with the case's name, and the program ends. The
   // scratch directory is left; the system's temp directory is cleaned for it.
   void OnFault(int)
   {
      Write("FAIL  what a task or a session reaches after a stop of the servers: ");
      Write(HM::StoppedServersTester::GetCurrentCase());
      Write(" faulted (SIGSEGV): it reached through an empty pointer for what a stop lets go of\n");
      _exit(1);
   }
}

int
main(int /*argc*/, char * /*argv*/[])
{
   boost::system::error_code error;
   const boost::filesystem::path scratch = HMTesterScratch::Make("hm-stopped-servers-tests", error);
   if (error)
   {
      printf("FAIL  what a task or a session reaches after a stop of the servers: no scratch directory could be made under the system's temp directory\n");
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

   struct sigaction action = {};
   action.sa_handler = OnFault;
   sigaction(SIGSEGV, &action, nullptr);
   sigaction(SIGBUS, &action, nullptr);

   try
   {
      HM::StoppedServersTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      return Fail(scratch, failure.what());
   }

   boost::filesystem::remove_all(scratch, error);

   printf("ok    what a task or a session reaches after a stop of the servers: a session on a stopped I/O service is told nothing; with no TCP/IP service the SpamAssassin test, a lesson, the two connection tests, a fetch and a delivery return, saying why, and a clear of the delivery queue says it is over; with no database manager a folder's messages and \\Recent, and a session closing its folder, fault nothing\n");
   return 0;
}
