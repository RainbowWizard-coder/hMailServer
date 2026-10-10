// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// A page that can open at one of its settings. A setting found in the Ctrl+K
   /// palette opened its page on the first tab with nothing marked - "MaxMind
   /// country database" opened Auto-ban on its Auto-ban tab, three tabs from the
   /// setting (review finding F274). The shell asks the page it has just opened to
   /// show the setting; a page that cannot (a hand-written page) is opened as it
   /// always was.
   /// </summary>
   public interface IRevealSetting
   {
      /// <summary>
      /// Shows the setting whose hMailServer.ini key or COM path is
      /// <paramref name="key"/>: its tab chosen, its row scrolled into view, the
      /// keyboard in its editor and a ring round the row once. False when the page
      /// has no such setting, which leaves the page as it opened.
      /// <paramref name="label"/> is the setting's English label, the catalogue key the
      /// settings index has: where more than one row of the page shares the key - Auto-ban's
      /// four CountryPolicy rows, one for each kind of connection - it says which, where
      /// IMAP, POP3 and the web all opened at the SMTP row. Null or unknown, the key's
      /// first row is shown.
      /// </summary>
      bool RevealSetting(string key, string label);
   }

   /// <summary>
   /// The half of <see cref="IRevealSetting"/> every settings page shares: finding
   /// a setting's editor by the automation id the page gives it (its key or path),
   /// or by its row's caption, and showing it - scrolled into view, the keyboard in
   /// it, and the row ringed once in the focus colour, which fades unless Windows
   /// has animations turned off. The ring is an adorner, as the focus glow is
   /// (Services/FocusGlow): it is drawn over the page and moves nothing.
   /// </summary>
   internal static class SettingReveal
   {
      /// <summary>How long the ring stays before it fades, and how long the fade takes.</summary>
      private static readonly TimeSpan Hold = TimeSpan.FromMilliseconds(900);
      private static readonly TimeSpan Fade = TimeSpan.FromMilliseconds(1100);

      /// <summary>
      /// The editor under <paramref name="root"/> whose automation id is
      /// <paramref name="key"/>, or the editor of the row captioned
      /// <paramref name="caption"/>; null when neither is there.
      /// </summary>
      public static FrameworkElement Find(DependencyObject root, string key, string caption = null)
      {
         if (root == null)
            return null;

         FrameworkElement byCaption = null;
         var queue = new Queue<DependencyObject>();
         queue.Enqueue(root);
         while (queue.Count > 0)
         {
            DependencyObject node = queue.Dequeue();
            if (node is FrameworkElement element)
            {
               if (!string.IsNullOrEmpty(key) && string.Equals(AutomationProperties.GetAutomationId(element), key, StringComparison.OrdinalIgnoreCase))
                  return element;

               if (byCaption == null && caption != null && element is Scaffold.FieldRow row &&
                   string.Equals(Services.MnemonicText.Strip(row.Label ?? ""), Services.MnemonicText.Strip(caption), StringComparison.Ordinal))
                  byCaption = row.Content as FrameworkElement ?? row;
            }

            int children = VisualTreeHelper.GetChildrenCount(node);
            for (int at = 0; at < children; at++)
               queue.Enqueue(VisualTreeHelper.GetChild(node, at));
         }

         return byCaption;
      }

      /// <summary>
      /// Whether a row's label is <paramref name="label"/>, its Alt key aside: the row's
      /// label is the translated caption, and <paramref name="label"/> the index's
      /// English label translated. False for a null label.
      /// </summary>
      public static bool SameLabel(string rowLabel, string label)
         => label != null && rowLabel != null &&
            string.Equals(Services.MnemonicText.Strip(rowLabel), Services.MnemonicText.Strip(label), StringComparison.Ordinal);

      /// <summary>
      /// Shows <paramref name="editor"/>: its row scrolled into view, the keyboard in
      /// the editor (or the first control in it that takes the keyboard), and the
      /// row ringed once. Nothing happens for a null editor.
      /// </summary>
      public static void Show(FrameworkElement editor)
      {
         if (editor == null)
            return;

         FrameworkElement row = RowOf_(editor);
         ScrollTo_(row);

         IInputElement target = editor.Focusable && editor.IsEnabled ? editor : FirstFocusable_(editor);
         if (target != null)
            Keyboard.Focus(target);

         Ring_(row);
      }

      /// <summary>
      /// The row a third of the way down its scroll viewer's viewport, with room for the
      /// ring and the start of what follows it (Services/RevealOffset), where
      /// row.BringIntoView() scrolled as little as it could: a row below the fold ended at
      /// the viewport's edge, its ring's lower edge cut off and the explanation under it out
      /// of sight. The viewport is the scroll viewer's content presenter, inside its
      /// padding. A row in no scroll viewer is brought into view with that room.
      /// </summary>
      private static void ScrollTo_(FrameworkElement row)
      {
         row.UpdateLayout();

         // The nearest viewer that scrolls up and down: one that scrolls only sideways
         // round part of a row is not the page's.
         ScrollContentPresenter viewport = null;
         for (DependencyObject at = VisualTreeHelper.GetParent(row); at != null && viewport == null; at = VisualTreeHelper.GetParent(at))
         {
            if (at is ScrollContentPresenter presenter && presenter.ScrollOwner != null &&
                presenter.ScrollOwner.VerticalScrollBarVisibility != ScrollBarVisibility.Disabled)
               viewport = presenter;
         }

         ScrollViewer scroll = viewport?.ScrollOwner;
         if (scroll == null)
         {
            row.BringIntoView(new Rect(-Services.RevealOffset.Above, -Services.RevealOffset.Above,
               row.ActualWidth + 2 * Services.RevealOffset.Above, row.ActualHeight + Services.RevealOffset.Above + Services.RevealOffset.Below));
            return;
         }

         double top = row.TranslatePoint(new Point(0, 0), viewport).Y;
         double? offset = Services.RevealOffset.For(scroll.VerticalOffset, scroll.ViewportHeight, scroll.ScrollableHeight, top, row.ActualHeight);
         if (offset != null)
            scroll.ScrollToVerticalOffset(offset.Value);
      }

      private static FrameworkElement RowOf_(FrameworkElement editor)
      {
         for (DependencyObject node = editor; node != null; node = VisualTreeHelper.GetParent(node))
         {
            if (node is Scaffold.FieldRow row)
               return row;
         }

         return editor;
      }

      private static IInputElement FirstFocusable_(DependencyObject root)
      {
         var queue = new Queue<DependencyObject>();
         queue.Enqueue(root);
         while (queue.Count > 0)
         {
            DependencyObject node = queue.Dequeue();
            if (node is UIElement element && element.Focusable && element.IsEnabled && element.IsVisible)
               return element;

            int children = VisualTreeHelper.GetChildrenCount(node);
            for (int at = 0; at < children; at++)
               queue.Enqueue(VisualTreeHelper.GetChild(node, at));
         }

         return null;
      }

      private static void Ring_(FrameworkElement target)
      {
         AdornerLayer layer = AdornerLayer.GetAdornerLayer(target);
         if (layer == null)
            return;

         var ring = new RingAdorner(target);
         layer.Add(ring);

         if (!SystemParameters.ClientAreaAnimation)
         {
            var timer = new DispatcherTimer { Interval = Hold + Fade };
            timer.Tick += (_, _) =>
            {
               timer.Stop();
               layer.Remove(ring);
            };
            timer.Start();
            return;
         }

         var fade = new DoubleAnimation(1, 0, new Duration(Fade)) { BeginTime = Hold };
         fade.Completed += (_, _) => layer.Remove(ring);
         ring.BeginAnimation(UIElement.OpacityProperty, fade);
      }

      /// <summary>A ring in the focus colour round the row, outside its edge; the system highlight under High Contrast.</summary>
      private sealed class RingAdorner : Adorner
      {
         public RingAdorner(UIElement adorned) : base(adorned)
         {
            IsHitTestVisible = false;
         }

         protected override void OnRender(DrawingContext drawing)
         {
            Brush brush = SystemParameters.HighContrast
               ? SystemColors.HighlightBrush
               : (AdornedElement as FrameworkElement)?.TryFindResource("AppFocusBrush") as Brush ?? SystemColors.HighlightBrush;
            Size size = AdornedElement.RenderSize;
            var bounds = new Rect(-4, -4, size.Width + 8, size.Height + 8);
            drawing.DrawRoundedRectangle(null, new Pen(brush, 2), bounds, 6, 6);
         }
      }
   }
}
