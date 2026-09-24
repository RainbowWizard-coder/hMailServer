// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../BO/ScheduledTask.h"

namespace HM
{
   // The list manager's clock (Util/ListManager.h, Util/ListArchive.h): the
   // daily digests, each in its list's own hour, and once an hour the sweep -
   // the archives trimmed to their retention and the join requests nobody
   // confirmed within a week removed.
   //
   // Every five minutes, as the quarantine digest's task runs, so that a
   // digest goes within five minutes of its hour; ListArchive::SendDigest asks
   // the once-a-day rule itself, so the eleven later ticks in the same hour
   // send nothing. A server with no managed list reads one short query a tick
   // and does nothing else.
   class ListDigestTask : public ScheduledTask
   {
   public:
      ListDigestTask();
      ~ListDigestTask();

      virtual void DoWork();
   };
}
