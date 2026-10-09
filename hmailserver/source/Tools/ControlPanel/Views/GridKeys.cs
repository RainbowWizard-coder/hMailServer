// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The keys of a list page's list: Enter and F2 open the selected row - its
   /// Edit, Properties or the like - Delete deletes it, with the confirmation the
   /// page's Delete asks, and Insert adds a row; a double-click on a row opens it.
   ///
   /// Until October 2026 no list answered Enter, Delete or F2 and a double-click
   /// was the only way to open a row (review finding F211), so a keyboard user
   /// selected a row and then tabbed to the page header's buttons to act on it.
   /// The double-click was the grid's own: on a collection page a double-click on
   /// a column header's divider - to fit the column - opened Edit for the selected
   /// row. Here it counts only on a row, and never on a button inside one.
   ///
   /// Each key presses the page's own button for the verb, so a key does what a
   /// click does - the same dialog, the same question before a delete - and a
   /// button that is disabled (no row selected, a read that failed) is not
   /// pressed by its key either. The keyboard goes back to the selected row
   /// afterwards, since the reload that follows a verb rebuilds the rows. The
   /// domains page's lists keep their own wiring (DomainsView.Rows.cs), which
   /// adds a context menu; the quarantine's grid has keys of its own.
   /// </summary>
   public static class GridKeys
   {
      /// <summary>Wires <paramref name="list"/> for its verbs; a null button is a verb the page does not have.</summary>
      public static void Wire(Selector list, ButtonBase open, ButtonBase delete, ButtonBase add)
         => Wire(list, Pressing(open), Pressing(delete), Pressing(add));

      /// <summary>
      /// Wires <paramref name="list"/> for verbs that are not buttons. Each says
      /// whether it acted; a null one is a verb the page does not have.
      /// </summary>
      public static void Wire(Selector list, Func<bool> open, Func<bool> delete, Func<bool> add)
      {
         if (list == null)
            return;

         list.PreviewKeyDown += (_, e) =>
         {
            if (e.Handled || Keyboard.Modifiers != ModifierKeys.None || !FromTheRows_(list, e.OriginalSource as DependencyObject))
               return;

            Func<bool> verb = e.Key switch
            {
               Key.Enter or Key.F2 => list.SelectedItem != null ? open : null,
               Key.Delete => list.SelectedItem != null ? delete : null,
               Key.Insert => add,
               _ => null
            };

            if (verb == null)
               return;

            bool hadKeyboard = list.IsKeyboardFocusWithin;
            if (verb())
            {
               e.Handled = true;
               if (hadKeyboard)
                  list.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() => FocusSelectedRow(list)));
            }
         };

         list.MouseDoubleClick += (_, e) =>
         {
            if (open == null || e.ChangedButton != MouseButton.Left || RowAt(list, e.OriginalSource as DependencyObject) == null)
               return;

            if (open())
               e.Handled = true;
         };
      }

      /// <summary>
      /// Presses <paramref name="button"/> as a click would, now, and says whether
      /// it could be pressed: not when it is disabled or not on screen. Its Click
      /// handlers run before this returns - a dialog the button opens has closed
      /// and the page has read its list again - so that what follows sees the
      /// result, which a press through the automation peer, queued for later,
      /// would not let it.
      /// </summary>
      public static bool Press(ButtonBase button)
      {
         if (button == null || !button.IsEnabled || !button.IsVisible)
            return false;

         if (button.Command != null)
         {
            if (!button.Command.CanExecute(button.CommandParameter))
               return false;
            button.Command.Execute(button.CommandParameter);
            return true;
         }

         button.RaiseEvent(new RoutedEventArgs(ButtonBase.ClickEvent, button));
         return true;
      }

      /// <summary>A verb that presses <paramref name="button"/>; null for no button.</summary>
      public static Func<bool> Pressing(ButtonBase button) => button == null ? null : () => Press(button);

      /// <summary>
      /// Insert's verb on a page whose add form is on the page: the keyboard goes
      /// to the form's first box, brought into view, rather than to a dialog.
      /// </summary>
      public static Func<bool> Focusing(UIElement field) => field == null ? null : () =>
      {
         if (!field.IsVisible || !field.IsEnabled)
            return false;
         (field as FrameworkElement)?.BringIntoView();
         return Keyboard.Focus(field) != null;
      };

      /// <summary>
      /// Puts the keyboard on the selected row, or on the list when nothing is
      /// selected - the reload after a verb took the row it was on.
      /// </summary>
      public static void FocusSelectedRow(Selector list)
      {
         if (!list.IsVisible)
            return;

         object item = list.SelectedItem;
         if (item != null)
         {
            if (list is DataGrid grid)
            {
               grid.ScrollIntoView(item);
               grid.UpdateLayout();
               if (grid.ItemContainerGenerator.ContainerFromItem(item) is DataGridRow row)
               {
                  DataGridColumn first = grid.CurrentColumn ?? (grid.Columns.Count > 0 ? grid.Columns[0] : null);
                  if (first?.GetCellContent(row)?.Parent is DataGridCell cell && cell.Focus())
                     return;
                  if (row.Focus())
                     return;
               }
            }
            else if (list.ItemContainerGenerator.ContainerFromItem(item) is UIElement container && container.Focus())
            {
               return;
            }
         }

         list.Focus();
      }

      /// <summary>
      /// Whether a key came from the list's rows - a row, a cell, the list itself
      /// - rather than from a control inside a row, which has keys of its own: a
      /// button that Enter presses, a box Delete edits.
      /// </summary>
      private static bool FromTheRows_(Selector list, DependencyObject source)
      {
         for (DependencyObject d = source; d != null && !ReferenceEquals(d, list); d = ParentOf_(d))
         {
            if (d is ButtonBase || d is TextBoxBase || d is PasswordBox || d is ComboBox)
               return false;
            if (d is DataGridRow || d is ListBoxItem || d is DataGridCell)
               return true;
         }

         return source != null && (ReferenceEquals(source, list) || IsWithin_(source, list));
      }

      /// <summary>The row an event came from, or null when it came from a column header, a blank part of the list or a button inside a row.</summary>
      public static FrameworkElement RowAt(Selector list, DependencyObject source)
      {
         for (DependencyObject d = source; d != null && !ReferenceEquals(d, list); d = ParentOf_(d))
         {
            if (d is ButtonBase || d is DataGridColumnHeader)
               return null;
            if (d is DataGridRow || d is ListBoxItem)
               return (FrameworkElement)d;
         }

         return null;
      }

      private static bool IsWithin_(DependencyObject element, DependencyObject root)
      {
         for (DependencyObject d = element; d != null; d = ParentOf_(d))
         {
            if (ReferenceEquals(d, root))
               return true;
         }

         return false;
      }

      private static DependencyObject ParentOf_(DependencyObject d)
         => d is Visual || d is System.Windows.Media.Media3D.Visual3D ? VisualTreeHelper.GetParent(d) : LogicalTreeHelper.GetParent(d);
   }
}
