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
      // Writes the value the row records as the old one, through the setter
      // that writes the column the server reads the setting from - SetString,
      // SetLong, or SetBool for a switch - so the put-back is itself recorded as
      // a change by whoever asked for it. `restored` is the value written ("true"
      // or "false" for a switch), or "(default)" for a [Settings] key returned to
      // its default. False with `error` when the row is not a setting's change,
      // when the trail does not carry the value (a secret, a value longer than
      // the trail keeps, values holding " -> " where the row does not say which
      // arrow divides them), when a number's row holds no number or a
      // [Settings] key's a value the store cannot hold, when the setting no
      // longer exists, or when the write failed. A write to one of
      // the live update's five trust keys (IniSettingStore::IsTrustAnchorName)
      // or to a setting the server's environment sets is refused by the store
      // it goes through, which throws EnvironmentSettings::Refusal unless the
      // value is the one the setting has already; COM and REST answer that
      // with its sentence, as they do for every other change of such a setting.
      static bool PutBack(const AuditTrail::Entry &entry, String &restored, String &error);
   };
}
