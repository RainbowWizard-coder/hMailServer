// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// A setting found by the Ctrl+K palette opens scrolled to, with the keyboard in
   /// it and its row ringed once (review finding F274), where the page opened at its
   /// top with nothing marked. These pages have no tabs: the setting's card is on
   /// the page, and its editor carries the setting's key as its automation id.
   /// </summary>
   public partial class FeatureSettingsView : IRevealSetting
   {
      public bool RevealSetting(string key)
      {
         if (string.IsNullOrEmpty(key) || cards_ == null)
            return false;

         Setting setting = cards_.SelectMany(card => card.Settings)
            .FirstOrDefault(candidate => string.Equals(candidate.Key, key, StringComparison.OrdinalIgnoreCase));
         if (setting == null)
            return false;

         SettingReveal.Show(SettingReveal.Find(CardsPanel, setting.Key, setting.Label));
         return true;
      }
   }
}
