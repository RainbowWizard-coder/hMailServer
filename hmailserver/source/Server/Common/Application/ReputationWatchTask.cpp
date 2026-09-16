// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "ReputationWatchTask.h"

#include "../Util/ReputationWatch.h"

#include <ctime>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   ReputationWatchTask::ReputationWatchTask()
   {
   }

   ReputationWatchTask::~ReputationWatchTask()
   {
   }

   void
   ReputationWatchTask::DoWork()
   {
      const __int64 now = (__int64) time(nullptr);

      // Whether it is due and the check itself are decided under one lock. Asked
      // here and answered a statement later, the question would be about a check
      // that an administrator's check on demand may have finished in between, and
      // the day's queries to every list would be made a second time.
      //
      // No audit scope: the check writes hm_reputationchecks, which the trail
      // does not list (it is rewritten at every check), and changes no setting.
      ReputationWatch::RunIfDue(now);
   }
}
