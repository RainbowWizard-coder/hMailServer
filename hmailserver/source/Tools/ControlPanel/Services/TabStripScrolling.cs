// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;   // ButtonBase, for the chevron RepeatButtons
using System.Windows.Input;
using System.Windows.Media;   // Visual and VisualTreeHelper, to find the header a request comes from

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Makes an overflowing tab strip reachable with a mouse.
   ///
   /// THE DEFECT. The Control Panel replaces the stock WPF tab strip, which wraps
   /// its headers onto several rows, with a single row inside a ScrollViewer whose
   /// scrollbar is hidden. That reads far better - right up to the point where the
   /// tabs no longer fit. AccountDialog has eleven of them. Past the edge of the
   /// dialog they were still there, still focusable with the arrow keys, and
   /// completely unreachable with a mouse: no scrollbar, no buttons, and nothing on
   /// screen to suggest anything had been cut off. A dialog that hides four of its
   /// eleven tabs from anyone who navigates by pointer is not a styling problem.
   ///
   /// THE FIX has three parts, and all three are needed.
   ///
   /// The wheel scrolls the strip horizontally. A vertical wheel gesture is what
   /// people try first on a horizontal strip, and a ScrollViewer with no vertical
   /// range does nothing with it - so the gesture that feels most natural was also
   /// the one that most convincingly said "there is nothing more here".
   ///
   /// Two chevron buttons appear at the ends, but only while there is somewhere to
   /// go, and each disables itself at its own end of the range. Permanently visible
   /// buttons would put furniture on every dialog whose tabs fit, which is most of
   /// them; buttons that are always enabled would claim there is more to see when
   /// there is not.
   ///
   /// Selecting a tab by keyboard scrolls it into view. Arrow-key navigation
   /// already moved selection to tabs that were off screen, so the selected tab
   /// could be one nobody could see - which is a worse state than not being able to
   /// reach it, because the dialog is then showing a page whose tab is invisible.
   ///
   /// It is attached in the TabControl template in App.xaml, so every tabbed dialog
   /// gets it without opting in. Attaching it per dialog is how AccountDialog would
   /// have been fixed and DomainDialog would not.
   /// </summary>
   ///
   /// Since 10 October 2026 the strip moves by whole headers (Services/TabStripSnap,
   /// review findings F193 and F107): a chevron click puts the first header that
   /// was cut or out of sight at the right at the strip's left edge, or goes back
   /// by as many whole headers as fit; a wheel notch is one header; and a header
   /// selected by keyboard or click is brought in by a whole-header step. By pixels
   /// - four fifths of the strip a click, 48 a notch, and Windows' least distance
   /// for the selected header - the strip began and ended mid-word ("… ing and
   /// disclaimer", "Tagg"). An edge that still cuts a header - the right edge as a
   /// rule, the left only at the strip's end - fades over
   /// <see cref="TabStripSnap.FadeWidth"/> pixels, so the cut header reads as one
   /// that goes on under the chevron.
   public static class TabStripScrolling
   {
      /// <summary>
      /// Sub-pixel slack. Scroll offsets are doubles and a maxed-out ScrollViewer
      /// routinely lands a fraction short of ScrollableWidth, which would leave
      /// the right-hand chevron enabled at the end of the strip, clickable, and
      /// doing nothing.
      /// </summary>
      private const double Epsilon = 0.5;

      public static readonly DependencyProperty LeftButtonProperty =
         DependencyProperty.RegisterAttached(
            "LeftButton", typeof(ButtonBase), typeof(TabStripScrolling),
            new PropertyMetadata(null, OnButtonChanged));

      public static readonly DependencyProperty RightButtonProperty =
         DependencyProperty.RegisterAttached(
            "RightButton", typeof(ButtonBase), typeof(TabStripScrolling),
            new PropertyMetadata(null, OnButtonChanged));

      public static void SetLeftButton(DependencyObject element, ButtonBase value) =>
         element.SetValue(LeftButtonProperty, value);

      public static ButtonBase GetLeftButton(DependencyObject element) =>
         (ButtonBase)element.GetValue(LeftButtonProperty);

      public static void SetRightButton(DependencyObject element, ButtonBase value) =>
         element.SetValue(RightButtonProperty, value);

      public static ButtonBase GetRightButton(DependencyObject element) =>
         (ButtonBase)element.GetValue(RightButtonProperty);

      private static void OnButtonChanged(DependencyObject target, DependencyPropertyChangedEventArgs args)
      {
         if (target is not ScrollViewer viewer)
            return;

         if (args.OldValue is ButtonBase old)
            old.Click -= OnChevronClick;

         if (args.NewValue is ButtonBase added)
         {
            // The handler reads which end it is from the sender's identity, so it
            // needs to be able to get back to the viewer.
            added.Tag = viewer;
            added.Click -= OnChevronClick;
            added.Click += OnChevronClick;
         }

         Attach(viewer);
         Update(viewer);
      }

      private static void Attach(ScrollViewer viewer)
      {
         // Both attached properties are set in the same template, so this runs
         // twice per strip. Unsubscribing first keeps that idempotent rather than
         // scrolling two notches per wheel click.
         viewer.ScrollChanged -= OnScrollChanged;
         viewer.ScrollChanged += OnScrollChanged;

         viewer.PreviewMouseWheel -= OnMouseWheel;
         viewer.PreviewMouseWheel += OnMouseWheel;

         viewer.Loaded -= OnLoaded;
         viewer.Loaded += OnLoaded;

         // A header given the keyboard or clicked asks to be brought into view;
         // the viewer's own handler does it by the least distance, which leaves
         // the header beside it cut. Snapped to a whole header once that is done.
         viewer.RemoveHandler(FrameworkElement.RequestBringIntoViewEvent, (RequestBringIntoViewEventHandler)OnRequestBringIntoView);
         viewer.AddHandler(FrameworkElement.RequestBringIntoViewEvent, (RequestBringIntoViewEventHandler)OnRequestBringIntoView, true);
      }

      private static void OnRequestBringIntoView(object sender, RequestBringIntoViewEventArgs args)
      {
         if (sender is not ScrollViewer viewer || HeaderOf_(viewer, args.TargetObject) is not { } header)
            return;

         viewer.Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Loaded, new Action(() => Reveal_(viewer, header)));
      }

      /// <summary>The header of this strip that <paramref name="target"/> is or is inside, or null.</summary>
      private static TabItem HeaderOf_(ScrollViewer viewer, DependencyObject target)
      {
         if (viewer.Content is not Panel panel)
            return null;

         for (DependencyObject current = target; current != null && !ReferenceEquals(current, viewer); current = current is Visual ? VisualTreeHelper.GetParent(current) : LogicalTreeHelper.GetParent(current))
         {
            if (current is TabItem item && panel.Children.Contains(item))
               return item;
         }

         return null;
      }

      /// <summary>
      /// The headers that are drawn, as left edge and width in the strip's own
      /// coordinates, in the order they are laid out.
      /// </summary>
      private static List<(double Left, double Width)> Headers_(ScrollViewer viewer, out List<TabItem> items)
      {
         var headers = new List<(double Left, double Width)>();
         items = new List<TabItem>();
         if (viewer.Content is not Panel panel)
            return headers;

         foreach (object child in panel.Children)
         {
            if (child is not TabItem item || item.Visibility != Visibility.Visible || item.ActualWidth <= 0)
               continue;

            try
            {
               headers.Add((item.TranslatePoint(new Point(0, 0), panel).X, item.ActualWidth));
               items.Add(item);
            }
            catch (InvalidOperationException)
            {
               // Not under the panel any more (removed between layout and now).
            }
         }

         return headers;
      }

      private static void Reveal_(ScrollViewer viewer, TabItem header)
      {
         List<(double Left, double Width)> headers = Headers_(viewer, out List<TabItem> items);
         int index = items.IndexOf(header);
         if (index < 0)
            return;

         double offset = TabStripSnap.Reveal(headers, index, viewer.HorizontalOffset, viewer.ViewportWidth, viewer.ScrollableWidth);
         if (Math.Abs(offset - viewer.HorizontalOffset) > Epsilon)
            viewer.ScrollToHorizontalOffset(offset);
      }

      private static void OnLoaded(object sender, RoutedEventArgs args)
      {
         if (sender is not ScrollViewer viewer)
            return;

         Update(viewer);
         BringSelectedIntoView(viewer);
      }

      private static void OnScrollChanged(object sender, ScrollChangedEventArgs args)
      {
         if (sender is ScrollViewer viewer)
         {
            Update(viewer);

            // ExtentWidthChange fires when tabs are added or removed, which is
            // also when a selection that was comfortably in view can stop being.
            if (Math.Abs(args.ExtentWidthChange) > Epsilon)
               BringSelectedIntoView(viewer);
         }
      }

      private static void OnMouseWheel(object sender, MouseWheelEventArgs args)
      {
         if (sender is not ScrollViewer viewer || viewer.ScrollableWidth <= Epsilon)
            return;

         // One header a notch: a wheel turned towards the reader (a negative
         // delta) moves the strip on, as a vertical list scrolls down.
         viewer.ScrollToHorizontalOffset(TabStripSnap.Notch(Headers_(viewer, out _), viewer.HorizontalOffset,
            viewer.ScrollableWidth, -Math.Sign(args.Delta)));

         // Handled, or the gesture keeps travelling up to whatever scrollable
         // panel the dialog sits in and scrolls the page behind the tabs instead.
         args.Handled = true;
      }

      private static void OnChevronClick(object sender, RoutedEventArgs args)
      {
         if (sender is not ButtonBase button || button.Tag is not ScrollViewer viewer)
            return;

         bool left = ReferenceEquals(GetLeftButton(viewer), button);
         List<(double Left, double Width)> headers = Headers_(viewer, out _);

         viewer.ScrollToHorizontalOffset(left
            ? TabStripSnap.Back(headers, viewer.HorizontalOffset, viewer.ViewportWidth, viewer.ScrollableWidth)
            : TabStripSnap.Forward(headers, viewer.HorizontalOffset, viewer.ViewportWidth, viewer.ScrollableWidth));
      }

      private static void Update(ScrollViewer viewer)
      {
         bool overflowing = viewer.ScrollableWidth > Epsilon;
         List<(double Left, double Width)> headers = overflowing ? Headers_(viewer, out _) : null;

         ButtonBase left = GetLeftButton(viewer);
         ButtonBase right = GetRightButton(viewer);

         // Each chevron is live while a header is out of sight or cut on its
         // side: with the strip on the last header's whole-header step, the few
         // pixels left to scroll are the strip's own end, not another tab.
         if (left != null)
         {
            left.Visibility = overflowing ? Visibility.Visible : Visibility.Collapsed;
            left.IsEnabled = overflowing && viewer.HorizontalOffset > Epsilon
                             && (headers.Count == 0 || TabStripSnap.MoreBefore(headers, viewer.HorizontalOffset));
         }

         if (right != null)
         {
            right.Visibility = overflowing ? Visibility.Visible : Visibility.Collapsed;
            right.IsEnabled = overflowing && viewer.HorizontalOffset < viewer.ScrollableWidth - Epsilon
                              && (headers.Count == 0 || TabStripSnap.MoreAfter(headers, viewer.HorizontalOffset, viewer.ViewportWidth));
         }

         Fade_(viewer, headers);
      }

      /// <summary>
      /// Fades each edge of the strip that cuts a header, over
      /// <see cref="TabStripSnap.FadeWidth"/> pixels, as an opacity mask on the
      /// viewer: a colour laid over the edge would have to be the colour of what
      /// is behind the strip, which is a dialog's Mica, a page or a card.
      /// </summary>
      private static void Fade_(ScrollViewer viewer, List<(double Left, double Width)> headers)
      {
         bool atLeft = headers != null && TabStripSnap.CutAtLeft(headers, viewer.HorizontalOffset);
         bool atRight = headers != null && TabStripSnap.CutAtRight(headers, viewer.HorizontalOffset, viewer.ViewportWidth);
         double width = viewer.ActualWidth;

         if ((!atLeft && !atRight) || width <= 2 * TabStripSnap.FadeWidth)
         {
            if (viewer.OpacityMask != null)
               viewer.OpacityMask = null;
            return;
         }

         double fade = TabStripSnap.FadeWidth / width;
         var mask = new System.Windows.Media.LinearGradientBrush { StartPoint = new Point(0, 0.5), EndPoint = new Point(1, 0.5) };
         mask.GradientStops.Add(new System.Windows.Media.GradientStop(atLeft ? System.Windows.Media.Colors.Transparent : System.Windows.Media.Colors.Black, 0));
         mask.GradientStops.Add(new System.Windows.Media.GradientStop(System.Windows.Media.Colors.Black, atLeft ? fade : 0));
         mask.GradientStops.Add(new System.Windows.Media.GradientStop(System.Windows.Media.Colors.Black, atRight ? 1 - fade : 1));
         mask.GradientStops.Add(new System.Windows.Media.GradientStop(atRight ? System.Windows.Media.Colors.Transparent : System.Windows.Media.Colors.Black, 1));
         mask.Freeze();
         viewer.OpacityMask = mask;
      }

      /// <summary>
      /// Scrolls the selected tab into view, so keyboard navigation past the edge
      /// does not leave the dialog showing a page whose tab is off screen.
      /// </summary>
      private static void BringSelectedIntoView(ScrollViewer viewer)
      {
         if (viewer.Content is not Panel panel)
            return;

         foreach (object child in panel.Children)
         {
            if (child is TabItem { IsSelected: true } selected)
            {
               // Deferred: during a Loaded or an extent change the item may not
               // have been arranged yet, and a header of no size is in no place
               // to scroll to. Brought in by a whole-header step.
               selected.Dispatcher.BeginInvoke(
                  System.Windows.Threading.DispatcherPriority.Loaded,
                  new Action(() =>
                  {
                     try
                     {
                        Reveal_(viewer, selected);
                     }
                     catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
                     {
                        // A tab removed between the post and the callback. Losing
                        // the scroll position is not worth taking the dialog down.
                     }
                  }));
               return;
            }
         }
      }
   }
}
