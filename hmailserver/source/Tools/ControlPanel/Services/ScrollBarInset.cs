// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Windows;
using System.Windows.Controls;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Keeps a dialog's scrolled content clear of the scroll bar while the bar is
   /// there (review findings F192 and F073).
   ///
   /// WPF-UI draws a scroll viewer's bar over its content, not beside it, so a
   /// dialog tab whose fields ran to its right edge lost the right border of every
   /// box, the end of every hint line and half of the DKIM tab's Browse… under the
   /// bar wherever the tab scrolled. A page pads its scroller by
   /// <see cref="DesignTokens.Space.Md"/> on the right for good
   /// (<c>FillPanel.ScrollingPage</c>); a dialog's tab scrolls only now and then, and
   /// a gutter kept for a bar that is not drawn would end its fields twelve pixels
   /// short of the tab strip and the Save button above and below them. So the inset
   /// here follows the bar: twelve pixels while the content is taller than the
   /// viewer, none while it fits.
   ///
   /// It cannot make the bar come and go for ever: the bar shows only when the
   /// content is taller than the viewer at its full width, and the inset can only
   /// make the content narrower and so taller, which keeps the bar; and it goes
   /// only when the content fits with the inset, where at the full width it fits
   /// the better.
   ///
   /// <code>svc:ScrollBarInset.IsEnabled="True"</code> on a ScrollViewer, or
   /// <see cref="SetIsEnabled"/> in code. The viewer's own right padding is
   /// replaced; its other three sides are kept.
   /// </summary>
   public static class ScrollBarInset
   {
      public static readonly DependencyProperty IsEnabledProperty = DependencyProperty.RegisterAttached(
         "IsEnabled", typeof(bool), typeof(ScrollBarInset), new PropertyMetadata(false, OnIsEnabledChanged));

      public static void SetIsEnabled(DependencyObject element, bool value) => element.SetValue(IsEnabledProperty, value);

      public static bool GetIsEnabled(DependencyObject element) => (bool)element.GetValue(IsEnabledProperty);

      /// <summary>The right inset a viewer takes with its bar showing, or none.</summary>
      public static double InsetFor(bool barShowing) => barShowing ? DesignTokens.Space.Md : 0;

      private static void OnIsEnabledChanged(DependencyObject target, DependencyPropertyChangedEventArgs args)
      {
         if (target is not ScrollViewer viewer)
            return;

         viewer.ScrollChanged -= OnScrollChanged;
         viewer.Loaded -= OnLoaded;

         if ((bool)args.NewValue)
         {
            viewer.ScrollChanged += OnScrollChanged;
            viewer.Loaded += OnLoaded;
            Apply_(viewer);
         }
      }

      private static void OnLoaded(object sender, RoutedEventArgs args)
      {
         if (sender is ScrollViewer viewer)
            Apply_(viewer);
      }

      // Raised after layout as the extent or the viewport changes, which is when
      // the bar comes or goes.
      private static void OnScrollChanged(object sender, ScrollChangedEventArgs args)
      {
         if (sender is ScrollViewer viewer && ReferenceEquals(args.OriginalSource, viewer))
            Apply_(viewer);
      }

      /// <summary>
      /// The same for a list's own viewer, the ScrollViewer in its template: a list
      /// whose rows end in buttons drew its bar over them where it scrolled - the
      /// Domains page's rows' delete buttons (review finding F101) - and, kept
      /// clear for good, the rows of a list that fits would end twelve pixels
      /// short of the form above them. Applied once the list has its template.
      ///
      /// A list in a part that is not shown when it loads has no template then, and
      /// so no viewer to find: the sidebar's tree is in a pane collapsed until sign-in
      /// (the owner's decision 9), and the inset given it at load found nothing, so
      /// its rows still ran under the bar and the keyboard's ring on a row was cut on
      /// the right. The template is made here, and the list is tried again each time
      /// it is shown.
      /// </summary>
      public static void ForList(Control list)
      {
         if (list == null)
            return;

         void Enable(object sender, RoutedEventArgs args)
         {
            list.ApplyTemplate();
            if (ViewerOf_(list) is ScrollViewer viewer && !GetIsEnabled(viewer))
               SetIsEnabled(viewer, true);
         }

         list.Loaded += Enable;
         list.IsVisibleChanged += (sender, args) =>
         {
            if (list.IsVisible)
               Enable(list, null);
         };
         if (list.IsLoaded)
            Enable(list, null);
      }

      /// <summary>The first ScrollViewer in a control's template, or null before it has one.</summary>
      private static ScrollViewer ViewerOf_(DependencyObject root)
      {
         var pending = new System.Collections.Generic.Queue<DependencyObject>();
         pending.Enqueue(root);
         while (pending.Count > 0)
         {
            DependencyObject node = pending.Dequeue();
            if (node is ScrollViewer viewer && !ReferenceEquals(node, root))
               return viewer;
            for (int i = 0; i < System.Windows.Media.VisualTreeHelper.GetChildrenCount(node); i++)
               pending.Enqueue(System.Windows.Media.VisualTreeHelper.GetChild(node, i));
         }
         return null;
      }

      private static void Apply_(ScrollViewer viewer)
      {
         bool showing = viewer.ComputedVerticalScrollBarVisibility == Visibility.Visible;
         double right = InsetFor(showing);
         Thickness padding = viewer.Padding;
         if (padding.Right != right)
            viewer.Padding = new Thickness(padding.Left, padding.Top, right, padding.Bottom);
      }
   }
}
