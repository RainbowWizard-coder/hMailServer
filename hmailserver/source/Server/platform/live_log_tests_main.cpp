// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The Logger's live log (COM's Logging.EnableLiveLogging and LiveLog, the Control
// Panel's Live logs) switched on and off while other threads log, as a program of
// its own for the POSIX build - the counterpart of spf-tests and backup-tests.
//
// Every thread that logs tests Logger::enable_live_log_ before it takes the live
// log's lock, so that an entry costs no lock while nobody watches; the switch and
// the buffer's limit write it under the lock. It was a plain bool until 6.3.6: a
// data race to the language, which a ThreadSanitizer build of this program reports
// against the Logger of 6.3.5 on its first switch (29 September 2026 left it for
// later; 30 September 2026 made it an atomic). Built as linux-build.sh builds it,
// the program checks what the flag guards - no entry is appended once the switch is
// off, a live log that outgrows its limit switches itself off and is emptied, and
// every entry reaches the buffer whole - and built with
//
//   -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer"
//   -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer"
//   -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
//
// it is the race detector's harness as well: four threads log while one switches
// the live log on and off and one drains it, for two seconds or until that has
// happened often enough to count (thirty at most).
//
// It writes a configuration file of its own in a directory of its own under the
// system's temp directory, which it names for every directory, so that the log
// files the entries also go to land there; the directory is removed again. No
// database and no network. One line per check, and a non-zero exit on a failure.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include <boost/filesystem.hpp>

#include "../Common/Application/IniFileSettings.h"
#include "../Common/Application/Logger.h"

namespace
{
   int failures = 0;

   void Check(bool condition, const char *what)
   {
      printf("%s  %s\n", condition ? "ok  " : "FAIL", what);
      if (!condition)
         failures++;
   }

   // Every line of a drained live log whole: ending in the default format's CRLF and
   // one of this program's entries. A line cut short by an append that raced another
   // would end without its CR or be missing its marker.
   bool WholeLines(const HM::String &text, const char *marker, long long &lines)
   {
      std::string narrow = HM::AnsiString(text).c_str();
      size_t start = 0;
      while (start < narrow.size())
      {
         size_t end = narrow.find('\n', start);
         if (end == std::string::npos)
            return false;

         const std::string line = narrow.substr(start, end - start);
         if (line.empty() || line[line.size() - 1] != '\r' || line.find(marker) == std::string::npos)
            return false;

         lines++;
         start = end + 1;
      }
      return true;
   }
}

int
main(int /*argc*/, char * /*argv*/[])
{
   boost::system::error_code error;
   const boost::filesystem::path scratch = boost::filesystem::temp_directory_path(error) /
      ("hm-live-log-tests-" + std::to_string((long long) ::getpid()));
   if (!error)
      boost::filesystem::create_directories(scratch, error);
   if (error)
   {
      printf("FAIL  the live log: no scratch directory could be made under the system's temp directory\n");
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
      {
         printf("FAIL  the live log: %s could not be written\n", file.string().c_str());
         boost::filesystem::remove_all(scratch, error);
         return 1;
      }
   }

   // The configuration first: the Logger takes its directory when it is made.
   HM::IniFileSettings::Instance()->SetInitializationFile(HM::String(file.string().c_str()));
   HM::IniFileSettings::Instance()->LoadSettings();

   HM::Logger *logger = HM::Logger::Instance();
   logger->SetLogMask(HM::Logger::LSEnabled | HM::Logger::LSApplication);

   // Switched on, an entry is there to read, once.
   logger->EnableLiveLogging(true);
   logger->LogApplication(_T("live-probe first"));
   const HM::String first = logger->GetLiveLog();
   Check(logger->GetLiveLogEnabled() && first.Find(_T("live-probe first")) >= 0, "switched on, an entry logged is in the live log");
   Check(logger->GetLiveLog().IsEmpty(), "  ... and a second read finds it taken");

   // Switched off, nothing is added and what was there is gone.
   logger->LogApplication(_T("live-probe dropped"));
   logger->EnableLiveLogging(false);
   logger->LogApplication(_T("live-probe off"));
   Check(!logger->GetLiveLogEnabled() && logger->GetLiveLog().IsEmpty(), "switched off, the live log is empty and takes nothing");

   // A listener that stopped reading: past LiveLogMaxSize the live log is emptied and
   // switches itself off.
   logger->EnableLiveLogging(true);
   const HM::String filler(_T("live-probe filler ") + HM::String(std::wstring(200, L'x').c_str()));
   int logged = 0;
   while (logger->GetLiveLogEnabled() && logged < 100000)
   {
      logger->LogApplication(filler);
      logged++;
   }
   Check(!logger->GetLiveLogEnabled() && logger->GetLiveLog().IsEmpty() && logged > 1000,
      "a live log nobody reads switches itself off past its limit, and is emptied");

   // Contention: four threads log, one switches the live log on and off, one drains
   // it. Under ThreadSanitizer this is where a plain flag is reported.
   std::atomic<bool> stop(false);
   std::atomic<bool> torn(false);
   std::atomic<long long> entries(0);
   std::atomic<long long> switches(0);
   std::atomic<long long> drained(0);
   std::vector<std::thread> threads;

   for (int t = 0; t < 4; t++)
   {
      threads.emplace_back([&, t]()
      {
         long long n = 0;
         while (!stop.load())
         {
            logger->LogApplication(HM::String(_T("live-probe t")) + HM::StringParser::IntToString(t) + _T(" n") +
               HM::StringParser::IntToString((int) (n++ % 1000000)));
            entries++;
         }
      });
   }

   threads.emplace_back([&]()
   {
      while (!stop.load())
      {
         logger->EnableLiveLogging(true);
         std::this_thread::sleep_for(std::chrono::microseconds(300));
         logger->EnableLiveLogging(false);
         std::this_thread::sleep_for(std::chrono::microseconds(100));
         switches++;
      }
   });

   threads.emplace_back([&]()
   {
      while (!stop.load())
      {
         long long lines = 0;
         if (!WholeLines(logger->GetLiveLog(), "live-probe t", lines))
            torn = true;
         drained += lines;
         std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
   });

   // Two seconds at least, and on until the contention the checks below ask for has
   // happened - a slow builder (an emulated arm64 package build, a sanitizer) gets
   // there later - but never past thirty.
   const auto started = std::chrono::steady_clock::now();
   for (;;)
   {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const auto elapsed = std::chrono::steady_clock::now() - started;
      const bool enough = switches.load() > 100 && entries.load() > 1000 && drained.load() > 0;
      if ((enough && elapsed >= std::chrono::seconds(2)) || elapsed >= std::chrono::seconds(30))
         break;
   }

   // Switched off for good while the loggers go on: from here nothing is added.
   stop = true;
   for (std::thread &thread : threads)
      thread.join();
   threads.clear();

   logger->EnableLiveLogging(false);
   long long lines = 0;
   std::atomic<bool> stopLate(false);
   for (int t = 0; t < 4; t++)
   {
      threads.emplace_back([&, t]()
      {
         while (!stopLate.load())
            logger->LogApplication(HM::String(_T("live-probe late t")) + HM::StringParser::IntToString(t));
      });
   }
   std::this_thread::sleep_for(std::chrono::milliseconds(300));
   stopLate = true;
   for (std::thread &thread : threads)
      thread.join();
   const HM::String late = logger->GetLiveLog();

   printf("      %lld entries logged, %lld switch pairs, %lld whole lines drained\n", entries.load(), switches.load(), drained.load());
   Check(!torn.load(), "every line drained while the live log was switched on and off is one whole entry");
   Check(switches.load() > 100 && entries.load() > 1000 && drained.load() > 0, "  ... and the contention happened (switches, entries and lines drained)");
   Check(late.IsEmpty() && WholeLines(late, "live-probe", lines), "switched off, nothing logged on four threads afterwards reaches the live log");

   boost::filesystem::remove_all(scratch, error);

   if (failures > 0)
   {
      printf("FAIL  the live log: %d check(s) failed\n", failures);
      return 1;
   }

   printf("ok    the live log: switched on and off, its limit, and four threads logging beside the switch\n");
   return 0;
}
