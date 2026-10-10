// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// A setting found by the Ctrl+K palette opens scrolled to, with the keyboard in
   /// it and its row ringed once (review finding F274), where the page opened at its
   /// top with nothing marked. The setting's editor carries its key as its automation
   /// id. On a section whose cards name tabs - Transport security's Encryption,
   /// Signing, Forwarding and Reports (F228) - the card's tab is chosen first and the
   /// row is looked for in it once laid out, as on ServerSettingsView's pages: the
   /// cards are in the tabs' columns, and the panel they are in on a page without
   /// tabs is empty there, so that a search of it found no row and DKIM
   /// oversigning, found by Ctrl+K, opened on Signing with nothing marked.
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
         bool shared = rows.Count > 1;

         CardDef card = cards_.FirstOrDefault(candidate => candidate.Settings.Contains(setting));
         TabItem item = cardTabs_ == null || card?.Tab == null ? null : cardTabs_.Items.OfType<TabItem>()
            .FirstOrDefault(candidate => string.Equals(candidate.Header as string, card.Tab, StringComparison.Ordinal));
         if (item == null)
         {
            Show_(CardsPanel, setting, shared);
            return true;
         }

         // After the tab's content is laid out: an unselected tab's content is not in
         // the visual tree to be found.
         cardTabs_.SelectedItem = item;
         Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() => Show_(cardTabs_, setting, shared)));
         return true;
      }

      private static void Show_(DependencyObject root, Setting setting, bool shared)
         => SettingReveal.Show(shared
            ? SettingReveal.Find(root, null, setting.Label) ?? SettingReveal.Find(root, setting.Key, setting.Label)
            : SettingReveal.Find(root, setting.Key, setting.Label));
   }
}
