// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The rows of a migration (schema 6090): hm_migrations, one per batch;
// hm_migrationjobs, one per mailbox; hm_migrationfolders, the resume state of
// one source folder of one job. What the rows mean, and who writes them when, is
// in Application/MigrationManager.h; this is the SQL and nothing else, so that
// the REST routes and the runner read and write the same columns the same way.
//
// Times are UTC seconds, 0 for "not yet", as the newer stores keep them - a
// bigint compares the same on every backend and needs no parsing. A job's
// secret - the source password or the OAuth refresh token - is protected at
// rest exactly as a fetch account's password and refresh token are
// (Crypt::ProtectSecret), and is read back only by GetJob for the runner and the
// sign-in; the listings never carry it. Neither ntext column (the secret, the
// report) is ever named in a WHERE or ORDER BY clause: SQL Server Compact refuses
// both for ntext.

#pragma once

#include <vector>

namespace HM
{
   struct MigrationRecord
   {
      MigrationRecord() : id(0), kind(0), port(993), security(1), concurrency(4), messagesPerMinute(0),
                          state(0), created(0), startedAt(0), finishedAt(0) { }

      __int64 id;
      String name;
      int kind;                // MigrationStore::Kind
      String host;
      int port;
      int security;            // ConnectionSecurity
      String oauthProvider;    // "google", "microsoft", or empty for a password
      String davUrl;           // where CardDAV and CalDAV are, when not https://<host>/
      int concurrency;         // jobs of this migration at once
      int messagesPerMinute;   // per job; 0 is no limit
      int state;               // MigrationStore::MigrationState
      __int64 created;
      __int64 startedAt;
      __int64 finishedAt;
   };

   struct MigrationJobRecord
   {
      MigrationJobRecord() : id(0), migrationId(0), targetAccountId(0), state(0), phase(0), foldersTotal(0),
                             foldersDone(0), messagesTotal(0), messagesDone(0), bytesDone(0), skipped(0),
                             attempts(0), nextAttempt(0), startedAt(0), finishedAt(0) { }

      __int64 id;
      __int64 migrationId;
      String sourceUser;
      String sourceSecret;     // in the clear in memory, protected in the row; GetJob only
      String signInHash;       // SHA-256 of the sign-in link's key while the job waits for its owner
      __int64 targetAccountId;
      int state;               // MigrationStore::JobState
      int phase;               // MigrationStore::Phase
      int foldersTotal;
      int foldersDone;
      int messagesTotal;
      int messagesDone;
      __int64 bytesDone;
      int skipped;
      String lastError;
      int attempts;
      __int64 nextAttempt;
      __int64 startedAt;
      __int64 finishedAt;
      String report;           // JSON; GetJob and ListJobs(withReport) only
   };

   struct MigrationFolderRecord
   {
      MigrationFolderRecord() : id(0), jobId(0), lastUid(0), messages(0), done(false) { }

      __int64 id;
      __int64 jobId;
      String folder;           // the source's name, as LIST sent it
      String targetFolder;     // the path here, in this server's delimiter

      // What says the source folder is still the one the mark was written
      // against: an IMAP folder's UIDVALIDITY, as decimal text - or, for a
      // source with no UIDs, whatever fingerprint that source's reader keeps
      // (text, at most 64 characters). Empty until the folder is first opened.
      String uidValidity;

      // The highest UID dealt with under that UIDVALIDITY - or the highest of
      // whatever ascending number another source's reader walks a folder by.
      __int64 lastUid;
      int messages;            // copied into the target folder by this job
      bool done;
   };

   class MigrationStore
   {
   public:
      enum Kind
      {
         KindImap = 0,
         KindGoogle = 1,
         KindMicrosoft = 2
      };

      enum MigrationState
      {
         MigrationNew = 0,
         MigrationRunning = 1,
         MigrationPaused = 2,
         MigrationFinished = 3
      };

      enum JobState
      {
         JobQueued = 0,
         JobRunning = 1,
         JobPaused = 2,
         JobDone = 3,
         JobFailed = 4,
         JobCancelled = 5
      };

      enum Phase
      {
         PhaseMail = 0,
         PhaseContacts = 1,
         PhaseCalendars = 2,
         PhaseReport = 3
      };

      // The longest a column holds, checked by the REST routes before a write
      // so that nothing is cut short on one backend and refused on another.
      static const int MaximumNameLength = 255;
      static const int MaximumHostLength = 255;
      static const int MaximumUserLength = 255;
      static const int MaximumErrorLength = 1000;
      static const int MaximumFolderLength = 1024;

      // The report is kept under what MySQL's text column holds (65,535 bytes).
      static const int MaximumReportBytes = 60000;

      static __int64 Now();

      static bool CreateMigration(MigrationRecord &migration);
      static bool GetMigration(__int64 id, MigrationRecord &migration);
      static bool ListMigrations(std::vector<MigrationRecord> &migrations);
      static bool UpdateMigration(const MigrationRecord &migration);

      // The migration, its jobs and their folder rows. The mail copied stays
      // where it was copied to.
      static bool DeleteMigration(__int64 id);

      // auditName, when given, is what the audit trail names the row by -
      // "anna@old.example -> anna@example.com" - since the row itself holds an
      // account id; the writes a route makes give it, the runner's (which no
      // actor is behind) do not.
      static bool CreateJob(MigrationJobRecord &job, const String &auditName = String());

      // One job, with its secret in the clear and its report.
      static bool GetJob(__int64 id, MigrationJobRecord &job);

      // A migration's jobs in the order they were added, without the secret;
      // with the report only when asked, since five hundred of them are large.
      static bool ListJobs(__int64 migrationId, bool withReport, std::vector<MigrationJobRecord> &jobs);

      // Every job queued, running or paused, of every migration, without the
      // secret or the report: what the runner chooses from.
      static bool ListActiveJobs(std::vector<MigrationJobRecord> &jobs);

      // Everything but the secret, the sign-in hash, the source user, the
      // target and the migration - the columns a job's life changes.
      static bool UpdateJob(const MigrationJobRecord &job, const String &auditName = String());

      // The runner's claim on a job: its state set to running and nothing
      // else, so that the report and the counters the row holds are kept.
      static bool MarkJobRunning(__int64 jobId);

      // Every job of a migration in one state moved to another, its next
      // attempt cleared: a migration resumed puts its paused jobs back in the
      // queue this way.
      static bool ChangeJobStates(__int64 migrationId, int fromState, int toState);

      // The secret, protected; clearSignIn empties the sign-in hash with it,
      // so a link works once.
      static bool SaveSecret(__int64 jobId, const String &secret, bool clearSignIn);

      // A new sign-in link's key hash.
      static bool SaveSignInHash(__int64 jobId, const String &hash);

      // Jobs a stopped service left running are queued again: the service's
      // start is the only thing that runs them, so none is running now.
      static int RequeueRunningJobs();

      // How many jobs of a migration are queued, running or paused.
      static bool CountActiveJobs(__int64 migrationId, int &count);

      // A job not yet finished - queued, running or paused, in any migration -
      // copying this source user into this account; jobId 0 when there is none.
      static bool FindActiveJob(__int64 targetAccountId, const String &sourceUser, __int64 &jobId);

      // Every unfinished job copying into an account, cancelled with the
      // reason: the account is being deleted, and its number will be given to
      // another account one day.
      static bool CancelJobsOfAccount(__int64 accountId, const String &reason);

      static bool ListFolders(__int64 jobId, std::vector<MigrationFolderRecord> &folders);

      // Inserts the row when its id is 0, else updates it.
      static bool SaveFolder(MigrationFolderRecord &folder);

      // The folder rows of a job, for a retry that starts it again.
      static bool DeleteFolders(__int64 jobId);
   };
}
