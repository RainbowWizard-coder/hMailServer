// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../BO/ScheduledTask.h"

namespace HM
{
   // The reputation watch's clock: a wake every hour, and a check when the last
   // one was a day ago.
   //
   // The hour is not the period; the day is. A task that simply ran every
   // twenty-four hours would check at start-up on a server that is restarted
   // daily, or never on one restarted more often - the period would be the
   // uptime rather than a day. So the period lives in the rows the last check
   // wrote (ReputationWatch::IsDue), and this wakes often enough to notice.
   //
   // Registered unconditionally. With the watch off, or with no address to check,
   // a wake is one select that finds nothing: the lookups happen only when there
   // is something to look up.
   class ReputationWatchTask : public ScheduledTask
   {
   public:
      ReputationWatchTask();
      ~ReputationWatchTask();

      virtual void DoWork();
   };
}
