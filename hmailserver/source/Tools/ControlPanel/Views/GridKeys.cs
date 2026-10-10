// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
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
      /// F2 on a selected row renames it through <paramref name="rename"/>, for a
      /// list whose Enter and double-click open something other than the row's
      /// Edit - the Groups page's rows open their members, and F2 is Windows' key
      /// for a rename (review finding F201). Wired before <see cref="Wire(Selector, Func{bool}, Func{bool}, Func{bool})"/>,
      /// whose handler then sees the key handled; a rename that answers false
      /// leaves F2 to it, which opens the row as Enter does.
      /// </summary>
      public static void WireRename(Selector list, Func<bool> rename)
      {
         if (list == null || rename == null)
            return;

         list.PreviewKeyDown += (_, e) =>
         {
            if (e.Handled || e.Key != Key.F2 || Keyboard.Modifiers != ModifierKeys.None || list.SelectedItem == null
                || !FromTheRows_(list, e.OriginalSource as DependencyObject))
               return;

            bool hadKeyboard = list.IsKeyboardFocusWithin;
            if (rename())
            {
               e.Handled = true;
               if (hadKeyboard)
                  list.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() => FocusSelectedRow(list)));
            }
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
      /// Makes <paramref name="grid"/>'s rows one Tab stop, after the controls its
      /// column headers hold. No cell, and nothing that does not mark itself a tab
      /// stop inside a row, is one: the arrow keys move between the rows and the
      /// page's own keys act on the selected one. Tab into the rows - from the
      /// headers or, with Shift, from after the grid - puts the keyboard on the
      /// selected row, selecting the first where none is; Tab from a row leaves
      /// the grid, and Shift+Tab from it goes back to the headers.
      ///
      /// The rows' host, the grid's ItemsPresenter, is the stop: it takes the
      /// keyboard only while the grid has rows, and passes it on at once. Setting
      /// the grid's TabNavigation to Once instead, as the quarantine did, made the
      /// header's first control the grid's one stop, so that Tab went through the
      /// header and left the grid without reaching a row, and the column headers
      /// and each row kept their own stops inside it (the review's skeptic, on
      /// F211).
      /// </summary>
      public static void RowsAreOneTabStop(DataGrid grid)
      {
         if (grid == null)
            return;

         KeyboardNavigation.SetTabNavigation(grid, KeyboardNavigationMode.Continue);

         var cells = new Style(typeof(DataGridCell));
         if ((grid.CellStyle ?? Application.Current?.TryFindResource(typeof(DataGridCell))) is Style basis)
            cells.BasedOn = basis;
         cells.Setters.Add(new Setter(Control.IsTabStopProperty, false));
         grid.CellStyle = cells;

         void MakeHostAStop()
         {
            if (RowsHost_(grid) is not ItemsPresenter host)
               return;
            host.FocusVisualStyle = null;
            KeyboardNavigation.SetIsTabStop(host, true);
            BindingOperations.SetBinding(host, UIElement.FocusableProperty, new Binding(nameof(ItemsControl.HasItems)) { Source = grid });
         }

         grid.Loaded += (_, _) => MakeHostAStop();
         if (grid.IsLoaded)
            MakeHostAStop();

         grid.AddHandler(UIElement.GotKeyboardFocusEvent, new KeyboardFocusChangedEventHandler((_, e) =>
         {
            if (e.OriginalSource is not ItemsPresenter host || !ReferenceEquals(host.TemplatedParent, grid))
               return;

            // Shift+Tab from a row reaches the host, which is before the rows:
            // the keyboard goes on to what is before the grid's rows.
            if (e.OldFocus is DependencyObject old && IsWithin_(old, host))
            {
               host.MoveFocus(new TraversalRequest(FocusNavigationDirection.Previous));
               return;
            }

            if (grid.SelectedItem == null && grid.Items.Count > 0)
               grid.SelectedIndex = 0;
            FocusSelectedRow(grid);
         }));
      }

      /// <summary>The grid's own ItemsPresenter, which holds its rows - not a column header's or a row's.</summary>
      private static ItemsPresenter RowsHost_(DataGrid grid)
      {
         grid.ApplyTemplate();
         var pending = new Queue<DependencyObject>();
         pending.Enqueue(grid);
         while (pending.Count > 0)
         {
            DependencyObject d = pending.Dequeue();
            if (d is ItemsPresenter presenter && ReferenceEquals(presenter.TemplatedParent, grid))
               return presenter;
            if (d is DataGridRow || d is DataGridColumnHeadersPresenter)
               continue;
            for (int i = 0; i < VisualTreeHelper.GetChildrenCount(d); i++)
               pending.Enqueue(VisualTreeHelper.GetChild(d, i));
         }

         return null;
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
