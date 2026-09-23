// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   /// <summary>
   /// The profiles as data (docs/Profiles.md): the tables ProfilesData.cpp holds,
   /// which build/generate-profiles.py writes from Common/Application/Profiles/*.json
   /// and holds to them with --check. The includes are resolved there - a profile's
   /// entries here are every entry it applies, in the order it applies them - and
   /// every name is a setting a profile may set, every value one its setting takes,
   /// and every caution the one the entry earns. Profiles (Profiles.h) is what reads
   /// them; nothing else should.
   /// </summary>
   namespace ProfilesData
   {
      enum Store
      {
         StoreIni = 0,        // a [Settings] key, hm_inisettings
         StoreDatabase = 1    // an hm_settings property
      };

      /// <summary>
      /// What an entry does that the server asks an explicit confirmation for
      /// before it applies it to a server where it would change something.
      /// </summary>
      enum Caution
      {
         CautionNone = 0,
         CautionClosesListener = 1,
         CautionTrustsAddresses = 2,
         CautionTurnsFilterOff = 3
      };

      struct Entry
      {
         Store store;
         const TCHAR *name;
         // The value as text: a whole number, 1 or 0 for a switch, or the text.
         const TCHAR *value;
         // One clause: why a server of this kind wants it.
         const TCHAR *why;
         Caution caution;
      };

      struct Profile
      {
         const TCHAR *name;
         const TCHAR *title;
         const TCHAR *summary;
         // The profile whose entries come first, or empty.
         const TCHAR *includes;
         // What the profile cannot do for the administrator, one sentence each.
         const TCHAR *const *steps;
         size_t stepCount;
         const Entry *entries;
         size_t entryCount;
      };

      extern const Profile Profiles[];
      extern const size_t ProfileCount;
   }
}
