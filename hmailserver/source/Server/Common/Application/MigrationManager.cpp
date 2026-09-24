// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See MigrationManager.h.

#include "StdAfx.h"
#include "MigrationManager.h"
#include "MigrationJob.h"

#include "ExceptionHandler.h"
#include "../BO/Account.h"
#include "../Cache/CacheContainer.h"
#include "../Util/AuditTrail.h"
#include "../Util/MigrationStore.h"
#include "../Util/Strings/Formatter.h"

#include <chrono>
#include <map>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const int MigrationManager::MaxAttempts;
   const int MigrationManager::MaxRunningJobs;

   namespace
   {
      AnsiString HostKey(const String &host)
      {
         AnsiString key = AnsiString(host);
         key.Trim();
         key.MakeLower();
         return key;
      }

      // A job as the audit trail names it: the mailbox and where it goes.
      String AuditName_(const MigrationJobRecord &job)
      {
         std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(job.targetAccountId);
         return job.sourceUser + _T(" -> ") + (account ? account->GetAddress() : String(_T("(a deleted account)")));
      }
   }

   MigrationManager::MigrationManager() :
      stopping_(true),
      woken_(false)
   {
   }

   MigrationManager::~MigrationManager()
   {
   }

   int
   MigrationManager::BackoffMinutes(int attempts)
   {
      if (attempts <= 1)
         return 1;
      if (attempts == 2)
         return 5;
      if (attempts == 3)
         return 15;
      return 60;
   }

   void
   MigrationManager::Start()
   {
      {
         std::lock_guard<std::mutex> guard(mutex_);
         if (!stopping_)
            return;

         stopping_ = false;
         woken_ = false;
      }

      // Nothing runs a job but this, and nothing was running it a moment ago:
      // a job the last stop left running is queued again and resumes from its
      // marks. A job a crash left running is the same case.
      const int requeued = MigrationStore::RequeueRunningJobs();
      if (requeued > 0)
         LOG_APPLICATION(Formatter::Format(_T("Migrations: {0} job(s) the last stop left running are queued again and resume from their marks."), requeued));

      scheduler_ = std::thread(&MigrationManager::SchedulerThread_, this);
   }

   void
   MigrationManager::Stop()
   {
      {
         std::lock_guard<std::mutex> guard(mutex_);
         if (stopping_)
            return;

         stopping_ = true;

         // A pause or a cancel asked for and not yet read is kept: the job
         // writes itself paused or cancelled, rather than running to be queued
         // again at the next start.
         for (std::shared_ptr<RunningJob> job : running_)
         {
            int expected = SignalNone;
            job->signal->compare_exchange_strong(expected, (int) SignalStop);
         }
      }

      wake_.notify_all();

      if (scheduler_.joinable())
         scheduler_.join();

      // Each job stops between two commands - within a quarter of a second of a
      // read, or before its next DAV or Graph request - writes its counters and
      // its marks, and leaves its row saying running.
      std::lock_guard<std::mutex> guard(mutex_);
      Reap_(true);
   }

   void
   MigrationManager::Wake()
   {
      {
         std::lock_guard<std::mutex> guard(mutex_);
         woken_ = true;
      }
      wake_.notify_all();
   }

   void
   MigrationManager::SchedulerThread_()
   {
      boost::function<void()> loop = boost::bind(&MigrationManager::SchedulerLoop_, this);
      ExceptionHandler::Run("MigrationManager", loop);
   }

   void
   MigrationManager::SchedulerLoop_()
   {
      std::unique_lock<std::mutex> lock(mutex_);

      while (!stopping_)
      {
         Reap_(false);

         try
         {
            SchedulePass_();
         }
         catch (...)
         {
            // A pass that failed - a database that went away - is tried again
            // two seconds later; the jobs already running are theirs.
         }

         wake_.wait_for(lock, std::chrono::seconds(2), [this]() { return stopping_ || woken_; });
         woken_ = false;
      }
   }

   void
   MigrationManager::Reap_(bool all)
   {
      for (std::list<std::shared_ptr<RunningJob> >::iterator it = running_.begin(); it != running_.end();)
      {
         std::shared_ptr<RunningJob> job = *it;
         if (!all && !job->finished->load())
         {
            ++it;
            continue;
         }

         if (job->thread.joinable())
            job->thread.join();

         it = running_.erase(it);
      }
   }

   bool
   MigrationManager::HasThread_(__int64 jobId)
   {
      for (std::shared_ptr<RunningJob> job : running_)
      {
         if (job->job_id == jobId)
            return true;
      }
      return false;
   }

   std::shared_ptr<MigrationManager::RunningJob>
   MigrationManager::FindRunning_(__int64 jobId)
   {
      for (std::shared_ptr<RunningJob> job : running_)
      {
         if (job->job_id == jobId && !job->finished->load())
            return job;
      }
      return std::shared_ptr<RunningJob>();
   }

   int
   MigrationManager::CountRunning_(__int64 migrationId)
   {
      int count = 0;
      for (std::shared_ptr<RunningJob> job : running_)
      {
         if (job->migration_id == migrationId && !job->finished->load())
            count++;
      }
      return count;
   }

   int
   MigrationManager::CountRunningAt_(const AnsiString &host)
   {
      int count = 0;
      for (std::shared_ptr<RunningJob> job : running_)
      {
         if (job->host == host && !job->finished->load())
            count++;
      }
      return count;
   }

   void
   MigrationManager::SignalMigration_(__int64 migrationId, int signal)
   {
      for (std::shared_ptr<RunningJob> job : running_)
      {
         if (job->migration_id == migrationId && !job->finished->load())
            job->signal->store(signal);
      }
   }

   void
   MigrationManager::SchedulePass_()
   {
      std::vector<MigrationRecord> migrations;
      if (!MigrationStore::ListMigrations(migrations))
         return;

      std::map<__int64, MigrationRecord> byId;
      for (const MigrationRecord &migration : migrations)
         byId[migration.id] = migration;

      std::vector<MigrationJobRecord> jobs;
      if (!MigrationStore::ListActiveJobs(jobs))
         return;

      const int perHost = Configuration::Instance()->GetMigrationConnectionsPerHost();
      const __int64 now = MigrationStore::Now();

      std::map<__int64, int> active;
      for (const MigrationJobRecord &job : jobs)
      {
         active[job.migrationId]++;

         std::map<__int64, MigrationRecord>::const_iterator migration = byId.find(job.migrationId);
         if (migration == byId.end() || migration->second.state != MigrationStore::MigrationRunning)
            continue;

         // A row that says running with no thread of this process behind it -
         // the list is read after the finished threads are reaped, and a thread
         // writes what became of its job before it finishes - is one whose
         // thread ended without writing it: started again from its marks.
         bool orphaned = false;
         if (job.state == MigrationStore::JobRunning)
         {
            if (HasThread_(job.id))
               continue;
            orphaned = true;
         }

         // Queued; or paused by a failure with its retry come due. A job an
         // administrator paused has no retry due, and waits to be resumed. A job
         // waiting for its owner's sign-in is not started.
         const bool due = orphaned || job.state == MigrationStore::JobQueued ||
                          (job.state == MigrationStore::JobPaused && job.nextAttempt > 0 && job.nextAttempt <= now);
         if (!due || !job.signInHash.IsEmpty() || FindRunning_(job.id))
            continue;

         const AnsiString host = HostKey(migration->second.host);
         const int concurrency = migration->second.concurrency > 0 ? migration->second.concurrency : 1;

         if ((int) running_.size() >= MaxRunningJobs)
            break;
         if (CountRunning_(job.migrationId) >= concurrency)
            continue;
         if (CountRunningAt_(host) >= perHost)
            continue;

         if (!MigrationStore::MarkJobRunning(job.id))
            continue;

         if (orphaned)
            LOG_APPLICATION(Formatter::Format(_T("Migration {0}: job {1} ({2}) was found running with nothing running it, and is started again from its marks."),
               job.migrationId, job.id, job.sourceUser));

         std::shared_ptr<RunningJob> running = std::shared_ptr<RunningJob>(new RunningJob());
         running->job_id = job.id;
         running->migration_id = job.migrationId;
         running->target_account_id = job.targetAccountId;
         running->host = host;
         running->signal = std::shared_ptr<std::atomic<int> >(new std::atomic<int>(SignalNone));
         running->finished = std::shared_ptr<std::atomic<bool> >(new std::atomic<bool>(false));

         const __int64 jobId = job.id;
         std::shared_ptr<std::atomic<int> > signal = running->signal;
         std::shared_ptr<std::atomic<bool> > finished = running->finished;

         running->thread = std::thread([jobId, signal, finished]()
         {
            boost::function<void()> work = [jobId, signal]()
            {
               MigrationJob job(jobId, signal);
               job.Run();
            };
            ExceptionHandler::Run("MigrationJob", work);
            finished->store(true);
         });

         running_.push_back(running);
      }

      // A running migration with nothing queued, running or paused is finished.
      for (const MigrationRecord &listed : migrations)
      {
         if (listed.state != MigrationStore::MigrationRunning || active[listed.id] > 0 || CountRunning_(listed.id) > 0)
            continue;

         // Read again, with its jobs counted again: a request may have added
         // jobs since the list above was read (JobsAdded, which takes this
         // lock, sets such a migration running again in any case), and what is
         // written is the record as it is now, not as it was.
         MigrationRecord migration;
         int stillActive = 0;
         if (!MigrationStore::GetMigration(listed.id, migration) || migration.state != MigrationStore::MigrationRunning ||
             !MigrationStore::CountActiveJobs(listed.id, stillActive) || stillActive > 0)
            continue;

         migration.state = MigrationStore::MigrationFinished;
         migration.finishedAt = now;
         if (MigrationStore::UpdateMigration(migration))
            LOG_APPLICATION(Formatter::Format(_T("Migration {0} ({1}) has finished: every job of it is done, failed or cancelled."), migration.id, migration.name));
      }
   }

   bool
   MigrationManager::IsJobRunning(__int64 jobId)
   {
      std::lock_guard<std::mutex> guard(mutex_);
      return FindRunning_(jobId) != nullptr;
   }

   int
   MigrationManager::RunningJobsOf(__int64 migrationId)
   {
      std::lock_guard<std::mutex> guard(mutex_);
      return CountRunning_(migrationId);
   }

   bool
   MigrationManager::StartMigration(__int64 migrationId, int &status, String &problem)
   {
      {
         std::lock_guard<std::mutex> guard(mutex_);

         MigrationRecord migration;
         if (!MigrationStore::GetMigration(migrationId, migration))
         {
            status = 404;
            problem = _T("no such migration");
            return false;
         }

         if (migration.state == MigrationStore::MigrationRunning)
         {
            status = 409;
            problem = _T("the migration is running already");
            return false;
         }

         if (migration.state == MigrationStore::MigrationPaused)
         {
            status = 409;
            problem = _T("the migration is paused; resume it");
            return false;
         }

         // A finished migration started again runs what was added to it since,
         // and what was retried.
         migration.state = MigrationStore::MigrationRunning;
         if (migration.startedAt == 0)
            migration.startedAt = MigrationStore::Now();
         migration.finishedAt = 0;

         if (!MigrationStore::UpdateMigration(migration))
         {
            status = 500;
            problem = _T("the migration could not be saved");
            return false;
         }
      }

      Wake();
      return true;
   }

   bool
   MigrationManager::PauseMigration(__int64 migrationId, int &status, String &problem)
   {
      std::lock_guard<std::mutex> guard(mutex_);

      MigrationRecord migration;
      if (!MigrationStore::GetMigration(migrationId, migration))
      {
         status = 404;
         problem = _T("no such migration");
         return false;
      }

      if (migration.state != MigrationStore::MigrationRunning)
      {
         status = 409;
         problem = _T("the migration is not running");
         return false;
      }

      migration.state = MigrationStore::MigrationPaused;
      if (!MigrationStore::UpdateMigration(migration))
      {
         status = 500;
         problem = _T("the migration could not be saved");
         return false;
      }

      // Its running jobs stop between two commands and write themselves
      // paused; its queued ones stay queued and wait for the resume.
      SignalMigration_(migrationId, SignalPause);
      return true;
   }

   bool
   MigrationManager::ResumeMigration(__int64 migrationId, int &status, String &problem)
   {
      {
         std::lock_guard<std::mutex> guard(mutex_);

         MigrationRecord migration;
         if (!MigrationStore::GetMigration(migrationId, migration))
         {
            status = 404;
            problem = _T("no such migration");
            return false;
         }

         if (migration.state != MigrationStore::MigrationPaused)
         {
            status = 409;
            problem = _T("the migration is not paused");
            return false;
         }

         if (CountRunning_(migrationId) > 0)
         {
            status = 409;
            problem = _T("the migration's jobs are still stopping; try again in a moment");
            return false;
         }

         // Every paused job goes back in the queue at once, a retry that was
         // waiting its turn included: the administrator has said go on.
         if (!MigrationStore::ChangeJobStates(migrationId, MigrationStore::JobPaused, MigrationStore::JobQueued))
         {
            status = 500;
            problem = _T("the migration's jobs could not be saved");
            return false;
         }

         migration.state = MigrationStore::MigrationRunning;
         migration.finishedAt = 0;
         if (!MigrationStore::UpdateMigration(migration))
         {
            status = 500;
            problem = _T("the migration could not be saved");
            return false;
         }
      }

      Wake();
      return true;
   }

   bool
   MigrationManager::DeleteMigration(__int64 migrationId, int &status, String &problem)
   {
      std::lock_guard<std::mutex> guard(mutex_);

      MigrationRecord migration;
      if (!MigrationStore::GetMigration(migrationId, migration))
      {
         status = 404;
         problem = _T("no such migration");
         return false;
      }

      if (CountRunning_(migrationId) > 0)
      {
         status = 409;
         problem = _T("a job of this migration is running; pause the migration first");
         return false;
      }

      if (!MigrationStore::DeleteMigration(migrationId))
      {
         status = 500;
         problem = _T("the migration could not be deleted");
         return false;
      }

      return true;
   }

   bool
   MigrationManager::CancelJob(__int64 jobId, int &status, String &problem)
   {
      std::lock_guard<std::mutex> guard(mutex_);

      std::shared_ptr<RunningJob> running = FindRunning_(jobId);
      if (running)
      {
         // Its own thread writes it cancelled once it has stopped, with what it
         // copied kept - with no actor on that thread, so the trail would not
         // know who asked. This is the record of it, made on the request's
         // thread, which carries the actor.
         running->signal->store(SignalCancel);

         MigrationJobRecord job;
         if (MigrationStore::GetJob(jobId, job))
            AuditTrail::Instance()->RecordEvent(_T("migration job"), AuditName_(job), _T("cancelled"),
               _T("cancelled while it was running; what it had copied stays"));
         return true;
      }

      MigrationJobRecord job;
      if (!MigrationStore::GetJob(jobId, job))
      {
         status = 404;
         problem = _T("no such job");
         return false;
      }

      if (job.state == MigrationStore::JobDone || job.state == MigrationStore::JobCancelled)
      {
         status = 409;
         problem = job.state == MigrationStore::JobDone ? String(_T("the job is done")) : String(_T("the job is cancelled already"));
         return false;
      }

      job.state = MigrationStore::JobCancelled;
      job.nextAttempt = 0;
      job.finishedAt = MigrationStore::Now();
      if (!MigrationStore::UpdateJob(job, AuditName_(job)))
      {
         status = 500;
         problem = _T("the job could not be saved");
         return false;
      }

      return true;
   }

   bool
   MigrationManager::RetryJob(__int64 jobId, bool secretGiven, const String &secret, int &status, String &problem)
   {
      {
         std::lock_guard<std::mutex> guard(mutex_);

         if (FindRunning_(jobId))
         {
            status = 409;
            problem = _T("the job is running");
            return false;
         }

         MigrationJobRecord job;
         if (!MigrationStore::GetJob(jobId, job))
         {
            status = 404;
            problem = _T("no such job");
            return false;
         }

         // A job whose row says running with no thread behind it (checked
         // above) is retried as a failed one is: nothing else would run it.
         if (job.state == MigrationStore::JobDone || job.state == MigrationStore::JobQueued)
         {
            status = 409;
            problem = Formatter::Format(_T("the job is {0}; only a failed, cancelled or paused job is retried"), String(AnsiString(MigrationJob::StateWord(job.state))));
            return false;
         }

         if (secretGiven && !MigrationStore::SaveSecret(jobId, secret, true))
         {
            status = 500;
            problem = _T("the job's secret could not be saved");
            return false;
         }

         // From where it stopped: the phase it was in and the marks it wrote are
         // kept, and so is when it first started, which is what the duplicate
         // check reads back from.
         job.state = MigrationStore::JobQueued;
         job.attempts = 0;
         job.nextAttempt = 0;
         job.finishedAt = 0;
         if (!MigrationStore::UpdateJob(job, AuditName_(job)))
         {
            status = 500;
            problem = _T("the job could not be saved");
            return false;
         }

         // A finished migration runs again for it.
         MigrationRecord migration;
         if (MigrationStore::GetMigration(job.migrationId, migration) && migration.state == MigrationStore::MigrationFinished)
         {
            migration.state = MigrationStore::MigrationRunning;
            migration.finishedAt = 0;
            MigrationStore::UpdateMigration(migration);
         }
      }

      Wake();
      return true;
   }

   void
   MigrationManager::JobsAdded(__int64 migrationId)
   {
      {
         std::lock_guard<std::mutex> guard(mutex_);

         MigrationRecord migration;
         if (MigrationStore::GetMigration(migrationId, migration) && migration.state == MigrationStore::MigrationFinished)
         {
            migration.state = MigrationStore::MigrationRunning;
            migration.finishedAt = 0;
            MigrationStore::UpdateMigration(migration);
         }
      }

      Wake();
   }

   void
   MigrationManager::SignedIn(__int64 jobId)
   {
      {
         std::lock_guard<std::mutex> guard(mutex_);

         MigrationJobRecord job;
         if (!FindRunning_(jobId) && MigrationStore::GetJob(jobId, job) &&
             (job.state == MigrationStore::JobFailed || job.state == MigrationStore::JobCancelled))
         {
            // What a retry does, for a job whose owner has now given what it
            // lacked: queued from its marks, its attempts counted afresh.
            job.state = MigrationStore::JobQueued;
            job.attempts = 0;
            job.nextAttempt = 0;
            job.finishedAt = 0;
            job.lastError = String();
            if (MigrationStore::UpdateJob(job))
            {
               MigrationRecord migration;
               if (MigrationStore::GetMigration(job.migrationId, migration) && migration.state == MigrationStore::MigrationFinished)
               {
                  migration.state = MigrationStore::MigrationRunning;
                  migration.finishedAt = 0;
                  MigrationStore::UpdateMigration(migration);
               }
            }
         }
      }

      Wake();
   }

   bool
   MigrationManager::TargetDeleted(__int64 accountId)
   {
      std::lock_guard<std::mutex> guard(mutex_);

      // A running job stops between two commands and writes itself cancelled
      // with the reason; the rest are cancelled here, and a running one's row
      // with them, which its own thread then writes over with the same.
      for (std::shared_ptr<RunningJob> job : running_)
      {
         if (job->target_account_id == accountId && !job->finished->load())
            job->signal->store(SignalTargetGone);
      }

      return MigrationStore::CancelJobsOfAccount(accountId, _T("The target account was deleted."));
   }
}
