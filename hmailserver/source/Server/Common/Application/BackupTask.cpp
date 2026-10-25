// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "StdAfx.h"
#include "./BackupTask.h"
#include "BackupExecuter.h"
#include "BackupManager.h"
#include "MailboxRestore.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   BackupTask::BackupTask(bool bDoBackup) :
      Task("BackupTask"),
      do_backup_(bDoBackup)
   {
   }

   BackupTask::~BackupTask(void)
   {
   }

   void
   BackupTask::DoWork()
   {
      // The run's outcome is reported once the run slot is free, so that whatever
      // acts on it - a backup started on the strength of the log line, or from the
      // event's own script - is not refused as "already started" (upstream #668;
      // BackupManager::OutcomeHold says how).
      BackupManager::OutcomeHold outcome;

      bool restored = false;

      if (mailbox_restore_)
      {
         mailbox_restore_->Run();
      }
      else
      {
         BackupExecuter oBE;
         if (do_backup_)
         {
            oBE.StartBackup();
         }
         else
         {
            restored = oBE.StartRestore(backup_);
         }
      }

      Application::Instance()->GetBackupManager()->OnThreadStopped();

      // A restore's success is a line in the backup log and no event; it waits for
      // the slot as the others do.
      if (restored)
         Logger::Instance()->LogBackup("Restore completed successfully.");

      outcome.Release();
   }


   void
   BackupTask::SetBackupToRestore(std::shared_ptr<Backup> pBackup)
   {
      backup_ = pBackup;
   }

   void
   BackupTask::SetMailboxRestore(std::shared_ptr<MailboxRestore> job)
   {
      mailbox_restore_ = job;
   }
}