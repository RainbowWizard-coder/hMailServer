// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Created 2005-07-21

#include "StdAfx.h"

#include "WorkQueueManager.h"
#include "Task.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <boost/asio.hpp>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   WorkQueueManager::WorkQueueManager(void) :
      next_queue_id_(0)
   {
   }

   WorkQueueManager::~WorkQueueManager(void)
   {
   }

   size_t 
   WorkQueueManager::CreateWorkQueue(int iMaxSimultaneous, const String &sQueueName, std::shared_ptr<void> outlives)
   //---------------------------------------------------------------------------
   // DESCRIPTION:
   // Creates a new work queue and adds it to the list of queues. Returns the ID
   // of the new queue. outlives: what the queue keeps alive for as long as it
   // exists (WorkQueue's constructor).
   //---------------------------------------------------------------------------
   {
      // Create the work queue
      std::shared_ptr<WorkQueue> pWorkQueue = std::shared_ptr<WorkQueue>(new WorkQueue(iMaxSimultaneous, sQueueName, outlives));
      pWorkQueue->Start();

      // A good moment to let go of any queue a past remove had to keep alive: its
      // thread has had until now to leave.
      SweepRetired_();

      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      // Counted up for ever, rather than derived from work_queues_.size(). Queues
      // come and go over the life of the process - the delivery, external fetch and
      // IOCP queues are created and removed with every server start and stop - so a
      // size-derived id is handed out again as soon as anything has been removed.
      // The reissued id then silently replaces the map entry of a live queue: the
      // caller holding the older id posts its tasks into a queue that is not the one
      // it created, and the queue that was displaced is destroyed without Stop()
      // ever being called on it, with its worker threads still running inside it.
      next_queue_id_++;

      size_t iQueueID = next_queue_id_;

      work_queues_[iQueueID] = pWorkQueue;

      return iQueueID;
   }

   void 
   WorkQueueManager::AddTask(size_t iQueueID, std::shared_ptr<Task> pTask)
   //---------------------------------------------------------------------------
   // DESCRIPTION:
   // Adds a task to a worker queue.
   //---------------------------------------------------------------------------
   {
      // Add the task to the work queue
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      auto iterQueue = work_queues_.find(iQueueID);

      if (iterQueue == work_queues_.end())
      {
         // Someone is trying to add a task to a queue that does not exist. The
         // assert is a debug-build aid and compiles to nothing in a release build,
         // where what used to follow it was a dereference of end() - a crash, in
         // the branch whose entire purpose was to say "this cannot happen".
         HM_ASSERT(0);

         // iQueueID is a size_t, which is unsigned __int64 on Windows and binds to
         // that FormatArgument constructor exactly. On a 64-bit POSIX build size_t
         // is unsigned long, which matches none of them exactly and is no closer to
         // one than to the others. The cast is an identity on Windows.
         ErrorManager::Instance()->ReportError(ErrorManager::High, 6076, "WorkQueueManager::AddTask",
            Formatter::Format("Task {0} was discarded because work queue {1} does not exist.",
                              pTask ? pTask->GetName() : String(_T("<null>")), (unsigned __int64) iQueueID));

         return;
      }

      std::shared_ptr<WorkQueue> pWorkQueue = (*iterQueue).second;

      pWorkQueue->AddTask(pTask);
   }

   void 
   WorkQueueManager::RemoveQueue(const String &sQueueName)
   //---------------------------------------------------------------------------
   // DESCRIPTION:
   // Stops and removes a queue
   //---------------------------------------------------------------------------
   {
      LOG_DEBUG(Formatter::Format("WorkQueueManager::RemoveQueue - {0}", sQueueName));

      // Locate the work queue
      std::shared_ptr<WorkQueue> pQueue;
      std::map<size_t, std::shared_ptr<WorkQueue> >::iterator iterQueue;

      {
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);

         iterQueue = GetQueueIterator_(sQueueName);
         if (iterQueue == work_queues_.end())
         {
            LOG_DEBUG(Formatter::Format("WorkQueueManager::RemoveQueue - Work quue not found {0}", sQueueName));
            return;
         }

         pQueue = (*iterQueue).second;
         if (!pQueue)
         {
            LOG_DEBUG(Formatter::Format("WorkQueueManager::RemoveQueue - Work quue not found {0}", sQueueName));
            return;
         }
      }

      pQueue->Stop();

      LOG_DEBUG(Formatter::Format("WorkQueueManager::RemoveQueue - Stopped {0}", sQueueName));

      {
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);

         // Found again by the pointer that was just stopped, rather than erased
         // through the iterator taken above. The lock was released for the whole of
         // Stop(), which is bounded at ten seconds - long enough for a second
         // RemoveQueue of the same name during a stop/start race to have erased this
         // entry already, leaving that iterator dangling and this erase undefined.
         // Matching on the pointer rather than the name also means a queue of the
         // same name created in that window is left where it is instead of being
         // torn out from under whoever just created it.
         for (auto iter = work_queues_.begin(); iter != work_queues_.end(); iter++)
         {
            if ((*iter).second == pQueue)
            {
               work_queues_.erase(iter);
               break;
            }
         }

         // Stop gave up on a worker still inside io_context::run() - a task in a
         // blocking wait outside Boost's interruption points. Letting pQueue drop
         // here would run the destructor, which frees the io_context, and the state
         // the worker still reads as it leaves, out from under that thread. The
         // queue is kept alive instead until the thread leaves (SweepRetired_
         // frees it then), so the worker finishes in a live object and the caller -
         // a stop or a reinitialise - goes on at once.
         if (pQueue->HasRunningWorkers())
            retired_queues_.push_back(pQueue);
      }

      LOG_DEBUG(Formatter::Format("WorkQueueManager::RemoveQueue - Erased {0}", sQueueName));

      SweepRetired_();
   }

   void
   WorkQueueManager::SweepRetiredQueues()
   {
      SweepRetired_();
   }

   void
   WorkQueueManager::SweepRetired_()
   //---------------------------------------------------------------------------
   // DESCRIPTION:
   // Frees the retired queues whose worker threads have all left. See the
   // member's comment in the header.
   //
   // Never frees the queue of the thread that calls it, whichever thread that
   // is: a thread inside a queue's io_context::run() counts in that queue's
   // HasRunningWorkers, so its own queue is not one of those freed here.
   //---------------------------------------------------------------------------
   {
      // Taken out under the lock and destroyed after it: the destructor joins the
      // threads - which have ended, so the join is near instant - and lets go of
      // what the queue kept alive, an IOService whose connections then close.
      // Holding mutex_ across that would stall every AddTask behind it.
      std::vector<std::shared_ptr<WorkQueue> > toDrop;

      {
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);

         for (auto iter = retired_queues_.begin(); iter != retired_queues_.end(); )
         {
            if (!(*iter)->HasRunningWorkers())
            {
               toDrop.push_back(*iter);
               iter = retired_queues_.erase(iter);
            }
            else
            {
               iter++;
            }
         }
      }

      toDrop.clear();
   }

   std::vector<std::shared_ptr<WorkQueue> >
   WorkQueueManager::GetAllQueues()
   //---------------------------------------------------------------------------
   // DESCRIPTION:
   // Every queue that currently exists.
   //
   // A copy is taken under the lock and the lock released before the caller looks
   // at any of them. The health check walks each queue's own state, which takes
   // that queue's lock; holding this one across that would mean two locks acquired
   // in the opposite order from anything that creates or removes a queue while
   // holding a queue lock, and a deadlock in the thing that exists to notice a
   // stall would be a poor joke.
   //---------------------------------------------------------------------------
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      std::vector<std::shared_ptr<WorkQueue> > queues;
      queues.reserve(work_queues_.size());

      for (auto &entry : work_queues_)
      {
         if (entry.second)
            queues.push_back(entry.second);
      }

      return queues;
   }

   std::shared_ptr<WorkQueue>
   WorkQueueManager::GetQueue(const String &sQueueName)
   //---------------------------------------------------------------------------
   // DESCRIPTION:
   // Returns the queue with a specific name.
   //---------------------------------------------------------------------------
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      // GetQueueIterator_ has already compared the name, so the second comparison
      // that used to be here could not fail, and the iterQueue++ that followed it
      // was unreachable in the case it was written for and did nothing in the case
      // it actually ran.
      auto iterQueue = GetQueueIterator_(sQueueName);

      if (iterQueue != work_queues_.end())
         return (*iterQueue).second;

      std::shared_ptr<WorkQueue> pEmpty;
      return pEmpty;

   }

   std::map<size_t, std::shared_ptr<WorkQueue> >::iterator
   WorkQueueManager::GetQueueIterator_(const String &sQueueName)
   //---------------------------------------------------------------------------
   // DESCRIPTION:
   // Returns a iterator to a queue with the specified name.
   //---------------------------------------------------------------------------
   {
      auto iterQueue = work_queues_.begin();
      while (iterQueue != work_queues_.end())
      {
         std::shared_ptr<WorkQueue> pQueue = (*iterQueue).second;
         if (pQueue->GetName().CompareNoCase(sQueueName) == 0)
            return iterQueue;

         iterQueue++;
      }

      return work_queues_.end();
   }

   namespace
   {
      // Longer than the stop's ten-second give-up, so that the worker is still
      // inside its task when RemoveQueue returns.
      const std::chrono::milliseconds TeardownTaskSpan(20000);

      struct TeardownProgress
      {
         std::atomic<bool> started{false};
         std::atomic<bool> finished{false};
      };

      // Sits in a wait the queue cannot shorten: std::this_thread::sleep_for is
      // not a Boost interruption point, so the stop's interrupt() does not reach
      // it and the task runs its whole span whatever the stop does. Nothing of
      // the queue is touched here by hand; the queue's own bookkeeping runs as
      // this returns.
      class TeardownBlockingTask : public Task
      {
      public:
         explicit TeardownBlockingTask(std::shared_ptr<TeardownProgress> progress) :
            Task(_T("WorkQueueTeardownTester's task")),
            progress_(progress)
         {
         }

      protected:
         void DoWork() override
         {
            progress_->started.store(true);
            std::this_thread::sleep_for(TeardownTaskSpan);
            progress_->finished.store(true);
         }

      private:
         std::shared_ptr<TeardownProgress> progress_;
      };

      // What the second queue keeps alive, standing in for the TCP/IP service:
      // an io_context and a counter, which the task below holds by reference and
      // nothing else.
      struct TeardownStandIn
      {
         boost::asio::io_context context;
         std::atomic<int> uses{0};
      };

      // Holds the stand-in by reference, as IOCPQueueWorkerTask holds the TCP/IP
      // service's io_context and a session's task a socket on it; after its wait
      // it posts to the context and counts, which reads both.
      class TeardownStandInTask : public Task
      {
      public:
         TeardownStandInTask(TeardownStandIn &standIn, std::shared_ptr<TeardownProgress> progress) :
            Task(_T("WorkQueueTeardownTester's kept task")),
            stand_in_(standIn),
            progress_(progress)
         {
         }

      protected:
         void DoWork() override
         {
            progress_->started.store(true);
            std::this_thread::sleep_for(TeardownTaskSpan);
            boost::asio::post(stand_in_.context, []() {});
            stand_in_.uses++;
            progress_->finished.store(true);
         }

      private:
         TeardownStandIn &stand_in_;
         std::shared_ptr<TeardownProgress> progress_;
      };

      bool WaitForFlag(const std::atomic<bool> &flag, std::chrono::seconds bound)
      {
         const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + bound;
         while (!flag.load() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
         return flag.load();
      }

      bool WaitForThreadsLeft(int count, std::chrono::seconds bound)
      {
         const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + bound;
         while (WorkQueue::GetThreadsLeft() != count && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
         return WorkQueue::GetThreadsLeft() == count;
      }

      void TeardownFailed(const char *why)
      {
         throw std::logic_error(std::string("WorkQueueTeardownTester: ") + why);
      }
   }

   void
   WorkQueueTeardownTester::Test()
   {
      WorkQueueManager &manager = *WorkQueueManager::Instance();

      // Relative to what is counted already: in the server, a thread an earlier
      // stop gave up on may still be running.
      const int before = WorkQueue::GetThreadsLeft();

      const String plainName = _T("WorkQueueTeardownTester's queue");
      const String keepingName = _T("WorkQueueTeardownTester's keeping queue");

      // The first queue keeps nothing alive; its task tests the queue itself.
      std::shared_ptr<TeardownProgress> plainTask = std::make_shared<TeardownProgress>();
      const size_t plainId = manager.CreateWorkQueue(1, plainName);
      std::weak_ptr<WorkQueue> plainSeen = manager.GetQueue(plainName);
      manager.AddTask(plainId, std::make_shared<TeardownBlockingTask>(plainTask));

      // The second keeps the stand-in alive, as the IOCP and session queues keep
      // the TCP/IP service; the test's own reference goes right after the
      // remove, as StopServers_ lets go of io_service_ after removing the queues.
      std::shared_ptr<TeardownProgress> keepingTask = std::make_shared<TeardownProgress>();
      std::shared_ptr<TeardownStandIn> standIn = std::make_shared<TeardownStandIn>();
      std::weak_ptr<TeardownStandIn> standInSeen = standIn;
      const size_t keepingId = manager.CreateWorkQueue(1, keepingName, standIn);
      std::weak_ptr<WorkQueue> keepingSeen = manager.GetQueue(keepingName);
      manager.AddTask(keepingId, std::make_shared<TeardownStandInTask>(*standIn, keepingTask));

      if (!WaitForFlag(plainTask->started, std::chrono::seconds(30)) || !WaitForFlag(keepingTask->started, std::chrono::seconds(30)))
         TeardownFailed("a task never began on its queue's worker thread.");

      // Side by side: each stop is ten seconds, and the tasks must outlast both.
      std::thread removePlain([&manager, plainName]() { manager.RemoveQueue(plainName); });
      manager.RemoveQueue(keepingName);
      removePlain.join();
      standIn.reset();

      const bool endedBeforeTheRemovesReturned = plainTask->finished.load() || keepingTask->finished.load();
      const int leftAfterTheRemoves = WorkQueue::GetThreadsLeft();
      const bool queuesKept = !plainSeen.expired() && !keepingSeen.expired();
      const bool standInKept = !standInSeen.expired();
      const bool unlisted = !manager.GetQueue(plainName) && !manager.GetQueue(keepingName);

      // The tasks' waits end and the workers unwind through the queues'
      // bookkeeping, and the second task through the stand-in - the window a
      // freed queue, or a freed stand-in, faults in. Waited for before anything
      // is judged, so that a failure below never leaves a task running in what
      // the test is about to let go of.
      const bool finished = WaitForFlag(plainTask->finished, std::chrono::seconds(60)) && WaitForFlag(keepingTask->finished, std::chrono::seconds(60));
      const bool noLongerCounted = finished && WaitForThreadsLeft(before, std::chrono::seconds(20));

      if (endedBeforeTheRemovesReturned)
         TeardownFailed("a remove returned only once its queue's task had ended: the queue's teardown waited for the task its stop had given up on.");

      if (!unlisted)
         TeardownFailed("a removed queue was still listed by the manager.");

      if (leftAfterTheRemoves != before + 2)
         TeardownFailed("the two threads the stops gave up on were not counted (WorkQueue::GetThreadsLeft).");

      if (!queuesKept)
         TeardownFailed("a removed queue whose thread was still inside it was freed under that thread.");

      if (!standInKept)
         TeardownFailed("what a removed queue keeps alive was freed while the queue's thread was still inside.");

      if (!finished)
         TeardownFailed("a task did not finish within a minute of its span.");

      if (!noLongerCounted)
         TeardownFailed("a thread a stop gave up on was still counted after its task had ended.");

      // A sweep may have run already - the queue health task's, once a minute,
      // in the server - but never one that frees the stand-in before its queue.
      if (standInSeen.expired() && !keepingSeen.expired())
         TeardownFailed("what a removed queue keeps alive was freed before the queue was.");

      manager.SweepRetiredQueues();

      if (!plainSeen.expired() || !keepingSeen.expired())
         TeardownFailed("a removed queue was still kept after its thread had left and a sweep had run.");

      if (!standInSeen.expired())
         TeardownFailed("what a removed queue kept alive was not freed with the queue.");
   }
}
