// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A setting put back to the value its audit row says it had. See
// AuditRevert.cpp for what can and cannot be put back from the trail.
#pragma once

#include "AuditTrail.h"

namespace HM
{
   class AuditRevert
   {
   public:
      // Writes the value the row records as the old one, through the same
      // setter the change came through, so the put-back is itself recorded as
      // a change by whoever asked for it. `restored` is the value written, or
      // "(default)" for a [Settings] key returned to its default. False with
      // `error` when the row is not a setting's change, when the trail does
      // not carry the value (a secret, a value longer than the trail keeps),
      // when the setting no longer exists, or when the write failed.
      static bool PutBack(const AuditTrail::Entry &entry, String &restored, String &error);
   };
}
