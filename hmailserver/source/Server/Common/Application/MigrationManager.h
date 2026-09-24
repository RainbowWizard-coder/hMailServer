// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Migration jobs: many mailboxes copied from another server, each as a job with
// a state, throttled as the source will bear, resumed after a failure or a
// restart, and reported on (docs/Migration.md, "Five hundred mailboxes:
// migration jobs").
//
// THE MODEL (schema 6090, Util/MigrationStore.h). A migration is a batch: the
// source - an IMAP server, Google Workspace or Microsoft 365, its host, port and
// security - how many of its jobs run at once (concurrency, 4 by default) and how
// many messages a minute each may fetch (0 for no limit). A job is one mailbox
// there copied into one account here: the source user, the password or the OAuth
// refresh token, the target account. Its state is queued, running, paused, done,
// failed or cancelled; its phase is mail, contacts, calendars, report.
//
// THE RUNNER. One scheduler thread, started with the servers and stopped with
// them, looks every two seconds - or at once when a route wakes it - for a job it
// may start: queued, or paused with a retry that has come due, of a migration
// that is running, and not waiting for its owner to sign in. It starts one when
// the job's migration runs fewer than its concurrency and the source host has
// fewer than MigrationConnectionsPerHost jobs at it across every migration (and
// never more than MaxRunningJobs in all), each on a thread of its own
// (MigrationJob.h). The scheduler also marks a running migration finished when
// nothing of it is queued, running or paused.
//
// WHO WRITES A RUNNING JOB'S STATE. Its own thread, and nothing else: a route that
// pauses or cancels a running job does not write the row, it signals the thread,
// which stops between two commands, writes what it copied and then the state.
// A route that acts on a job that is not running writes the row itself. Both are
// decided under one lock with the scheduler's choice, so a job cannot be started
// between a route's question and its answer.
//
// A STOP. The service's stop (and a reinitialise) signals every running job to
// stop and waits for them: each writes its counters and its resume marks and
// leaves its state running, as the row says it was when the service went; the
// next start queues every running job again (MigrationStore::RequeueRunningJobs),
// and they go on from their marks. A pause or a cancel asked for and not yet
// read when the stop comes is kept, and the job writes itself paused or
// cancelled instead. A row that says running with no thread of this process
// behind it - its thread ended before it could write what became of it, the
// database away as it tried, a fault - is found by the scheduler and started
// again from its marks.
//
// A FAILURE. A network failure, an HTTP 429 or 5xx from the source or from the
// provider's token endpoint, a source that cannot log anybody on just now, a
// folder it will not open for now, or the database away pauses the job with the
// error and a retry due after 1, 5, 15 and then 60 minutes; after MaxAttempts in
// a row without a message copied between them the job fails. The source
// refusing the credentials themselves fails it at once, since no retry mends a
// wrong password; POST .../retry, with the secret mended, queues it again.
//
// A TARGET DELETED. Deleting the account a job copies into cancels every
// unfinished job of it with the reason, a running one stopped first
// (PersistentAccount::DeleteObject), since account numbers are given again.

#pragma once

#include <atomic>
#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <thread>

namespace HM
{
   class MigrationManager : public Singleton<MigrationManager>
   {
   public:
      MigrationManager();
      ~MigrationManager();

      // What a running job is told, read between any two commands it sends.
      enum Signal
      {
         SignalNone = 0,
         SignalPause = 1,
         SignalCancel = 2,
         SignalStop = 3,
         SignalTargetGone = 4     // cancel: the account it copies into is being deleted
      };

      // The most attempts a job gets before it fails.
      static const int MaxAttempts = 24;

      // The most jobs running at once across every migration, whatever the
      // migrations and the per-host ceiling allow.
      static const int MaxRunningJobs = 64;

      // Minutes before the next attempt after this many failures: 1, 5, 15,
      // then 60.
      static int BackoffMinutes(int attempts);

      // From Application::StartServers and StopServers. Start queues again what
      // a stop left running; Stop signals every running job to stop and waits
      // for it.
      void Start();
      void Stop();

      // Look for a job to start now rather than in up to two seconds.
      void Wake();

      // The routes' verbs. False with the HTTP status the route answers and the
      // sentence it says, when refused.
      bool StartMigration(__int64 migrationId, int &status, String &problem);
      bool PauseMigration(__int64 migrationId, int &status, String &problem);
      bool ResumeMigration(__int64 migrationId, int &status, String &problem);
      bool DeleteMigration(__int64 migrationId, int &status, String &problem);
      bool CancelJob(__int64 jobId, int &status, String &problem);

      // A failed, cancelled or paused job queued again, from the start of its
      // current phase with its marks kept; secretGiven replaces its password
      // or refresh token first.
      bool RetryJob(__int64 jobId, bool secretGiven, const String &secret, int &status, String &problem);

      bool IsJobRunning(__int64 jobId);
      int RunningJobsOf(__int64 migrationId);

      // Jobs were added to a migration: a finished one runs again, decided
      // under the scheduler's lock so that a pass marking it finished at the
      // same moment cannot leave the new jobs waiting.
      void JobsAdded(__int64 migrationId);

      // A job's owner signed in: a failed or cancelled job is queued again, as
      // a retry queues it, and its migration runs again if it had finished.
      void SignedIn(__int64 jobId);

      // The account a job copies into is being deleted: every unfinished job
      // of it is cancelled with the reason, a running one told to stop.
      bool TargetDeleted(__int64 accountId);

   private:
      struct RunningJob
      {
         RunningJob() : job_id(0), migration_id(0), target_account_id(0) { }

         __int64 job_id;
         __int64 migration_id;
         __int64 target_account_id;
         AnsiString host;
         std::shared_ptr<std::atomic<int> > signal;
         std::shared_ptr<std::atomic<bool> > finished;
         std::thread thread;
      };

      void SchedulerThread_();
      void SchedulerLoop_();

      // Under mutex_.
      void SchedulePass_();
      void Reap_(bool all);
      std::shared_ptr<RunningJob> FindRunning_(__int64 jobId);

      // Any thread of this process for the job, finished or not.
      bool HasThread_(__int64 jobId);
      int CountRunning_(__int64 migrationId);
      int CountRunningAt_(const AnsiString &host);
      void SignalMigration_(__int64 migrationId, int signal);

      std::mutex mutex_;
      std::condition_variable wake_;
      bool stopping_;
      bool woken_;
      std::thread scheduler_;
      std::list<std::shared_ptr<RunningJob> > running_;
   };
}
