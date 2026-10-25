// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include "IPrerequisite.h"

namespace HM
{
   class PrerequisiteList
   {
   public:
      PrerequisiteList(void);
      ~PrerequisiteList(void);

      // Every prerequisite of the step that reaches scriptDatabaseVersion, on the
      // connection the step's script then runs on (inside its transaction, where
      // the walker has one).
      bool Ensure(std::shared_ptr<DALConnection> connection, int scriptDatabaseVersion, String &sErrorMessage);
      bool Ensure(std::shared_ptr<DALConnection> connection, int scriptDatabaseVersion, const PrerequisiteReport &report, String &sErrorMessage);

      // Only those of the step's prerequisites that run before its transaction as
      // well (IPrerequisite::RunsBeforeTransaction), for a walker to call on a
      // connection with no transaction open.
      bool EnsureBeforeTransaction(std::shared_ptr<DALConnection> connection, int scriptDatabaseVersion, const PrerequisiteReport &report, String &sErrorMessage);

   private:

      void Initialize_();
      bool Ensure_(std::shared_ptr<DALConnection> connection, int scriptDatabaseVersion, bool beforeTransactionOnly, const PrerequisiteReport &report, String &sErrorMessage);

      std::vector<std::pair<int, std::shared_ptr<IPrerequisite > > > prerequisites_;

   };
}
