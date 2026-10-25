// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include <functional>

namespace HM
{
   // A line a prerequisite has to say about what it did - how many rows it
   // changed - for the walker that ran it to print or log. SchemaUpgrader passes
   // its own report; the COM route DBUpdater takes passes none, and a
   // prerequisite then writes the line to the application log itself.
   typedef std::function<void(const String &line)> PrerequisiteReport;

   class IPrerequisite
   {
   public:
      virtual ~IPrerequisite() = default;

      virtual int GetDatabaseVersion() = 0;
      virtual bool Ensure(std::shared_ptr<DALConnection> connection, const PrerequisiteReport &report, String &sErrorMessage) = 0;

      // True for a prerequisite that rewrites the rows of a table that may hold
      // millions of them, and whose work stays correct when it is committed as
      // it goes. The walkers run such a prerequisite once BEFORE the step's
      // transaction, on a connection that commits each statement - so an
      // upgrade stopped part-way keeps what was done and the next run resumes
      // where it stopped - and once more inside the transaction, where it finds
      // only what was left or written meanwhile. One that returns true must
      // also do nothing, and succeed, on a database too old for its work: it is
      // called before the steps that bring the database up to it have run.
      virtual bool RunsBeforeTransaction() const { return false; }
   };
}
