// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include "../Threading/Task.h"

namespace HM
{
   class MailboxRestore;

   class BackupTask : public Task
   {
   public:
      BackupTask(bool bDoBackup);
      ~BackupTask(void);

      virtual void DoWork();

      void SetBackupToRestore(std::shared_ptr<Backup> pBackup);

      // The third kind of run: one mailbox, or one message, put back from an
      // archive (MailboxRestore). On this task rather than a task of its own so
      // that it holds the same run slot a whole restore holds, and two restores
      // - or a restore and a backup - never overlap.
      void SetMailboxRestore(std::shared_ptr<MailboxRestore> job);

   private:

      bool do_backup_;

      std::shared_ptr<Backup> backup_;
      std::shared_ptr<MailboxRestore> mailbox_restore_;
   };
}