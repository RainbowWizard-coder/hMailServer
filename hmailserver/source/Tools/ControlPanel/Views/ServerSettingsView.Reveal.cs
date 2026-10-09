// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using System.Windows.Controls;
using System.Windows.Threading;

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
      public bool RevealSetting(string key)
      {
         if (string.IsNullOrEmpty(key) || tabs_ == null)
            return false;

         foreach (TabDef tab in tabs_)
         {
            ComSetting setting = tab.Cards.SelectMany(card => card.Settings)
               .FirstOrDefault(candidate => string.Equals(candidate.Path, key, StringComparison.OrdinalIgnoreCase));
            if (setting == null)
               continue;

            // The tab the page built for this definition: matched by its header,
            // as the page builds one tab item per definition in order.
            TabItem item = SettingsTabs.Items.OfType<TabItem>()
               .FirstOrDefault(candidate => string.Equals(candidate.Header as string, tab.Header, StringComparison.Ordinal));
            if (item != null)
               SettingsTabs.SelectedItem = item;

            // After the tab's content is laid out: an unselected tab's content is
            // not in the visual tree to be found.
            Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
               SettingReveal.Show(SettingReveal.Find(SettingsTabs, setting.Path, setting.Label))));
            return true;
         }

         return false;
      }
   }
}
