// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../Threading/Task.h"
#include "../Util/Event.h"

namespace HM
{
   class ScheduledTask;

   class Scheduler : public Task
   {
   public:
      Scheduler();
      virtual ~Scheduler();


      void ScheduleTask(std::shared_ptr<ScheduledTask> pTask);

      // Moves a registered task to a new period, measured from now, under the same
      // lock the due list is taken with. For a setting that decides a task's period
      // and is applied when it is saved (GreylistingRecordExpirationInterval):
      // without the lock the scheduler thread could be reading the next run time
      // while this wrote it.
      void SetTaskInterval(std::shared_ptr<ScheduledTask> pTask, int minutes);

      void DoWork();

   private:

      void RunTasks_();

      boost::recursive_mutex mutex_;
      std::vector<std::shared_ptr<ScheduledTask >> scheduled_tasks_;

   };

}
