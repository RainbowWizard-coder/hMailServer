// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Threading;
using hMailServer.ControlPanel.Services;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel
{
   /// <summary>
   /// The help pane follows the page's tab as well as the page (review finding
   /// F270): a tab with a document of its own - the Logging page's SNMP, the
   /// anti-spam page's Greylisting, the auto-ban page's Countries - opens that
   /// document while it is the tab showing (Services/HelpMap.Tabs), and the
   /// page's own entry is shown on its other tabs. The pane was told the page
   /// alone, so the Logging page's four tabs all opened the syslog document.
   /// </summary>
   public partial class MainWindow
   {
      private void WireHelpTabs_()
      {
         // A page's tab strip changing its tab, bubbling out of the page; a list's
         // or a box's selection raises the same event and is passed over.
         ContentHost.AddHandler(Selector.SelectionChangedEvent, new SelectionChangedEventHandler((s, e) =>
         {
            if (helpOpen_ && e.OriginalSource is TabControl)
               ShowHelpForPage_();
         }));
      }

      /// <summary>The help for the page on screen, on the tab it is showing.</summary>
      private void ShowHelpForPage_()
      {
         if (currentPage_ == null)
            return;

         HelpPanel.ShowPage(currentPage_, HelpTabOf_(currentPage_));
      }

      /// <summary>
      /// Shows the help for the page just opened, and once more when it has been
      /// laid out: a page opened for the first time has no tab strip to read until
      /// then, and one reached by Back is put back on its tab then.
      /// </summary>
      private void ShowHelpForNewPage_(string key)
      {
         ShowHelpForPage_();
         Dispatcher.BeginInvoke(DispatcherPriority.ContextIdle, new Action(() =>
         {
            if (helpOpen_ && string.Equals(currentPage_, key, StringComparison.OrdinalIgnoreCase))
               ShowHelpForPage_();
         }));
      }

      /// <summary>
      /// The English header of the tab the page on screen shows, when the help map
      /// has an entry for that tab; null otherwise, for the page's own entry. The
      /// tab is told by its header in the language in force, which is the English
      /// header's translation.
      /// </summary>
      private string HelpTabOf_(string key)
      {
         if (PageTabs_(ContentHost.Content as DependencyObject)?.SelectedItem is not TabItem item || item.Header is not string header)
            return null;

         return HelpMap.TabsOf(key).FirstOrDefault(tab => string.Equals(L(tab), header, StringComparison.Ordinal));
      }
   }
}
