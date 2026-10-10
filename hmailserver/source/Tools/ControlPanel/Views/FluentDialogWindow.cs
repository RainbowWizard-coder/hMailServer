// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The base class for every Control Panel dialog.
   ///
   /// Every dialog used to derive from plain System.Windows.Window, which
   /// meant a stock Win32 title bar following the OS app mode rather than the
   /// in-app theme - so on a light-mode OS, every Add/Edit dialog opened with
   /// a white title bar bolted onto dark content. Deriving from WPF-UI's
   /// FluentWindow gives the dialogs the same themed chrome as the shell.
   ///
   /// This class exists so the chrome, the application face and the theme
   /// background are decided once rather than re-decided (or forgotten) per
   /// dialog - the same reasoning as the Dialogs message-box class. It also
   /// carries the standard frame (<see cref="UseFrame"/>): heading, a body that
   /// scrolls, a footer with the primary button first, the window sized to its
   /// content within the token bounds instead of to a number typed into each
   /// dialog, Enter and Escape wired, and the keyboard put in the first field.
   /// The eighteen dialogs that predate it keep their own layout until they are
   /// migrated; nothing here runs unless a dialog asks for it.
   /// </summary>
   public class FluentDialogWindow : Wpf.Ui.Controls.FluentWindow
   {
      public FluentDialogWindow()
      {
         FontFamily = new FontFamily(Typography.UiFontFamily);
         SetResourceReference(BackgroundProperty, "ApplicationBackgroundBrush");
         // The default text colour for everything in the dialog, delivered by
         // property inheritance rather than by an implicit TextBlock style.
         // App.xaml used to carry such a style as a "global contrast
         // guarantee", but an app-level implicit style also reaches the
         // TextBlocks that ContentPresenter generates inside control
         // templates, and a style setter outranks an inherited value - so it
         // overwrote WPF-UI's on-accent button text (near-black on accent
         // blue in the light theme), tab dim states and the sidebar's
         // hover/selected foreground swap. Inheritance gives bare TextBlocks
         // the same default while letting templates and Appearance setters
         // override it, which is the whole point.
         SetResourceReference(ForegroundProperty, "TextFillColorPrimaryBrush");
         ShowInTaskbar = false;
      }

      /// <summary>
      /// Puts the standard frame in the window and returns it: the heading, the
      /// body, the primary button (which takes Enter) and the secondary button
      /// (which takes Escape; without one, Escape closes the dialog with no
      /// result). The window is sized to the content at <paramref name="width"/>
      /// - clamped to the dialog tokens - and its height is capped at a fraction
      /// of the work area, past which the body scrolls. On load the keyboard
      /// goes to the first field of the body, not to the title bar's buttons.
      /// </summary>
      protected DialogFrame UseFrame(string heading, UIElement body, Button primary, Button secondary,
                                    string description = null, double width = DesignTokens.Dialog.DefaultWidth)
      {
         var frame = new DialogFrame
         {
            Heading = heading,
            Description = description,
            Body = body,
            PrimaryButton = primary,
            SecondaryButton = secondary
         };

         if (primary != null)
            primary.IsDefault = true;

         if (secondary != null)
            secondary.IsCancel = true;
         else
            PreviewKeyDown += CloseOnEscape_;

         Content = frame;
         SizeToContentWithin(width);
         FocusFirstFieldOnLoad(body);
         return frame;
      }

      /// <summary>
      /// Sizes the window to its content: a fixed width within the dialog
      /// tokens' bounds, the height from the content up to
      /// <see cref="DesignTokens.Dialog.MaxHeightFraction"/> of the work area.
      /// SizeToContent.Height with a fixed width, never WidthAndHeight: auto-
      /// sizing both under FluentWindow's chrome is the combination known to
      /// clip the bottom of the content on first show (see Dialogs.Show).
      /// </summary>
      public void SizeToContentWithin(double width)
      {
         // As short as its content: WPF-UI's window style holds every window to 320
         // pixels at least, which drew a message box of one line 320 tall, its notice
         // stretched down it (review finding F041).
         MinHeight = 0;

         // A dialog's width is set for its text at 100 per cent: at a larger Windows
         // text size it widens with its text (review finding F317), up to the token's
         // widest and never past the work area, where a 520-pixel editor would have
         // wrapped every caption twice as often and cut what does not wrap.
         double widest = Math.Min(DesignTokens.Dialog.MaxWidth * TextScaleFactor.Factor, Math.Max(DesignTokens.Dialog.MaxWidth, SystemParameters.WorkArea.Width - 48));
         MinWidth = DesignTokens.Dialog.MinWidth;
         MaxWidth = widest;
         Width = Math.Max(DesignTokens.Dialog.MinWidth, Math.Min(widest, width * TextScaleFactor.Factor));
         SizeToContent = SizeToContent.Height;
         MaxHeight = Math.Max(240, SystemParameters.WorkArea.Height * DesignTokens.Dialog.MaxHeightFraction);
         ResizeMode = ResizeMode.NoResize;
         WindowStartupLocation = Owner != null ? WindowStartupLocation.CenterOwner : WindowStartupLocation.CenterScreen;
      }

      /// <summary>
      /// Keeps room under the body for <paramref name="lines"/> lines of a field's
      /// message, so that a message appearing under a field moves nothing but the
      /// fields below it: once the window has sized itself to its content, the
      /// body is held to that height and the room more, and its fields grow into
      /// the room as messages appear - past it, the window grows as before, or
      /// the body scrolls at the cap. A window sized to its content grew by every
      /// message, 20 pixels a line, and moved its footer, Add and Cancel with it,
      /// from under the pointer that had pressed Add (review finding F213). For
      /// the dialogs whose fields can be refused on the field: one line for each
      /// field a press can refuse at once, or for each line the longest message
      /// wraps to. The body's floor, not the window's height: a FluentWindow sized
      /// to its content counts a title bar its chrome does not draw, and a height
      /// set from it left that much more blank.
      ///
      /// The room follows the body: as the body's own content changes - a row
      /// shown or collapsed, a notice put in - the floor is taken again from the
      /// body's height without its messages, so that the dialog grows and shrinks
      /// with its content as it did and only a message is kept from moving the
      /// footer. A floor measured once, when the dialog loaded, held the
      /// access-control entry as tall as with its account box after Anyone
      /// collapsed the box, about 110 pixels of blank above Save. With
      /// <paramref name="refusable"/> named, the room is kept only while one of
      /// those rows is shown: a dialog whose only field that can be refused is
      /// collapsed has no message to keep room for.
      /// </summary>
      public void KeepRoomForFieldErrors(int lines = 1, params FieldRow[] refusable)
      {
         if (lines <= 0)
            return;

         Loaded += (_, _) =>
         {
            if (SizeToContent != SizeToContent.Height || (Content as DialogFrame)?.Body is not FrameworkElement body
                || VisualTreeHelper.GetParent(body) is not FrameworkElement holder || ScrollerAround_(holder) is not ScrollViewer scroller)
               return;

            // The floor is the presenter's that holds the body, inside the body's
            // scroller, so that the body's own height stays its content's and a
            // change of it can be seen. The window's height outside the scroller -
            // the title bar, the heading, the footer - is what a change of the
            // body leaves as it is.
            var room = new FieldErrorRoom_(this, body, holder, lines * FieldRow.ErrorLineHeight(this),
               ActualHeight - scroller.ActualHeight, refusable ?? Array.Empty<FieldRow>());
            double grows = room.Take();

            // Centred as it was placed: the room it grows by is shared above and below.
            if (grows > 0 && WindowStartupLocation != WindowStartupLocation.Manual)
               Top -= grows / 2;

            // Taken again after every layout in which the body's height changed.
            // The layout manager holds what LayoutUpdated is given, so it is let
            // go with the dialog.
            EventHandler follow = (_, _) => room.Take();
            body.LayoutUpdated += follow;
            Closed += (_, _) => body.LayoutUpdated -= follow;
         };
      }

      private static ScrollViewer ScrollerAround_(DependencyObject element)
      {
         for (DependencyObject at = VisualTreeHelper.GetParent(element); at != null; at = VisualTreeHelper.GetParent(at))
         {
            if (at is ScrollViewer scroller)
               return scroller;
         }

         return null;
      }

      /// <summary>
      /// The room one dialog keeps for its fields' messages (KeepRoomForFieldErrors):
      /// the floor of the presenter that holds the body, at the body's height
      /// without the messages shown in it and the room more, but no higher than
      /// the window can still grow (Services/FieldMessageRoom).
      /// </summary>
      private sealed class FieldErrorRoom_
      {
         private readonly FluentDialogWindow window_;
         private readonly FrameworkElement body_;
         private readonly FrameworkElement holder_;
         private readonly double room_;
         private readonly double chrome_;
         private readonly FieldRow[] refusable_;
         private double measured_ = double.NaN;

         public FieldErrorRoom_(FluentDialogWindow window, FrameworkElement body, FrameworkElement holder, double room, double chrome, FieldRow[] refusable)
         {
            window_ = window;
            body_ = body;
            holder_ = holder;
            room_ = room;
            chrome_ = chrome;
            refusable_ = refusable;
         }

         /// <summary>
         /// Sets the floor for the body as it is laid out now, where its height
         /// changed since the last time; what the room adds to the body's height.
         /// </summary>
         public double Take()
         {
            double height = body_.DesiredSize.Height;
            if (height.Equals(measured_))
               return 0;
            measured_ = height;

            var spans = new List<(double Top, double Bottom)>();
            bool refusableShown = refusable_.Length == 0;
            Gather_(body_, spans, ref refusableShown);

            (double floor, double grows) = FieldMessageRoom.Take(height, FieldMessageRoom.Covered(spans), room_, window_.MaxHeight, chrome_, refusableShown);
            if (Math.Abs(holder_.MinHeight - floor) >= 0.5)
               holder_.MinHeight = floor;
            return grows;
         }

         // The messages shown in the body, where they are, and whether one of the
         // rows the room is for is shown; nothing under a collapsed element is.
         private void Gather_(DependencyObject at, List<(double Top, double Bottom)> spans, ref bool refusableShown)
         {
            if (at is UIElement element && element.Visibility == Visibility.Collapsed)
               return;

            if (at is FieldRow row)
            {
               if (!refusableShown && Array.IndexOf(refusable_, row) >= 0)
                  refusableShown = true;

               if (row.ShownMessage is FrameworkElement line && line.IsArrangeValid)
               {
                  Rect bounds = line.TransformToAncestor(body_).TransformBounds(new Rect(0, 0, line.ActualWidth, line.ActualHeight));
                  spans.Add((bounds.Top - line.Margin.Top, bounds.Bottom + line.Margin.Bottom));
               }
            }

            int count = VisualTreeHelper.GetChildrenCount(at);
            for (int i = 0; i < count; i++)
               Gather_(VisualTreeHelper.GetChild(at, i), spans, ref refusableShown);
         }
      }

      /// <summary>
      /// On load, puts the keyboard in the first focusable field inside
      /// <paramref name="root"/>. Without this a FluentWindow opens with the
      /// focus on nothing, and the first Tab lands on the title bar's buttons.
      /// </summary>
      public void FocusFirstFieldOnLoad(UIElement root)
      {
         Loaded += (s, e) =>
         {
            if (Keyboard.FocusedElement is UIElement focused && IsInside_(focused, root))
               return;

            FrameworkElement first = FirstField_(root);
            if (first != null)
               Keyboard.Focus(first);
         };
      }

      private static bool IsInside_(DependencyObject element, DependencyObject root)
      {
         for (DependencyObject current = element; current != null; current = VisualTreeHelper.GetParent(current))
         {
            if (ReferenceEquals(current, root))
               return true;
         }

         return false;
      }

      /// <summary>Depth-first, in visual order, which is tab order for everything here.</summary>
      private static FrameworkElement FirstField_(DependencyObject root)
      {
         if (root is FrameworkElement element && element.Focusable && element.IsVisible && element.IsEnabled
             && KeyboardNavigation.GetIsTabStop(element) && root is not ScrollViewer)
            return element;

         int count = VisualTreeHelper.GetChildrenCount(root);
         for (int i = 0; i < count; i++)
         {
            FrameworkElement found = FirstField_(VisualTreeHelper.GetChild(root, i));
            if (found != null)
               return found;
         }

         return null;
      }

      private void CloseOnEscape_(object sender, KeyEventArgs e)
      {
         if (e.Key != Key.Escape)
            return;

         e.Handled = true;
         Close();
      }
   }
}
