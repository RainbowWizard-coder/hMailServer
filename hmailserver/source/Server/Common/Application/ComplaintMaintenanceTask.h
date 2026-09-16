// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../BO/ScheduledTask.h"

namespace HM
{
   // The feedback loops' sweep, hourly and once at start: complaints older than
   // ComplaintRetentionDays, and the daily tally rows behind them.
   //
   // It runs whether or not ComplaintProcessingEnabled is on, because switching the
   // feature off must not leave a record of who complained about what sitting there
   // for ever; on an installation that never switched it on, the pass is two
   // indexed deletes that count nothing and delete nothing.
   class ComplaintMaintenanceTask : public ScheduledTask
   {
   public:
      ComplaintMaintenanceTask(void);
      ~ComplaintMaintenanceTask(void);

      virtual void DoWork();
   };
}
