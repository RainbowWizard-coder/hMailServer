// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Created 2005-07-21

#pragma once

#include "WorkQueue.h"

namespace HM
{
   class WorkQueue;
   class Task;

   class WorkQueueManager : public Singleton<WorkQueueManager>
   {
   public:
      WorkQueueManager(void);
      ~WorkQueueManager(void);

      // outlives: an object the queue keeps alive for as long as the queue exists
      // (WorkQueue's constructor); null for most queues.
      size_t CreateWorkQueue(int iMaxSimultaneous, const String &sQueueName, std::shared_ptr<void> outlives = std::shared_ptr<void>());
      void RemoveQueue(const String &sQueueName);

      // Frees the retired queues (below) whose worker threads have all left. Also
      // done by every create and remove; WorkQueueHealthTask calls it once a
      // minute, so that a queue - and the IOService it keeps alive, with the
      // connections on it - is let go of soon after its last thread has left
      // rather than at the next stop or start of the servers.
      void SweepRetiredQueues();

      void AddTask(size_t iQueueID, std::shared_ptr<Task> pTask);

      std::shared_ptr<WorkQueue> GetQueue(const String &sQueueName);

      // Every queue that currently exists, for callers that measure rather than
      // dispatch. Returned by value, and deliberately: the health check must not
      // hold the manager's lock while it walks each queue's own state, or the two
      // locks are taken in opposite orders by anything that creates a queue while
      // holding a queue lock.
      std::vector<std::shared_ptr<WorkQueue> > GetAllQueues();

   private:

      std::map<size_t, std::shared_ptr<WorkQueue> >::iterator GetQueueIterator_(const String &sQueueName);

      // Drops the retired queues below whose worker threads have all left
      // io_context::run() (WorkQueue::HasRunningWorkers() is false): their
      // destructors join threads that have already ended, so it is near
      // instantaneous. A thread still inside a queue keeps that queue's
      // HasRunningWorkers true, so the caller's own queue is never one of them,
      // whichever thread calls; the queues are destroyed outside mutex_ so a join
      // cannot be held behind it.
      void SweepRetired_();

      boost::recursive_mutex mutex_;
      std::map<size_t, std::shared_ptr<WorkQueue> > work_queues_;

      // Queues whose stop gave up on a worker thread still inside
      // io_context::run() - a task in a blocking wait outside Boost's
      // interruption points, past the ten seconds the stop waits. Freeing such
      // a queue would pull its io_context, and the mutexes, containers and
      // counters the worker still reads as it leaves, out from under that
      // thread. The queue is kept here instead until the thread leaves, and
      // SweepRetired_ frees it then. It is not the worker that holds the queue
      // alive (a worker that dropped the last reference would run the
      // destructor, and so join its own thread, on itself): the manager does,
      // and sweeps on the next create or remove and once a minute
      // (SweepRetiredQueues).
      std::vector<std::shared_ptr<WorkQueue> > retired_queues_;

      // Never reused, never derived from the size of the map above. See
      // CreateWorkQueue for what reissuing an id costs.
      size_t next_queue_id_;

   };

   // A work queue removed while one of its worker threads is still inside
   // io_context::run() - a task in a wait nothing in the queue can interrupt,
   // past the stop's ten seconds - through the manager, as every queue of the
   // server's is removed. Two queues are removed side by side while each runs a
   // twenty-second task; the second is given a stand-in to keep alive (an
   // io_context and a counter, which its task holds by reference as the IOCP
   // queue's tasks hold the TCP/IP service's context), which the test lets go
   // of right after the remove, as Application::StopServers_ lets go of the
   // TCP/IP service. It holds that each remove returns while its task still
   // runs, that both threads are counted (WorkQueue::GetThreadsLeft), that both
   // queues and the stand-in are kept while the tasks run, that the stand-in is
   // not freed before its queue, and that a sweep once the threads have left
   // frees both queues and the stand-in. A queue freed under its thread, or a
   // stand-in freed under the task, is a use of freed memory: under
   // AddressSanitizer a reported fault, and on Windows, whose io_context's
   // destructor waits for the task, a remove that returns only once the task
   // has ended.
   //
   // Run inside the server on Windows by ClassTester, and on Linux by
   // workqueue-teardown-tests, in the ordinary build and under
   // AddressSanitizer (linux-sanitize). Throws std::logic_error naming what did
   // not hold. About twenty seconds.
   class WorkQueueTeardownTester
   {
   public:
      void Test();
   };
}