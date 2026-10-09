// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using hMailServer.ControlPanel.Views.Scaffold;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The one way a page shows a read that failed.
   ///
   /// A list page whose read threw used to keep whatever it had on screen - its
   /// empty state ("No routes defined yet."), the rows of its last visit, a count
   /// of zero, ticked defaults with Save enabled - and the only thing that said
   /// otherwise was a toast that went away by itself. Every one of those is a
   /// claim the page cannot make: an empty state says the server has nothing,
   /// and a server that could not be read may have anything.
   ///
   /// So a failed read does four things, here, the same on every page: the list
   /// is emptied and its empty state hidden, since neither can be trusted; the
   /// page's notice says what could not be read and why, at the Critical level,
   /// with a Try again button that runs the read once more; and the controls
   /// that write - Add, Restore defaults, Save - are disabled until a read
   /// succeeds, so that nothing is written over a list that was never seen.
   /// <see cref="Clear"/> is the other half: a read that worked hides the
   /// failure and gives the writes back.
   /// </summary>
   public static class ReadFailure
   {
      // What marks a notice as showing a failure of this class's, so that Clear
      // hides that and never a message a page wrote there itself.
      private static readonly object Marker = new();

      /// <summary>
      /// Shows a read that failed with an exception: <paramref name="prefix"/>
      /// ("Could not read the routes: ") followed by the reason as
      /// <see cref="ServerSession.DescribeComError"/> puts it.
      /// </summary>
      public static void Show(InlineNotice notice, EmptyState empty, ItemsControl list, string prefix, Exception why,
         Action tryAgain, params UIElement[] writes)
         => Show(notice, empty, list, prefix, ServerSession.DescribeComError(why), tryAgain, writes);

      /// <summary>
      /// Shows a read that failed: the list emptied, the empty state hidden, the
      /// notice Critical with the sentence and a Try again button that runs
      /// <paramref name="tryAgain"/>, and every element in <paramref name="writes"/>
      /// disabled. A page with no notice of its own passes null and keeps the
      /// rest; one whose rows are not an ItemsSource passes null for the list and
      /// clears them itself.
      /// </summary>
      public static void Show(InlineNotice notice, EmptyState empty, ItemsControl list, string prefix, string why,
         Action tryAgain, params UIElement[] writes)
      {
         if (list != null)
            list.ItemsSource = null;

         if (empty != null)
            empty.Visibility = Visibility.Collapsed;

         Enable(false, writes);

         if (notice == null)
            return;

         notice.Level = StatusLevel.Critical;
         notice.Text = (prefix ?? "") + (why ?? "");

         // The button is kept from one failure to the next. Try again runs the
         // page's read, the read fails again - the common case right after a lost
         // link - and comes back here, and a new button in the old one's place
         // would take the keyboard focus with it to the window: a screen reader
         // announces the window, and Tab starts again from the top of the page.
         if (tryAgain == null)
            notice.Content = null;
         else if (IsShown(notice) && notice.Content is Wpf.Ui.Controls.Button kept && kept.Tag is Retry retry)
            retry.Run = tryAgain;
         else
            notice.Content = TryAgainButton(tryAgain);

         notice.Tag = Marker;
         notice.Visibility = Visibility.Visible;
      }

      /// <summary>
      /// After a read that worked: the notice is hidden when it is showing a
      /// failure of this class's - a message the page wrote there itself is left
      /// alone - and every element in <paramref name="writes"/> is enabled again.
      /// A page calls it before it writes its own message to the notice, so that
      /// the Try again button does not stay beside a sentence it has nothing to
      /// do with.
      /// </summary>
      public static void Clear(InlineNotice notice, params UIElement[] writes)
      {
         Enable(true, writes);

         if (notice == null || !ReferenceEquals(notice.Tag, Marker))
            return;

         // Try again was pressed from the keyboard and the read worked: the focus
         // goes to the first control that writes - Add, or the card - or, where
         // none is given, to the next control in the tab order, before the button
         // that holds it leaves the tree and the focus falls to the window.
         if (notice.IsKeyboardFocusWithin)
            MoveFocusFrom(notice, writes);

         notice.Tag = null;
         notice.Content = null;
         notice.Text = null;
         notice.Visibility = Visibility.Collapsed;
      }

      private static void MoveFocusFrom(InlineNotice notice, UIElement[] writes)
      {
         foreach (UIElement write in writes ?? Array.Empty<UIElement>())
         {
            if (write == null || !write.IsEnabled || !write.IsVisible)
               continue;

            if (write.Focusable && write.Focus())
               return;

            // A card or a form: its first control in the tab order.
            if (write.MoveFocus(new TraversalRequest(FocusNavigationDirection.First)))
               return;
         }

         if (Keyboard.FocusedElement is UIElement focused && notice.IsAncestorOf(focused))
            focused.MoveFocus(new TraversalRequest(FocusNavigationDirection.Next));
      }

      // What the Try again button runs, held on the button so that a failure
      // after a failure gives it the new read without replacing the button.
      private sealed class Retry
      {
         public Action Run;
      }

      /// <summary>Whether the notice is showing a failure of this class's.</summary>
      public static bool IsShown(InlineNotice notice)
         => notice != null && ReferenceEquals(notice.Tag, Marker);

      /// <summary>
      /// The action a failure notice carries: Try again, which runs the read once
      /// more. One button for every page, so that it reads the same everywhere.
      ///
      /// It carries no Alt key, in any language. It is on some twenty pages, and
      /// no letter is free of a collision on every one of them - in English T is
      /// Require TLS for auth, Add action, Take the role away, Restore settings
      /// and Test connection - and WPF, given two enabled targets for one key,
      /// moves the focus between them and presses neither. It is reached by Tab,
      /// as the one action in a notice that is a live region.
      /// </summary>
      public static Wpf.Ui.Controls.Button TryAgainButton(Action tryAgain)
      {
         var button = new Wpf.Ui.Controls.Button
         {
            Content = L("Try again"),
            Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary,
            VerticalAlignment = VerticalAlignment.Center
         };
         System.Windows.Automation.AutomationProperties.SetAutomationId(button, "read-try-again");
         var retry = new Retry { Run = tryAgain };
         button.Tag = retry;
         button.Click += (_, _) => retry.Run?.Invoke();
         return button;
      }

      private static void Enable(bool enabled, UIElement[] writes)
      {
         if (writes == null)
            return;

         foreach (UIElement write in writes)
         {
            if (write != null)
               write.IsEnabled = enabled;
         }
      }
   }
}
