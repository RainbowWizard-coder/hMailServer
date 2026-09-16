// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "ComplaintMaintenanceTask.h"

#include "../../SMTP/ComplaintTracker.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   ComplaintMaintenanceTask::ComplaintMaintenanceTask(void)
   {

   }

   ComplaintMaintenanceTask::~ComplaintMaintenanceTask(void)
   {

   }

   void
   ComplaintMaintenanceTask::DoWork()
   {
      int complaintsPruned = 0;

      if (!ComplaintTracker::RunMaintenance(complaintsPruned))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6934, "ComplaintMaintenanceTask::DoWork",
            "The feedback loops' maintenance sweep failed, so complaints older than ComplaintRetentionDays may remain until the next hourly pass.");
         return;
      }

      if (complaintsPruned > 0)
      {
         LOG_APPLICATION(Formatter::Format(_T("Complaints: the maintenance sweep removed {0} old complaint(s) and tally row(s)."),
            complaintsPruned));
      }
   }
}
