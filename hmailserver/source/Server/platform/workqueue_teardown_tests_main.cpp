// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A work queue removed while one of its worker threads is still inside
// io_context::run() - a task in a wait nothing in the queue can interrupt - as
// a program of its own, for the reason spf-tests is one and built the same way.
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
//     let go of by this program right after the remove.
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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>

#include "../Common/Application/IniFileSettings.h"
#include "../Common/Threading/Task.h"
#include "../Common/Threading/WorkQueue.h"
#include "../Common/Threading/WorkQueueManager.h"

namespace
{
   // Longer than the stop's ten-second give-up, so that the worker is still
   // inside its task when RemoveQueue returns.
   const std::chrono::milliseconds TaskSpan(20000);

   struct Progress
   {
      std::atomic<bool> started{false};
      std::atomic<bool> finished{false};
   };

   // Sits in a wait the queue cannot shorten: std::this_thread::sleep_for is not
   // a Boost interruption point, so the queue's interrupt() does not reach it and
   // the task runs its whole span whatever the stop does. Nothing of the queue is
   // touched here by hand; the queue's own bookkeeping runs as this returns.
   class BlockingTask : public HM::Task
   {
   public:
      explicit BlockingTask(std::shared_ptr<Progress> progress) :
         Task(_T("WorkQueueTeardownBlockingTask")),
         progress_(progress)
      {
      }

   protected:
      void DoWork() override
      {
         progress_->started.store(true);
         std::this_thread::sleep_for(TaskSpan);
         progress_->finished.store(true);
      }

   private:
      std::shared_ptr<Progress> progress_;
   };

   // What a queue keeps alive, standing in for the IOService: an io_context, and
   // a counter, which the task below holds by reference and nothing else.
   struct StandIn
   {
      boost::asio::io_context context;
      std::atomic<int> uses{0};
   };

   // Holds the stand-in by reference, as IOCPQueueWorkerTask holds the IOService's
   // io_context and a session's task holds a socket on it; after its wait it
   // posts to the context and counts, which reads both.
   class StandInTask : public HM::Task
   {
   public:
      StandInTask(StandIn &standIn, std::shared_ptr<Progress> progress) :
         Task(_T("WorkQueueTeardownStandInTask")),
         stand_in_(standIn),
         progress_(progress)
      {
      }

   protected:
      void DoWork() override
      {
         progress_->started.store(true);
         std::this_thread::sleep_for(TaskSpan);
         boost::asio::post(stand_in_.context, []() {});
         stand_in_.uses++;
         progress_->finished.store(true);
      }

   private:
      StandIn &stand_in_;
      std::shared_ptr<Progress> progress_;
   };

   bool WaitFor(const std::atomic<bool> &flag, std::chrono::seconds bound)
   {
      const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + bound;
      while (!flag.load() && std::chrono::steady_clock::now() < until)
         std::this_thread::sleep_for(std::chrono::milliseconds(10));
      return flag.load();
   }

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

   HM::WorkQueueManager &manager = *HM::WorkQueueManager::Instance();

   // The first queue keeps nothing alive; its task tests the queue itself.
   std::shared_ptr<Progress> queueTask = std::make_shared<Progress>();
   const size_t queueId = manager.CreateWorkQueue(1, _T("teardown-queue"));
   manager.AddTask(queueId, std::make_shared<BlockingTask>(queueTask));

   // The second keeps the stand-in alive, as the IOCP and session queues keep
   // the IOService; this program's own reference goes right after the remove,
   // as StopServers_ lets go of io_service_ after removing the queues.
   std::shared_ptr<Progress> standInTask = std::make_shared<Progress>();
   std::shared_ptr<StandIn> standIn = std::make_shared<StandIn>();
   std::weak_ptr<StandIn> standInSeen = standIn;
   const size_t standInId = manager.CreateWorkQueue(1, _T("teardown-outlives"), standIn);
   manager.AddTask(standInId, std::make_shared<StandInTask>(*standIn, standInTask));

   if (!WaitFor(queueTask->started, std::chrono::seconds(10)) || !WaitFor(standInTask->started, std::chrono::seconds(10)))
      return Fail(scratch, "a task never began on its queue's worker thread");

   printf("      removing two queues while their 20 s tasks run (each stop waits 10 s, then gives up)...\n");

   // Side by side: each stop is ten seconds, and the tasks must outlast both.
   std::thread removeQueue([&manager]() { manager.RemoveQueue(_T("teardown-queue")); });
   manager.RemoveQueue(_T("teardown-outlives"));
   removeQueue.join();
   standIn.reset();

   if (queueTask->finished.load() || standInTask->finished.load())
      return Fail(scratch, "a task had ended before the removes returned, so nothing was tested");

   if (HM::WorkQueue::GetThreadsLeft() != 2)
      return Fail(scratch, "the two threads the stops gave up on were not counted (WorkQueue::GetThreadsLeft)");

   if (manager.GetRetiredQueueCount() != 2)
      return Fail(scratch, "a removed queue whose thread was still inside was not kept");

   if (standInSeen.expired())
      return Fail(scratch, "what a removed queue keeps alive was freed while the queue's thread was still inside");

   // The tasks' waits end and the workers unwind through the queues'
   // bookkeeping, and the second task through the stand-in - the window a freed
   // queue, or a freed stand-in, faults in.
   if (!WaitFor(queueTask->finished, std::chrono::seconds(40)) || !WaitFor(standInTask->finished, std::chrono::seconds(40)))
      return Fail(scratch, "a task did not finish within the window");

   // The threads leave; then a sweep frees both queues, and the stand-in with
   // the second.
   const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
   while (HM::WorkQueue::GetThreadsLeft() != 0 && std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));

   if (HM::WorkQueue::GetThreadsLeft() != 0)
      return Fail(scratch, "a thread a stop gave up on was still counted after its task had ended");

   if (standInSeen.expired())
      return Fail(scratch, "what a removed queue keeps alive was freed before the queue was");

   manager.SweepRetiredQueues();

   if (manager.GetRetiredQueueCount() != 0)
      return Fail(scratch, "a removed queue was still kept after its thread had left and a sweep had run");

   if (!standInSeen.expired())
      return Fail(scratch, "what a removed queue kept alive was not freed with the queue");

   boost::filesystem::remove_all(scratch, error);

   printf("ok    the work-queue teardown: a queue removed while a worker is still in a wait the stop cannot interrupt is kept, with what it keeps alive, until the worker has left, and freed after\n");
   return 0;
}
