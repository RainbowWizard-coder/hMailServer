// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{
   class Backup;
   class MailboxRestore;
   class Task;

   class BackupManager
   {
   public:
      BackupManager(void);
      ~BackupManager(void);

      // What happened to a run requested by the scheduler. Reported to the caller
      // instead of being turned into an error here, because only the caller knows
      // whether a collision means "the backup the schedule wanted is already
      // happening" or "try again shortly".
      enum ScheduledBackupResult
      {
         ScheduledBackupQueued = 1,
         ScheduledBackupAlreadyRunning = 2,
         ScheduledBackupRestoreRunning = 3,
         ScheduledBackupNoWorkQueue = 4
      };

      bool StartBackup();
      // Backups the server according to the settings
      // specified in the configuration.

      // The same work, requested by BackupScheduleTask rather than by an
      // administrator over COM. It exists as a separate entry point because of
      // what it does *not* do: a scheduled run that collides with a backup
      // already in progress is a normal, expected outcome, not a critical error.
      // StartBackup routes that case through OnBackupFailed, which reports
      // ErrorManager::Critical - on a schedule whose interval is shorter than the
      // time a backup takes, that would fill the error log with critical entries
      // describing a server that is working exactly as configured, and would
      // break every fixture's clean-error-log assertion along the way.
      ScheduledBackupResult StartScheduledBackup();

      // Opens an archive and reports what is in it, or nothing at all with the
      // reason in failureReason. The passphrase is the one the caller was given
      // for an encrypted archive, and is empty for an archive that is not
      // encrypted; it is carried on the returned object so that the restore that
      // follows opens the archive the same way, and is stored nowhere.
      //
      // The reason is returned rather than only logged because every caller shows
      // it to somebody: an encrypted archive with no passphrase, a wrong
      // passphrase, an archive from a newer version and a damaged file are four
      // different things to do next, and "the backup could not be loaded" is none
      // of them.
      //
      // archiveOpenTimeoutMilliseconds and openTimedOut are BackupRestorer::
      // ReadArchiveIndex's, passed straight through: zero, the default, waits for
      // the archiver however long it takes, which is right for every caller whose
      // thread exists to do this and wrong for one serving an HTTP request.
      std::shared_ptr<Backup> LoadBackup(const String &sZipFile, const String &passphrase, String &failureReason,
                                         unsigned int archiveOpenTimeoutMilliseconds = 0, bool *openTimedOut = nullptr) const;

      bool StartRestore(std::shared_ptr<Backup> pBackup);
      // Starts a restore of the backup. Restors the part of the
      // backup specified by iRestoreMode.

      // Queues a restore of one mailbox, or one message, prepared by
      // MailboxRestore::Prepare, under the same run slot a whole restore
      // holds. False, with the reason, when a backup or restore is already
      // running or the queue is not there - reported to the caller, which
      // answers 409, and not through OnBackupFailed: a collision between two
      // administrators is not a critical error of the server.
      bool StartMailboxRestore(std::shared_ptr<MailboxRestore> job, String &failureReason);

      // What a restore is about to do, in the words the audit trail records it
      // with: which parts, and whether the archive is an encrypted one.
      //
      // One function for both ways in. The restore itself cannot be recorded -
      // it runs on the maintenance work queue, where no actor is installed, so
      // the deletes and inserts it makes reach the statement chokepoint with
      // nobody to attribute them to - so what IS recorded is the request, by the
      // caller, on the thread that knows who made it. Two call sites writing
      // that row in two different sets of words would make a trail that reads as
      // though COM and REST had done two different things.
      static String DescribeRestore(std::shared_ptr<Backup> backup);

      void OnThreadStopped();
      // Called by backup/restore thread when operation has finished.

      void SetStatus(const String &sStatus);
      String GetStatus();

      // Whether a backup or restore is running now, and whether the last backup
      // that finished succeeded: what an unattended update waits on before it
      // hands the installer over.
      static bool IsRunning();
      bool LastBackupSucceeded() const { return last_backup_succeeded_; }

      void OnBackupCompleted();
      void OnBackupFailed(const String &sReason);

      // THE OUTCOME IS REPORTED ONCE THE RUN SLOT IS FREE (upstream #668)
      //
      // A run ended by reporting its outcome from inside the task - the backup
      // log's "Backup completed successfully" or "BACKUP ERROR:" line, the error,
      // the alert and the OnBackupCompleted or OnBackupFailed script - and only
      // then did BackupTask::DoWork release the run slot. Anything that acted on
      // the outcome found the slot still held: a backup started on the strength of
      // the log line, or from the event's own script, was refused as "already
      // started", for as long as the script ran.
      //
      // BackupTask holds the outcome for the length of the run: an OnBackupCompleted
      // or OnBackupFailed made on its thread while a hold is open records it -
      // LastBackupSucceeded included, which an unattended update reads the moment
      // IsRunning turns false - and Release reports it, after the slot has gone.
      // The first failure a run reports is the one it ends on. A hold destroyed
      // unreleased - an exception through the task - writes the held line to the
      // backup log alone, and runs no script.
      class OutcomeHold
      {
      public:
         OutcomeHold();
         ~OutcomeHold();

         void Release();

      private:
         bool released_;
      };

   private:

      static bool HoldOutcome_(bool failed, const String &reason);

      enum RunKind
      {
         RunBackup = 1,
         RunRestore = 2
      };

      // THE SINGLE-FLIGHT GUARD, AND WHY IT IS PROCESS-WIDE RATHER THAN A MEMBER
      //
      // A backup and a restore both write into the backup destination and both
      // rewrite the same hMailServerBackup.xml and the same DataBackup folder, so
      // exactly one may run at a time. That was previously a plain bool member,
      // read and then written without the mutex, and it was good enough while the
      // only way to start a backup was an administrator clicking twice.
      //
      // Two things break it once a schedule exists:
      //
      //  * The read-then-write race becomes reachable without anybody doing
      //    anything. The COM call arrives on an apartment thread while the
      //    scheduler's request arrives on the scheduler's thread; both could see
      //    "not running" and both queue a backup. Claiming under the mutex fixes
      //    that half.
      //
      //  * BackupManager itself is destroyed and recreated by
      //    Application::ExitInstance/InitInstance on every Reinitialize, while a
      //    backup may still be running. WorkQueue::Stop only waits ten seconds for
      //    its threads (deliberately bounded), and a backup of a real message
      //    store takes far longer than that - so the running BackupTask outlives
      //    the manager that started it, and a fresh manager with a fresh member
      //    flag would happily start a second backup into the same destination
      //    while the first is still copying into it. Per-instance state cannot see
      //    that; process-wide state can.
      //
      // The slot is held as a weak_ptr to the task that owns it rather than as a
      // bool, and that is what makes process-wide state safe. A bool would leak:
      // AddTask can accept a task that never runs (WorkQueue::Stop drops tasks
      // still waiting for a blocking slot), OnThreadStopped would then never be
      // called, and no backup could ever start again for the life of the process.
      // A weak_ptr self-heals - when the task object is destroyed, whether it ran
      // or was dropped, the slot is free again.
      static bool ClaimRunSlot_(std::shared_ptr<Task> owner, RunKind kind);
      static void ReleaseRunSlot_();

      // Whether the operation currently holding the slot is a restore. The
      // scheduler treats the two differently: a backup already in progress is the
      // backup the schedule wanted, while a restore is unrelated work that
      // happens to be in the way - and a restore is precisely when the next
      // backup matters most, so that slot must not be written off.
      static bool RestoreHoldsRunSlot_();

      static boost::mutex run_slot_mutex_;
      static std::weak_ptr<Task> run_slot_owner_;
      static bool run_slot_is_restore_;

      boost::recursive_mutex mutex_;
      String log_;
      bool last_backup_succeeded_ = false;
   };
}
