// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel
{
   /// <summary>
   /// Back and Forward (review finding F013): the pages the administrator has
   /// been on since signing in (Services/NavigationHistory), the Back and Forward
   /// buttons at the start of the breadcrumb bar, Alt+Left and Alt+Right and the
   /// keyboard's own Back and Forward keys (Services/KeyboardShortcuts, answered
   /// in MainWindow.Keys.cs), and the mouse's back and forward buttons. Back
   /// returns to the page and to where it was: its scroll offset and the tab its
   /// first tab strip was on.
   ///
   /// A step goes through NavigateTo as every other way to a page does, so a page
   /// with unsaved changes asks first, and the step is taken - the history moves -
   /// only once the page may be left (NavTree_SelectedItemChanged): Stay leaves
   /// both the page and the history as they were. The mouse's buttons are read
   /// as buttons rather than as the browse commands Windows also raises for them,
   /// and the window binds no NavigationCommands.BrowseBack, so one press is one
   /// step.
   /// </summary>
   public partial class MainWindow
   {
      private readonly NavigationHistory history_ = new NavigationHistory();

      // Which way the navigation under way moves through the history: 0 a visit,
      // -1 Back, +1 Forward. Set around the NavigateTo of a step and read by
      // NavTree_SelectedItemChanged, which takes the step once the page may be left.
      private int historyStep_;

      private void WireHistory_()
      {
         // The mouse's back and forward buttons (XButton1, XButton2), wherever the
         // pointer is in the window, as in a browser.
         PreviewMouseDown += (s, e) =>
         {
            if (e.ChangedButton == MouseButton.XButton1)
               e.Handled = GoBack_();
            else if (e.ChangedButton == MouseButton.XButton2)
               e.Handled = GoForward_();
         };

         UpdateHistoryButtons_();
      }

      private void Back_Click(object sender, RoutedEventArgs e) => GoBack_();

      private void Forward_Click(object sender, RoutedEventArgs e) => GoForward_();

      /// <summary>Back: the page opened before, as it was left. False when there is none.</summary>
      private bool GoBack_() => Step_(-1);

      /// <summary>Forward: the page Back left. False when there is none.</summary>
      private bool GoForward_() => Step_(+1);

      private bool Step_(int step)
      {
         if (!connected_)
            return false;

         HistoryEntry target = step < 0 ? history_.PeekBack : history_.PeekForward;
         if (target == null)
            return false;

         historyStep_ = step;
         try
         {
            NavigateTo(target.Key);
         }
         finally
         {
            historyStep_ = 0;
         }

         return true;
      }

      /// <summary>
      /// The history's part of a navigation that is going ahead: how the page
      /// being left was left, then the step - Back, Forward or a visit. Returns
      /// the entry a step arrived at, whose scroll and tab are to be put back, or
      /// null for a visit.
      /// </summary>
      private HistoryEntry RecordVisit_(string key, DependencyObject leaving)
      {
         if (leaving != null && currentPage_ != null)
            history_.Remember(PageScrollOffset_(leaving), PageTabIndex_(leaving));

         if (historyStep_ < 0)
            return history_.Back();
         if (historyStep_ > 0)
            return history_.Forward();

         history_.Visit(key);
         return null;
      }

      /// <summary>
      /// Puts a page reached by Back or Forward where it was left: its tab first,
      /// then, once that tab is laid out, its scroll offset. After the page has
      /// been entered, which reads it again and may lay it out afresh.
      /// </summary>
      private void RestoreVisit_(HistoryEntry entry, DependencyObject page)
      {
         if (entry == null || page == null)
            return;

         Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
         {
            if (!ReferenceEquals(ContentHost.Content, page))
               return;

            TabControl tabs = PageTabs_(page);
            if (tabs != null && entry.TabIndex >= 0 && entry.TabIndex < tabs.Items.Count)
               tabs.SelectedIndex = entry.TabIndex;

            if (entry.ScrollOffset <= 0)
               return;

            Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
            {
               if (ReferenceEquals(ContentHost.Content, page))
                  PageScroller_(page)?.ScrollToVerticalOffset(entry.ScrollOffset);
            }));
         }));
      }

      /// <summary>The page's first tab strip, or null.</summary>
      private static TabControl PageTabs_(DependencyObject page)
         => PageCommands.Descendants_(page).OfType<TabControl>().FirstOrDefault(t => t.IsVisible);

      private static int PageTabIndex_(DependencyObject page) => PageTabs_(page)?.SelectedIndex ?? -1;

      /// <summary>
      /// The scroll viewer that scrolls the page: the first, outermost first, that
      /// is on screen and can scroll down. A box's own viewer (a text box, the
      /// tab strip's headers) is passed over, since it cannot.
      /// </summary>
      private static ScrollViewer PageScroller_(DependencyObject page)
         => PageCommands.Descendants_(page).OfType<ScrollViewer>().FirstOrDefault(v => v.IsVisible && v.ScrollableHeight > 0);

      private static double PageScrollOffset_(DependencyObject page) => PageScroller_(page)?.VerticalOffset ?? 0;

      /// <summary>
      /// Points the Back and Forward buttons at the pages they open: the page's
      /// title in the tool tip and the accessible name, the key in the tool tip,
      /// and neither button pressable when there is nothing that way.
      /// </summary>
      private void UpdateHistoryButtons_()
      {
         if (BackButton == null || ForwardButton == null)
            return;

         HistoryEntry back = connected_ ? history_.PeekBack : null;
         HistoryEntry forward = connected_ ? history_.PeekForward : null;

         BackButton.IsEnabled = back != null;
         BackButton.ToolTip = back == null ? null : F("Back to {0} (Alt+Left)", L(NavigationMap.TitleOf(back.Key)));
         AutomationProperties.SetName(BackButton, back == null ? L("Go back") : F("Back to {0}", L(NavigationMap.TitleOf(back.Key))));

         ForwardButton.IsEnabled = forward != null;
         ForwardButton.ToolTip = forward == null ? null : F("Forward to {0} (Alt+Right)", L(NavigationMap.TitleOf(forward.Key)));
         AutomationProperties.SetName(ForwardButton, forward == null ? L("Go forward") : F("Forward to {0}", L(NavigationMap.TitleOf(forward.Key))));
      }
   }
}
