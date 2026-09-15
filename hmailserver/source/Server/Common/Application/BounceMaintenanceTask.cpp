// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "BounceMaintenanceTask.h"

#include "../../SMTP/BounceTracker.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   BounceMaintenanceTask::BounceMaintenanceTask(void)
   {

   }

   BounceMaintenanceTask::~BounceMaintenanceTask(void)
   {

   }

   void
   BounceMaintenanceTask::DoWork()
   {
      int suppressionsExpired = 0;
      int countsPruned = 0;
      int reportKeysPruned = 0;

      if (!BounceTracker::RunMaintenance(suppressionsExpired, countsPruned, reportKeysPruned))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6624, "BounceMaintenanceTask::DoWork",
            "The suppression list's maintenance sweep failed, so expired entries, old bounce counts or old report keys may remain until the next hourly pass.");
      }

      if (suppressionsExpired + countsPruned + reportKeysPruned > 0)
      {
         LOG_APPLICATION(Formatter::Format(_T("Bounces: the maintenance sweep removed {0} expired suppression entr(ies), {1} old bounce count(s) and {2} old report key(s)."),
            suppressionsExpired, countsPruned, reportKeysPruned));
      }
   }
}
