// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "DkimRotationTask.h"

#include "../AntiSpam/DKIM/DkimRotation.h"
#include "../Util/AuditTrail.h"

#include <ctime>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   DkimRotationTask::DkimRotationTask()
   {
   }

   DkimRotationTask::~DkimRotationTask()
   {
   }

   void
   DkimRotationTask::DoWork()
   {
      // A switch moves the domain's selectors, which is a change to hm_domains the
      // audit trail records - but only with an actor on the thread. The server's
      // own writes run with none, so without this scope a key that stopped signing
      // at 03:00 would be a change nobody made.
      AuditTrail::Actor actor;
      actor.present = true;
      actor.name = _T("DKIM key rotation");
      actor.kind = _T("system");
      actor.interface_name = _T("Scheduler");

      AuditScope scope(actor);

      DkimRotation::RunSummary summary = DkimRotation::RunAll((__int64) time(nullptr));

      if (summary.generated > 0 || summary.switched > 0 || summary.retired > 0 || summary.errors > 0)
      {
         LOG_APPLICATION(Formatter::Format("DKIM rotation: a scheduled pass over {0} domain(s) generated {1} key(s), switched {2}, retired {3}, with {4} error(s).",
                                           summary.domains, summary.generated, summary.switched, summary.retired, summary.errors));
      }
   }
}
