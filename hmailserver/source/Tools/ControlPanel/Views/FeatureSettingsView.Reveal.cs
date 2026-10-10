// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using static hMailServer.ControlPanel.Services.Loc;

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
      public bool RevealSetting(string key, string label)
      {
         if (string.IsNullOrEmpty(key) || cards_ == null)
            return false;

         // Every row of the key; where more than one shares it, the label says which,
         // and the row is found by its caption, since the automation id names the first.
         List<Setting> rows = cards_.SelectMany(card => card.Settings)
            .Where(candidate => string.Equals(candidate.Key, key, StringComparison.OrdinalIgnoreCase))
            .ToList();
         if (rows.Count == 0)
            return false;

         Setting setting = rows.FirstOrDefault(candidate => SettingReveal.SameLabel(candidate.Label, label == null ? null : L(label))) ?? rows[0];
         SettingReveal.Show(rows.Count > 1
            ? SettingReveal.Find(CardsPanel, null, setting.Label) ?? SettingReveal.Find(CardsPanel, setting.Key, setting.Label)
            : SettingReveal.Find(CardsPanel, setting.Key, setting.Label));
         return true;
      }
   }
}
