// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../Util/VariantDateTime.h"
#include "../Threading/Task.h"

namespace HM
{
   class ScheduledTask : public Task
   {
   public:
      ScheduledTask();
      ~ScheduledTask(void);

      enum Reoccurance
      {
         RunOnce = 1,
         RunInfinitely =2
      };

      int GetMinutesBetweenRun() const;
      void SetMinutesBetweenRun(int iNewVal);

      Reoccurance GetReoccurance() const;
      void SetReoccurance(Reoccurance ro);

      DateTime GetNextRunTime() const;
      void SetNextRunTime();

      // A new period taken from now, as a start takes it, rather than from the run
      // that was due: for a period changed while the server runs, so that a shorter
      // one does not first wait out the rest of the longer. The caller holds the
      // scheduler's lock (Scheduler::SetTaskInterval).
      void RestartPeriod(int minutes);

      // How many times RestartPeriod has run. The scheduler reads it before a run
      // and after, under its lock: a period restarted while the task was running
      // has already set the next run from now, and moving it on again when the run
      // ends would put the first run after the save at twice the period.
      unsigned int GetPeriodRestarts() const;

   private:

      Reoccurance reoccurance_;
      DateTime next_run_time_;
      int minutes_between_run_;
      unsigned int period_restarts_;

   };
}