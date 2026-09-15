// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../BO/ScheduledTask.h"

namespace HM
{
   // The suppression list's three sweeps, hourly and once at start: entries whose
   // own expiry has passed or which are older than BounceSuppressionExpiryDays,
   // counts older than BounceCountRetentionDays with no entry behind them, and
   // report keys older than BounceReportWindowDays.
   //
   // It runs whether or not BounceProcessingEnabled is on, because switching the
   // feature off must not leave expired entries and old keys behind for ever; on
   // an installation that never switched it on, the three are indexed deletes that
   // count nothing and delete nothing.
   class BounceMaintenanceTask : public ScheduledTask
   {
   public:
      BounceMaintenanceTask(void);
      ~BounceMaintenanceTask(void);

      virtual void DoWork();
   };
}
