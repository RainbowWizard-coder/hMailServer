// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../BO/ScheduledTask.h"

namespace HM
{
   // The DKIM key rotation's clock: one pass of DkimRotation::RunAll at start-up and
   // every fifteen minutes after. Registered unconditionally, because a rotation
   // started by hand is carried through with every schedule off; with no domain's
   // schedule on and nothing in flight a pass is two selects that find nothing - no
   // lookup and no write.
   //
   // Fifteen minutes because the waits it measures are hours and days, and a record
   // an administrator has just published is looked at within a quarter of an hour
   // without anybody pressing a button. The switch and the retire a pass makes are
   // recorded in the audit trail as the system's, through the scope below.
   class DkimRotationTask : public ScheduledTask
   {
   public:
      DkimRotationTask();
      ~DkimRotationTask();

      virtual void DoWork();
   };
}
