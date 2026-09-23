// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // hm_settingoverrides (schema 6088): a domain's or an account's own value for
   // a setting that may be set below the server (Application/EffectiveSetting.h,
   // docs/SettingLevels.md). One row per (scope, owner, name); no row is the
   // level inheriting. The rows are read through CacheContainer, which keeps
   // one owner's rows at a time and is told here of every write, so that a
   // value is in effect for the next reader the moment it is stored.
   class PersistentSettingOverride
   {
   public:
      enum Scope
      {
         ScopeDomain = 1,
         ScopeAccount = 2
      };

      // Every row of one owner, name to value, the names lower-cased. False
      // when the database did not answer - which is not the same as an owner
      // with no rows, and the caller must not cache it as one.
      static bool ReadOwner(int scope, __int64 ownerId, std::map<String, String> &values);

      // Stores the value, replacing the row the owner has for that name.
      static bool Write(int scope, __int64 ownerId, const String &name, const String &value);

      // Removes the owner's row for the name: the level inherits again.
      static bool Remove(int scope, __int64 ownerId, const String &name);

      // Every row of one owner, for the delete of a domain or an account. Ids are
      // reissued, and a value left behind would become the next owner's.
      static bool DeleteByOwner(int scope, __int64 ownerId);

      // Every row any domain or account holds for the name, (scope, owner) to
      // value: for the reads of one setting across every owner - the sweep that
      // reaches the shortest recover-deleted window, the digest pass asking which
      // accounts have a switch or an hour of their own, the OpenPGP portal's
      // switch. Read through CacheContainer::GetSettingOverridesNamed, never per
      // request. False when the database did not answer.
      static bool RowsOfName(const String &name, std::map<std::pair<int, __int64>, String> &rows);
   };
}
