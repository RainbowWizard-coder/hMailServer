// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Created 2005-07-21

#pragma once

#include "../Util/Event.h"

#include <boost/asio.hpp>
#include <boost/thread.hpp>

#include <atomic>
#include <deque>
#include <map>
#include <set>
#include <vector>

namespace HM
{
   class Task;
   class Thread;

   // One entry per worker thread that is currently executing something. All
   // durations are milliseconds measured at the moment the snapshot was taken.
   struct WorkQueueTaskInfo
   {
      String name;
      unsigned int thread_id = 0;
      unsigned __int64 queue_wait_ms = 0;
      unsigned __int64 running_ms = 0;
      bool may_block = false;
   };

   class WorkQueue
   {
   public:

      enum TaskClass
      {
         TaskNormal = 0,
         // The task may block on something outside this process - a scanner, a
         // script, a database or a remote host. Those are capped below the
         // thread count so a wedged dependency cannot consume the whole pool.
         // Two caller constraints follow from the cap: such a task must not wait
         // for another task on the same queue, and nothing may wait on its
         // GetIsStartedEvent(), because it can legitimately sit in the wait list
         // for as long as the dependency is wedged.
         TaskMayBlock = 1
      };

      // outlives: an object this queue keeps alive for as long as the queue itself
      // exists - past its io_context and every handler still in it. Given by a queue
      // whose tasks hold what lives on that object, so that the object cannot be
      // freed while a task, or a handler not yet run, still holds part of it: the
      // session queues and the IOCP queue are given the IOService whose io_context
      // their connections' sockets are built on (Application::CreateSessionWorkQueues_,
      // IOService::DoWork). Null for every other queue.
      WorkQueue(unsigned int iMaxSimultaneous, const String &sName, std::shared_ptr<void> outlives = std::shared_ptr<void>());
      ~WorkQueue(void);

      void SetMaxSimultaneous(int iMaxSimultaneous);

      void AddTask(std::shared_ptr<Task> pTask, TaskClass task_class = TaskNormal);
      void Start();

      void Stop();

      // How many worker threads a stop gave up on are still running, in every
      // queue: counted when Stop gives up on one after its ten seconds, or the
      // destructor after its five more, and no longer from the moment the
      // thread ends - it says so itself, through what it shares with its queue,
      // which may be gone by then. Read by the stop before the process ends
      // (Application::ShutDown), which then ends the process at once rather
      // than through the destructors of statics, which such a thread may be
      // using. From 10 October 2026 until the re-check of that change this
      // counted queues, and a queue once counted stayed counted until a later
      // stop or its destructor joined the thread, so that a reinitialise that
      // gave up on a thread - which then ended - left every later stop of the
      // process taking the thread for one still running.
      static int GetThreadsLeft();

      // Whether any worker thread is still inside io_context::run() on this
      // queue right now - true while it runs, and still true of a thread a stop
      // or the destructor gave up on, until that thread leaves. The manager
      // reads it after a Stop to decide whether freeing the queue would pull the
      // io_context, and the state the worker still reads, out from under a thread
      // that is still in it; such a queue is kept alive (retired) instead and
      // freed once this is false.
      bool HasRunningWorkers() const { return live_worker_count_.load() > 0; }

      const String &GetName() const;

      // Whether WorkQueueHealthTask should report this queue's threads as stalled
      // when they are all busy for longer than AsyncQueueStallThreshold.
      //
      // Off by default, and that default is the important part. The stall report
      // means "nothing posted here can start", which is only a fault on a queue
      // whose tasks are expected to finish. Three of this server's queues host
      // tasks that never finish by design - the main server queue runs the
      // delivery manager and the external-fetch manager as permanent looping
      // tasks, and the IOCP queue's threads sit in io_context::run for the life of
      // the process - so on those "all threads busy for two minutes" is the
      // healthy state and reporting it would raise a High error every minute on a
      // stock installation. The maintenance queue is excluded for a milder version
      // of the same reason: a backup or a message-store consistency pass can
      // legitimately run for much longer than the threshold.
      //
      // So this is opt-in, per queue, at the point the queue is created, where
      // whoever adds the next queue has to decide which kind theirs is.
      void SetMonitorForStalls(bool monitor);
      bool GetMonitorForStalls() const;

      // Tasks accepted but not yet started, including those held back by the
      // TaskMayBlock cap.
      int GetQueueDepth() const;

      // Tasks accepted as TaskMayBlock that are waiting for a slot. These do not
      // occupy a worker thread.
      int GetWaitingBlockingTaskCount() const;

      // What every worker thread is doing right now, and for how long.
      std::vector<WorkQueueTaskInfo> GetRunningTasks() const;

      // Emits a rate limited error when no worker thread has been able to pick
      // up new work for AsyncQueueStallThreshold seconds. Meant to be called
      // periodically from a thread that is not owned by this queue.
      void ReportStalledTasks();

      // True while the last call to ReportStalledTasks found every worker thread
      // stuck in the same task. Read by WorkQueueHealthTask, which needs the
      // answer for the whole set of queues in order to raise the queue.stalled
      // alert once and clear it once; ReportStalledTasks itself cannot answer
      // that, because a queue that is fine returns without saying so.
      bool GetIsStalled() const;

   private:

      struct RunningTask
      {
         String name;
         unsigned int thread_id = 0;
         unsigned __int64 enqueue_tick = 0;
         unsigned __int64 start_tick = 0;
         bool may_block = false;
      };

      struct PendingTask
      {
         std::shared_ptr<Task> task;
         String name;
         unsigned __int64 enqueue_tick = 0;
      };

      void RemoveRunningTask_(unsigned __int64 task_id);
      void FinishTask_(unsigned __int64 task_id, bool may_block);

      // What became of one worker thread, which the thread and its queue
      // share: Running; Left, once a stop has given up on it and counted it in
      // GetThreadsLeft; Ended, once it has left its loop. Shared, so that the
      // thread can say it has ended after the queue that gave up on it is gone.
      enum WorkerFate
      {
         WorkerRunning = 0,
         WorkerLeft = 1,
         WorkerEnded = 2
      };

      typedef std::shared_ptr<std::atomic<int>> WorkerFatePtr;

      // The worker threads Stop or the destructor still have to join, counted
      // in GetThreadsLeft (Left), unless they have ended since.
      void CountThreadsLeft_();

      // The top of a worker thread, and the exception barrier for it. Nothing
      // above these frames can catch anything, so nothing may escape them.
      void IoServiceRunWorker(WorkerFatePtr fate);
      void RunWorker_();
      void ReportWorkerException_(const String &detail);

      // Bounded wait for every worker thread to leave. True when none remain.
      bool JoinWorkers_(int max_wait_ms);

      void ExecuteTask(std::shared_ptr<Task> pTask, String name, unsigned __int64 enqueue_tick, bool may_block);
      void PostTask_(std::shared_ptr<Task> pTask, const String &name, unsigned __int64 enqueue_tick, bool may_block);

      void DispatchBlockingTasks_();
      unsigned int GetBlockingLimit_() const;

      static unsigned __int64 ElapsedSince_(unsigned __int64 now, unsigned __int64 earlier);

      // See the constructor. Declared first, so that it is let go of last: after
      // io_context_, whose destructor destroys the handlers that never ran and the
      // connections they hold.
      std::shared_ptr<void> outlives_;

      boost::asio::io_context io_context_;
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;

      std::map<unsigned __int64, RunningTask> runningTasks_;
      unsigned __int64 next_task_id_;
      bool stall_reported_;
      unsigned __int64 last_stall_report_tick_;
      mutable boost::recursive_mutex runningTasksMutex_;

      // Guards the TaskMayBlock throttle. Never held while a task is running and
      // never taken while runningTasksMutex_ is held.
      mutable boost::mutex blockingMutex_;
      std::deque<PendingTask> pending_blocking_tasks_;
      unsigned int running_blocking_tasks_;

      std::atomic<int> queue_depth_;

      // Rate limit for the queue-wait warning; see the report site.
      std::atomic<unsigned __int64> last_queue_wait_report_tick_;

      // The number of threads Start() actually created, which is not the same as
      // max_simultaneous_ once that has been changed at runtime.
      std::atomic<unsigned int> worker_thread_count_;

      // The number of threads that are still inside io_context::run() right now.
      // Not the same as worker_thread_count_ the moment a worker leaves, and it is
      // this one the stall detector compares against: a queue that has lost workers
      // is precisely the queue a stall report exists for, and measured against the
      // count Start() created it could never satisfy "every thread is busy" again.
      std::atomic<unsigned int> live_worker_count_;

      // Set by Stop() before the io_context is stopped. From that point a task
      // handed to this queue can never run, so AddTask says so rather than
      // accepting it and dropping it in silence.
      std::atomic<bool> stopping_;

      // GetThreadsLeft: a thread is added when its fate moves from Running to
      // Left, and taken off when it moves from Left to Ended, and only then.
      static std::atomic<int> threads_left_;

      unsigned int max_simultaneous_;

      String queue_name_;

      // See SetMonitorForStalls. Atomic because the health task reads it from the
      // scheduler thread while the queue runs on its own.
      std::atomic<bool> monitor_for_stalls_{false};

      // Each worker thread not yet joined, with its fate.
      std::map<std::shared_ptr<boost::thread>, WorkerFatePtr> workerThreads_;
   };

}
