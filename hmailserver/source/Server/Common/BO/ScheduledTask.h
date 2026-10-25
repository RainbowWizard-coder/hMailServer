// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

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

      // Whether the task is due at now, a time() - seconds since 1970, UTC - as
      // the Scheduler reads the clock at each of its turns. The next run is kept
      // in the same whole seconds and compared exactly: a task due at a second is
      // due at a turn within that second. Until 9 October 2026 it was a local
      // DATE, a double counting days, that each run moved on by a minute's span
      // added to it, while the clock it was compared with was composed from the
      // second's fields; the sum came out a rounding error above the second it
      // meant more often than not, so that a turn within the due second found the
      // task not due and it waited a whole turn, a minute, more. And when the
      // clocks went back every task waited an hour, its next run a local time
      // that came round again only an hour later.
      bool IsDue(__int64 now) const;
      __int64 GetNextRunTime() const;

      void SetNextRunTime();
      void SetNextRunTime(__int64 now);

      // A new period taken from now, as a start takes it, rather than from the run
      // that was due: for a period changed while the server runs, so that a shorter
      // one does not first wait out the rest of the longer. The caller holds the
      // scheduler's lock (Scheduler::SetTaskInterval).
      void RestartPeriod(int minutes);
      void RestartPeriod(int minutes, __int64 now);

      // How many times RestartPeriod has run. The scheduler reads it before a run
      // and after, under its lock: a period restarted while the task was running
      // has already set the next run from now, and moving it on again when the run
      // ends would put the first run after the save at twice the period.
      unsigned int GetPeriodRestarts() const;

      // time(), as the Scheduler and the parameterless calls above read it.
      static __int64 Now();

   private:

      Reoccurance reoccurance_;

      // In seconds since 1970, UTC; 0 until a period has been set.
      __int64 next_run_time_;
      int minutes_between_run_;
      unsigned int period_restarts_;

   };

   // The run times' arithmetic in whole seconds (ScheduledTask.cpp): a task due at
   // a second is due at a turn within it, run after run and across a week of them,
   // and one late by more than its period resumes from now.
   class ScheduledTaskTester
   {
   public:
      void Test();
   };
}
