// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Threading;
using hMailServer.ControlPanel.Views;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel
{
   /// <summary>
   /// A link that names a page's tab opens the page on that tab (review findings
   /// F228 and F251): the stalled-mail guide's SpamAssassin and ClamAV tests, the
   /// ARC trusted sealers on Anti-spam settings' Sender auth tab, SRS and BATV on
   /// Transport security's Forwarding tab - and every other way into a page of tabs
   /// that knows the tab its subject is on: a setup checklist item (SetupItem.Tab),
   /// and a Ctrl+K task or page alias (PaletteRow.Tab).
   /// </summary>
   public partial class MainWindow
   {
      /// <summary>
      /// Opens <paramref name="key"/> as <see cref="NavigateTo(string)"/> does, then
      /// the tab of it headed <paramref name="tab"/> - the English header, as the
      /// page writes it before L() - once the page is laid out. The page stays as it
      /// opened where it has no such tab, and nothing more happens where the page did
      /// not open: a Stay at the question about unsaved changes keeps the reader on
      /// the page they were on.
      /// </summary>
      public void NavigateToTab(string key, string tab) => NavigateToTab(key, tab, false);

      /// <summary>
      /// <see cref="NavigateToTab(string, string)"/>, with <paramref name="fromSearch"/>
      /// marking the arrival as a palette jump, as NavigateTo's does.
      /// </summary>
      private void NavigateToTab(string key, string tab, bool fromSearch)
      {
         NavigateTo(key, fromSearch);
         if (string.IsNullOrEmpty(tab))
            return;

         // After layout: a page opened for the first time has built its tab strip
         // but is not in the visual tree yet, where the strip is looked for.
         Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
         {
            if (string.Equals(currentPage_, key, StringComparison.OrdinalIgnoreCase))
               TabJump.Show(ContentHost.Content as DependencyObject, L(tab));
         }));
      }
   }
}
