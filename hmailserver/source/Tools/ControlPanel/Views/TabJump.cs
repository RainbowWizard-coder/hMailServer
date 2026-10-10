// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using System.Windows;
using System.Windows.Controls;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Shows one tab of a page by its header: what a link that names a tab - the
   /// stalled-mail guide's "test SpamAssassin", Transport security's way to the ARC
   /// trusted sealers - opens, where it opened the page on its first tab and left
   /// the reader to find the one it meant (review findings F228 and F251). Every
   /// tabbed page draws a TabControl whose items' headers are their translated
   /// captions, so a header is matched as the page wrote it.
   /// </summary>
   internal static class TabJump
   {
      /// <summary>
      /// Selects the tab headed <paramref name="header"/> (translated) in the first
      /// tab strip of <paramref name="page"/> that has one; false, leaving the page
      /// as it is, when none does.
      /// </summary>
      public static bool Show(DependencyObject page, string header)
      {
         if (page == null || string.IsNullOrEmpty(header))
            return false;

         foreach (TabControl tabs in PageCommands.Descendants_(page).OfType<TabControl>())
         {
            TabItem item = tabs.Items.OfType<TabItem>()
               .FirstOrDefault(candidate => string.Equals(candidate.Header as string, header, StringComparison.Ordinal));
            if (item == null)
               continue;

            tabs.SelectedItem = item;
            return true;
         }

         return false;
      }
   }
}
