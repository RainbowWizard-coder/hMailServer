// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Media3D;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The rows of a plain list - a ListBox or a ListView, not a data grid, which
   /// copies its own - can be copied: Ctrl+C, and Copy on the list's right-click
   /// menu, put the selected rows on the clipboard as they are shown, one row per
   /// line in the list's order, the pieces of text in a row (a list view's columns,
   /// a domain and its "inactive") separated by a tab as a data grid's cells are.
   /// Select all is on the menu where a list takes more than one row at a time.
   /// Issue #48 made every piece of text selectable but left the words in a list's
   /// rows to the list, because a press there chooses the row; this copies the rows.
   ///
   /// Registered once for every ListBox (and so every ListView) in the application,
   /// as SelectableText is for text. A list that copies something of its own - one
   /// with its own Copy command or its own context menu, as the live log has - is
   /// left to it, and so is a list inside something with a menu of its own. The
   /// words of the buttons in a row (Properties, Delete) and icons are not copied.
   /// A container turns it off with <see cref="SelectableText.IsEnabledProperty"/>.
   /// </summary>
   public static class SelectableList
   {
      private static bool installed_;

      /// <summary>
      /// Registers the handlers for every list in the application. Called once, from
      /// App.OnStartup, before the first window; safe to call again.
      /// </summary>
      public static void Install()
      {
         if (installed_)
            return;
         installed_ = true;

         CommandManager.RegisterClassCommandBinding(typeof(ListBox),
            new CommandBinding(ApplicationCommands.Copy, OnCopy, OnCanCopy));
         EventManager.RegisterClassHandler(typeof(ListBox), FrameworkElement.ContextMenuOpeningEvent,
            new ContextMenuEventHandler(OnContextMenuOpening));
      }

      /// <summary>
      /// Whether this list's rows are copied here: not switched off, and no Copy or
      /// menu of its own or of anything it is in.
      /// </summary>
      public static bool IsCopyable(ListBox list)
      {
         if (list == null || !SelectableText.GetIsEnabled(list) || list.ContextMenu != null)
            return false;

         if (list.CommandBindings.Cast<CommandBinding>().Any(binding => binding.Command == ApplicationCommands.Copy))
            return false;

         for (DependencyObject node = ParentOf_(list); node != null; node = ParentOf_(node))
         {
            if (node is FrameworkElement element && element.ContextMenu != null)
               return false;
         }

         return true;
      }

      /// <summary>
      /// The selected rows' text, as <see cref="RowText"/> reads each, one row per line
      /// (CR LF) in the order the list shows them, filtered and sorted as it is.
      /// </summary>
      public static string SelectedRowsText(ListBox list)
      {
         if (list == null || list.SelectedItems.Count == 0)
            return "";

         // By reference: two rows with the same words are two rows.
         var chosen = new HashSet<object>(list.SelectedItems.Cast<object>(), SameItem.Instance);
         var lines = new List<string>();
         var reader = new OffScreenReader_();
         foreach (object item in list.Items)
         {
            if (chosen.Contains(item))
               lines.Add(RowText_(list, item, reader));
         }

         return string.Join("\r\n", lines);
      }

      /// <summary>
      /// A row's text as it is shown: every visible piece of text in the row, in the
      /// row's own order, tab-separated, leaving out the words of buttons, text boxes
      /// and icons in it. A row scrolled out of view (a list does not keep what it is
      /// not showing) is built again from the list's own template to be read.
      /// </summary>
      public static string RowText(ListBox list, object item) => RowText_(list, item, new OffScreenReader_());

      private static string RowText_(ListBox list, object item, OffScreenReader_ reader)
      {
         var container = (item as ListBoxItem) ?? list.ItemContainerGenerator.ContainerFromItem(item) as FrameworkElement;
         if (container != null)
         {
            string shown = string.Join("\t", TextsIn_(container, container));
            if (shown.Length > 0 || item is UIElement)
               return shown;
         }

         // An element that is its own row is read where it is: put into a presenter of
         // ours it would take that as its parent and refuse the list's own when the
         // row came back into view.
         if (item is Visual || item is Visual3D)
            return string.Join("\t", TextsIn_((DependencyObject)item, (DependencyObject)item));

         // Not on screen: the row as the list would build it. A list view's columns
         // one by one - a row presenter would join the columns the list shows and
         // could change their widths - and otherwise the item's template.
         if (list is ListView view && view.View is GridView grid)
         {
            return string.Join("\t", grid.Columns.Select(column => column.DisplayMemberBinding != null
               ? reader.Bound(column, item, column.DisplayMemberBinding)
               : reader.Built(column, item, column.CellTemplate, column.CellTemplateSelector)));
         }

         if (list.ItemTemplate != null || list.ItemTemplateSelector != null)
            return reader.Built(list, item, list.ItemTemplate, list.ItemTemplateSelector);

         if (!string.IsNullOrEmpty(list.DisplayMemberPath))
            return reader.Bound(list, item, new Binding(list.DisplayMemberPath));

         return item?.ToString() ?? "";
      }

      /// <summary>
      /// Reads the rows out of view, each template built once and handed one row after
      /// another, as a list recycles its rows. Built afresh for every row, the domains
      /// page's account template took 1.2 to 2.5 milliseconds a row: Select all and
      /// Ctrl+C on 5,000 accounts held the Control Panel for 8 seconds, on 20,000 for 24.
      /// Kept only while one copy is read, so no row outlives it.
      /// </summary>
      private sealed class OffScreenReader_
      {
         private readonly Dictionary<object, TextBlock> probes_ = new();
         private readonly Dictionary<object, ContentPresenter> presenters_ = new();

         /// <summary>What a binding shows for the item, read through a text block of its own.</summary>
         public string Bound(object key, object item, BindingBase binding)
         {
            if (!probes_.TryGetValue(key, out TextBlock probe))
            {
               probe = new TextBlock { DataContext = item };
               BindingOperations.SetBinding(probe, TextBlock.TextProperty, binding);
               probes_.Add(key, probe);
            }

            probe.DataContext = item;
            return probe.Text ?? "";
         }

         /// <summary>The item built from a template, off the screen, and read.</summary>
         public string Built(object key, object item, DataTemplate template, DataTemplateSelector selector)
         {
            if (!presenters_.TryGetValue(key, out ContentPresenter presenter))
            {
               presenter = new ContentPresenter { ContentTemplate = template, ContentTemplateSelector = selector };
               presenters_.Add(key, presenter);
            }

            presenter.Content = item;
            presenter.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            return string.Join("\t", TextsIn_(presenter, presenter));
         }
      }

      private static IEnumerable<string> TextsIn_(DependencyObject node, DependencyObject row)
      {
         if (node is UIElement element && element.Visibility != Visibility.Visible)
            yield break;
         // The words of a button, a text box and the like in the row are theirs, and
         // an icon's glyph is not text.
         if (node != row && (IsInteractive_(node) || node is Wpf.Ui.Controls.IconElement))
            yield break;

         if (node is TextBlock block)
         {
            string text = SelectableText.TextToCopy(block);
            if (text.Length > 0)
               yield return text;
            yield break;
         }

         int count = node is Visual || node is Visual3D ? VisualTreeHelper.GetChildrenCount(node) : 0;
         for (int i = 0; i < count; i++)
         {
            foreach (string text in TextsIn_(VisualTreeHelper.GetChild(node, i), row))
               yield return text;
         }
      }

      private static bool IsInteractive_(DependencyObject node)
      {
         return node is ButtonBase || node is TextBoxBase || node is PasswordBox || node is ComboBox
             || node is Thumb || node is ScrollBar || node is Slider;
      }

      // ---- the handlers ------------------------------------------------------------

      private static void OnCanCopy(object sender, CanExecuteRoutedEventArgs e)
      {
         if (sender is not ListBox list)
            return;

         if (!IsCopyable(list))
         {
            // Its own Copy answers, if it has one; if nothing does, the key goes on,
            // as it did before lists could copy.
            e.ContinueRouting = true;
            return;
         }

         if (list.SelectedItems.Count > 0)
         {
            e.CanExecute = true;
         }
         else
         {
            // Nothing chosen: the key goes on, as it did before a list could copy.
            e.CanExecute = false;
            e.ContinueRouting = true;
         }

         e.Handled = true;
      }

      private static void OnCopy(object sender, ExecutedRoutedEventArgs e)
      {
         if (sender is not ListBox list || !IsCopyable(list))
            return;

         SelectableText.SetClipboard(SelectedRowsText(list));
         e.Handled = true;
      }

      private static void OnContextMenuOpening(object sender, ContextMenuEventArgs e)
      {
         if (e.Handled || sender is not ListBox list || !IsCopyable(list))
            return;

         // A press on a button in a row, a scroll bar or anything with a menu of its
         // own stays theirs; a list inside a list is the inner one's. The row's icon
         // and words are the row's.
         for (DependencyObject node = e.OriginalSource as DependencyObject; node != null && node != list; node = ParentOf_(node))
         {
            if (IsInteractive_(node) || node is ListBox)
               return;
            if (node is FrameworkElement element && element.ContextMenu != null)
               return;
         }

         bool several = list.SelectionMode != SelectionMode.Single;
         if (list.SelectedItems.Count == 0 && !(several && list.Items.Count > 0))
            return;   // nothing to copy or select: no menu with nothing on it

         ContextMenu menu = SelectableText.CopyMenu(
            () => SelectableText.SetClipboard(SelectedRowsText(list)),
            () => list.SelectedItems.Count > 0,
            several ? list.SelectAll : null);

         // From the keyboard (the menu key, Shift+F10) there is no pointer to open at:
         // the menu opens under the row that has the keyboard, or the list.
         if (e.CursorLeft < 0)
         {
            menu.PlacementTarget = Keyboard.FocusedElement is ListBoxItem row && ItemsControl.ItemsControlFromItemContainer(row) == list ? row : list;
            menu.Placement = PlacementMode.Bottom;
         }
         else
         {
            menu.PlacementTarget = list;
            menu.Placement = PlacementMode.MousePoint;
         }

         menu.IsOpen = true;
         e.Handled = true;
      }

      /// <summary>
      /// The same row: the same object, or for a value (a number, an enum) the same
      /// value - a list over values hands out a new box for it each time it is read.
      /// </summary>
      private sealed class SameItem : IEqualityComparer<object>
      {
         public static readonly SameItem Instance = new();

         public new bool Equals(object x, object y)
            => x != null && x.GetType().IsValueType ? x.Equals(y) : ReferenceEquals(x, y);

         public int GetHashCode(object obj)
            => obj == null ? 0 : obj.GetType().IsValueType ? obj.GetHashCode() : RuntimeHelpers.GetHashCode(obj);
      }

      private static DependencyObject ParentOf_(DependencyObject node)
      {
         DependencyObject parent = node is Visual || node is Visual3D ? VisualTreeHelper.GetParent(node) : null;
         return parent ?? LogicalTreeHelper.GetParent(node);
      }
   }
}
