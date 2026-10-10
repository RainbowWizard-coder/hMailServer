// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows.Controls;
using System.Windows.Threading;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// A setting found by the Ctrl+K palette opens on its tab, scrolled to, with the
   /// keyboard in it and its row ringed once (review finding F274): "MaxMind country
   /// database" opened Auto-ban on its Auto-ban tab, three tabs from the setting,
   /// with nothing marked.
   /// </summary>
   public partial class ServerSettingsView : IRevealSetting
   {
      public bool RevealSetting(string key, string label)
      {
         if (string.IsNullOrEmpty(key) || tabs_ == null)
            return false;

         // Every row of the key, with its tab: Auto-ban's four CountryPolicy rows share
         // one key and one automation id, and only their labels tell them apart.
         List<(TabDef Tab, ComSetting Setting)> rows = tabs_
            .SelectMany(definition => definition.Cards.SelectMany(card => card.Settings).Select(candidate => (Tab: definition, Setting: candidate)))
            .Where(row => string.Equals(row.Setting.Path, key, StringComparison.OrdinalIgnoreCase))
            .ToList();
         if (rows.Count == 0)
            return false;

         (TabDef tab, ComSetting setting) = rows
            .Where(row => SettingReveal.SameLabel(row.Setting.Label, label == null ? null : L(label)))
            .DefaultIfEmpty(rows[0])
            .First();
         bool shared = rows.Count > 1;

         // The tab the page built for this definition: matched by its header,
         // as the page builds one tab item per definition in order.
         TabItem item = SettingsTabs.Items.OfType<TabItem>()
            .FirstOrDefault(candidate => string.Equals(candidate.Header as string, tab.Header, StringComparison.Ordinal));
         if (item != null)
            SettingsTabs.SelectedItem = item;

         // After the tab's content is laid out: an unselected tab's content is
         // not in the visual tree to be found. A key more than one row shares is
         // found by the row's caption, since its automation id names the first.
         Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
            SettingReveal.Show(shared
               ? SettingReveal.Find(SettingsTabs, null, setting.Label) ?? SettingReveal.Find(SettingsTabs, setting.Path, setting.Label)
               : SettingReveal.Find(SettingsTabs, setting.Path, setting.Label))));
         return true;
      }
   }
}
