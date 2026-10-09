// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "ScheduledTask.h"

#include <ctime>
#include <stdexcept>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   ScheduledTask::ScheduledTask(void) :
      Task("ScheduledTask"),
      reoccurance_(RunOnce),
      next_run_time_(0),
      minutes_between_run_(0),
      period_restarts_(0)
   {

   }

   ScheduledTask::~ScheduledTask(void)
   {
   }

   __int64
   ScheduledTask::Now()
   {
      return (__int64) ::time(nullptr);
   }

   int
   ScheduledTask::GetMinutesBetweenRun() const
   {
      return minutes_between_run_;
   }

   void
   ScheduledTask::SetMinutesBetweenRun(int iNewVal)
   {
      minutes_between_run_ = iNewVal;

      SetNextRunTime();
   }

   void
   ScheduledTask::RestartPeriod(int minutes)
   {
      RestartPeriod(minutes, Now());
   }

   void
   ScheduledTask::RestartPeriod(int minutes, __int64 now)
   {
      minutes_between_run_ = minutes;

      next_run_time_ = now + (__int64) minutes_between_run_ * 60;

      period_restarts_++;
   }

   unsigned int
   ScheduledTask::GetPeriodRestarts() const
   {
      return period_restarts_;
   }

   ScheduledTask::Reoccurance
   ScheduledTask::GetReoccurance() const
   {
      return reoccurance_;
   }

   void
   ScheduledTask::SetReoccurance(Reoccurance ro)
   {
      reoccurance_ = ro;
   }

   bool
   ScheduledTask::IsDue(__int64 now) const
   {
      return next_run_time_ <= now;
   }

   __int64
   ScheduledTask::GetNextRunTime() const
   {
      return next_run_time_;
   }

   void
   ScheduledTask::SetNextRunTime()
   {
      SetNextRunTime(Now());
   }

   void
   ScheduledTask::SetNextRunTime(__int64 now)
   //---------------------------------------------------------------------------
   // DESCRIPTION:
   // Moves the next run one interval on, measured from the run that was DUE rather
   // than from now.
   //
   // This is called after the task has returned, so "now plus the interval" made the
   // real period interval + however long the task took, and every pass drifted by its
   // own duration. A six-hourly log retention pass taking two minutes lost an hour a
   // fortnight; the message-store consistency check, which can run for a long time on
   // a large store, drifted much faster. Nothing was ever skipped, so nothing ever
   // looked wrong - the runs just wandered away from the times an administrator set.
   //
   // If the due time is already more than one interval in the past - the task
   // overran its own interval, or the process was suspended - the next run is taken
   // from now instead. Catching up by running back-to-back would be worse than
   // resuming the cadence, and the alternative is a task that never stops running.
   //
   // Whole seconds, added and compared exactly (ScheduledTask.h).
   //---------------------------------------------------------------------------
   {
      const __int64 interval = (__int64) minutes_between_run_ * 60;

      if (next_run_time_ == 0)
      {
         next_run_time_ = now + interval;
         return;
      }

      const __int64 scheduled = next_run_time_ + interval;

      next_run_time_ = (scheduled > now) ? scheduled : (now + interval);
   }

   namespace
   {
      void ExpectSchedule(bool condition, const std::string &what)
      {
         if (!condition)
            throw std::logic_error("ScheduledTask: " + what);
      }

      // A task with nothing to do, for its run times alone.
      class IdleTask : public ScheduledTask
      {
      public:
         void DoWork() override {}
      };
   }

   void
   ScheduledTaskTester::Test()
   {
      // A start in the middle of a second, as the servers' start is: the task is
      // due a minute on, at that second, and not a second before it.
      const __int64 start = 1791572711;  // 2026-10-09 19:05:11 UTC, the start of the failed gate's test

      IdleTask task;
      task.SetReoccurance(ScheduledTask::RunInfinitely);
      task.RestartPeriod(1, start);

      ExpectSchedule(!task.IsDue(start + 59), "a task every minute was due a second early");
      ExpectSchedule(task.IsDue(start + 60), "a task every minute was not due at the second it was due");

      // Run at every turn within its due second, a week of minutes: due at each,
      // never early, and the next run a minute on from the one that was due.
      // The DATE this was kept in until 9 October 2026 came out above the second
      // it meant at the second run already.
      __int64 due = start + 60;
      for (int run = 1; run <= 7 * 24 * 60; run++)
      {
         ExpectSchedule(!task.IsDue(due - 1), "a task every minute was due early at run " + std::to_string(run));
         ExpectSchedule(task.IsDue(due), "a turn within the due second found a task every minute not due at run " + std::to_string(run));

         task.SetNextRunTime(due);
         due += 60;
         ExpectSchedule(task.GetNextRunTime() == due, "the next run did not move on by exactly a minute at run " + std::to_string(run));
      }

      // A turn late within the period keeps the cadence; one later than the
      // period resumes from the turn.
      task.SetNextRunTime(due + 30);
      ExpectSchedule(task.GetNextRunTime() == due + 60, "a run half a minute late moved the cadence");
      due += 60;
      task.SetNextRunTime(due + 600);
      ExpectSchedule(task.GetNextRunTime() == due + 660, "a run ten minutes late did not resume from its turn");

      // The first period of a task set up by its interval counts from now, and a
      // restarted one from the restart.
      IdleTask fresh;
      fresh.SetMinutesBetweenRun(0);
      ExpectSchedule(fresh.GetNextRunTime() > 0, "a task's first run was not set from now");

      IdleTask hourly;
      hourly.RestartPeriod(60, start);
      ExpectSchedule(!hourly.IsDue(start + 3599) && hourly.IsDue(start + 3600), "an hourly task was not due an hour on, to the second");
      const unsigned int restarts = hourly.GetPeriodRestarts();
      hourly.RestartPeriod(5, start + 100);
      ExpectSchedule(hourly.GetNextRunTime() == start + 400 && hourly.GetPeriodRestarts() == restarts + 1, "a restarted period did not count from the restart");
   }
}
