// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"
#include "Scheduler.h"

#include "../Threading/WorkQueue.h"

#include "../BO/ScheduledTask.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   Scheduler::Scheduler()
      : Task("Scheduler")
   {  
       
   }

   Scheduler::~Scheduler()
   {

   }

   void 
   Scheduler::DoWork()
   {
      SetIsStarted();

      while (true)
      {
         // Check if there are any tasks to run.
         RunTasks_();
         
         // Wait one minute.
         try
         {
            boost::this_thread::sleep_for(boost::chrono::minutes(1));
         }
         catch (const boost::thread_interrupted&)
         {
            boost::this_thread::disable_interruption disabled;

            boost::lock_guard<boost::recursive_mutex> guard(mutex_);
            scheduled_tasks_.clear();

            return;
         }
      }
   }

   void
   Scheduler::RunTasks_()
   //---------------------------------------------------------------------------
   // DESCRIPTION:
   // Runs every task that is due.
   //
   // The due list is taken under the lock and the tasks are run with it released.
   // Held across the run, as it was, mutex_ was held for the whole of a backup or
   // an ACME renewal, so ScheduleTask blocked behind them and so did the shutdown
   // path that clears the list.
   //
   // Each task runs inside its own barrier. An exception out of any one of them
   // used to leave this function, leave DoWork, and end the scheduler thread: the
   // while (true) loop was abandoned and nothing scheduled ever ran again for the
   // life of the process - no backup, no certificate renewal, no log retention, no
   // greylist cleanup - with nothing logged to say so.
   //---------------------------------------------------------------------------
   {
      // Each due task with how many times its period had been restarted when it was
      // found due (ScheduledTask::GetPeriodRestarts).
      std::vector<std::pair<std::shared_ptr<ScheduledTask>, unsigned int> > due;

      {
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);

         // Whole seconds, as the tasks keep their next runs (ScheduledTask.h): a
         // task due at this second is due now.
         const __int64 now = ScheduledTask::Now();

         for (auto iterTask = scheduled_tasks_.begin(); iterTask != scheduled_tasks_.end(); iterTask++)
         {
            std::shared_ptr<ScheduledTask> pTask = (*iterTask);

            if (pTask->IsDue(now))
               due.push_back(std::make_pair(pTask, pTask->GetPeriodRestarts()));
         }
      }

      for (auto &entry : due)
      {
         std::shared_ptr<ScheduledTask> pTask = entry.first;

         try
         {
            pTask->Run();
         }
         catch (const boost::thread_interrupted&)
         {
            // Shutting down. Passed on so DoWork clears the list and exits.
            throw;
         }
         catch (...)
         {
            // Application log rather than an error record, deliberately: it could not
            // be shown that no scheduled task throws on a stock configuration, and an
            // ERROR entry that did fire there would break every fixture after it.
            LOG_APPLICATION(Formatter::Format("Scheduled task {0} ended in an exception. It has been contained; the scheduler is still running.",
                                              pTask->GetName()));
         }

         // Rescheduled whatever happened, so a task that fails once is retried on its
         // own interval rather than run again on every tick. Under the lock, because
         // SetTaskInterval writes the same time from a thread that saved a setting -
         // and not at all when it did so while the task ran: RestartPeriod has set the
         // next run from now already, and moving it on from there would put the
         // first run after the save at twice the new period.
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);
         if (pTask->GetPeriodRestarts() == entry.second)
            pTask->SetNextRunTime();
      }
   }

   void
   Scheduler::SetTaskInterval(std::shared_ptr<ScheduledTask> pTask, int minutes)
   {
      if (!pTask)
         return;

      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      if (pTask->GetMinutesBetweenRun() == minutes)
         return;

      // From now, as a start would take it, so a shorter period is not left
      // waiting out the rest of the longer one.
      pTask->RestartPeriod(minutes);
   }

   void
   Scheduler::ScheduleTask(std::shared_ptr<ScheduledTask> pTask)
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      // If task should only be run once, run it now.
      if (pTask->GetReoccurance() == ScheduledTask::RunOnce)
      {
         // GetMaintenanceWorkQueue reports HM5118 and answers an empty pointer
         // while the servers are being torn down; this used to dereference it.
         // A start-up task queued into a shutdown has nothing to run on, and
         // the next start queues it again.
         std::shared_ptr<WorkQueue> maintenanceQueue = Application::Instance()->GetMaintenanceWorkQueue();
         if (!maintenanceQueue)
         {
            LOG_DEBUG("Scheduler::ScheduleTask - The maintenance queue is not available, so a run-once task is dropped: " + pTask->GetName());
            return;
         }

         maintenanceQueue->AddTask(pTask);
         return;
      }

      scheduled_tasks_.push_back(pTask);
     
   }


}
