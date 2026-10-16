// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A work queue removed while one of its worker threads is still inside
// io_context::run() - a task in a wait nothing in the queue can interrupt - as
// a program of its own, for the reason spf-tests is one and built the same way:
// WorkQueueTeardownTester (Common/Threading/WorkQueueManager.h), which on
// Windows runs inside the server through ClassTester, which nothing reaches
// here.
//
// The queue's stop waits ten seconds for its worker and gives up. A task that
// outlives that - twenty seconds here - leaves a worker inside the queue after
// WorkQueueManager::RemoveQueue has returned. Two things must then still exist
// when the task ends:
//
//   - the queue itself: its io_context, and the mutexes, containers and
//     counters the worker reads as it leaves. The manager keeps a queue whose
//     stop gave up on a thread (it retires it) and frees it once the thread
//     has left;
//   - what the queue was given to keep alive (WorkQueue's outlives): the
//     IOService whose io_context the IOCP and session queues' tasks hold
//     connections on, which Application::StopServers_ lets go of as soon as the
//     queues are removed. Here a stand-in, an io_context and a counter the task
//     holds by reference as IOCPQueueWorkerTask holds the IOService's context,
//     let go of by the tester right after the remove.
//
// Built under AddressSanitizer (CMake -DHM_SANITIZE=ON, the linux-sanitize CI
// job) a tree that frees either under the thread is a heap-use-after-free the
// sanitiser reports and, with ASAN_OPTIONS=abort_on_error=1, aborts on; in the
// ordinary build linux-build.sh runs, it checks the same bookkeeping without
// the sanitiser. With both kept until the worker has left, the tasks end in live
// objects, and once a sweep has run both are freed - nothing kept for ever.
//
// A scratch configuration directory of its own under the system's temp
// directory (the Logger and the ini singletons want one), removed at the end.
// No database and no network. One line either way; a non-zero exit on failure.
// About twenty-two seconds: the two removes run side by side.

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include <boost/filesystem.hpp>

#include "../Common/Application/IniFileSettings.h"
#include "../Common/Threading/WorkQueueManager.h"

namespace
{
   int Fail(const boost::filesystem::path &scratch, const char *why)
   {
      printf("FAIL  the work-queue teardown: %s\n", why);
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
      ("hm-workqueue-teardown-tests-" + std::to_string((long long) ::getpid()));
   if (!error)
      boost::filesystem::create_directories(scratch, error);
   if (error)
   {
      printf("FAIL  the work-queue teardown: no scratch directory could be made under the system's temp directory\n");
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

   printf("      removing two queues while their 20 s tasks run (each stop waits 10 s, then gives up)...\n");

   try
   {
      HM::WorkQueueTeardownTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      return Fail(scratch, failure.what());
   }

   boost::filesystem::remove_all(scratch, error);

   printf("ok    the work-queue teardown: a queue removed while a worker is still in a wait the stop cannot interrupt is kept, with what it keeps alive, until the worker has left, and freed after\n");
   return 0;
}
