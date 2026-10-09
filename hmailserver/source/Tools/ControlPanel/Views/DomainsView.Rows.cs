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
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The rows' verbs from the keyboard and the right mouse button. Each of the
   /// page's four lists puts its verbs in buttons inside the row template, which
   /// the keyboard never reaches: a ListView keeps ListBox's TabNavigation of
   /// Once, so Tab enters the list on the selected row and the next Tab leaves
   /// it, and the arrow keys only move the selection - a keyboard user could
   /// select a domain or an account and then neither open, delete nor manage it.
   /// So each list answers Enter with the selected row's Properties or Edit,
   /// Delete with its delete and the confirmation that goes with it, Shift+F10,
   /// the Menu key and the right mouse button with a context menu of the same
   /// verbs, and a double-click on a row with its Properties or Edit. The
   /// buttons stay for the mouse, and both roads lead to the one method per verb.
   /// </summary>
   public partial class DomainsView
   {
      private void WireRows_()
      {
         WireList_(DomainList, L("_Properties…"), row => EditDomain_(((DomainRow)row).Name), null,
            L("Delete _domain"), row => DeleteDomain_(((DomainRow)row).Name));
         WireList_(AccountList, L("_Edit…"), row => EditAccount_(((AccountRow)row).Address), null,
            L("Delete _account"), row => DeleteAccount_(((AccountRow)row).Address));
         WireList_(AliasList, null, null, null,
            L("Delete _alias"), row => DeleteAlias_(((AliasRow)row).Name));
         WireList_(DistList, L("_Properties…"), row => EditDistList_(((DistRow)row).Address), row => EditRecipients_(((DistRow)row).Address),
            L("Delete _list"), row => DeleteDistList_(((DistRow)row).Address));
      }

      /// <summary>
      /// Gives one list its keys, its double-click and its context menu. A verb
      /// that is null is one the list's rows do not have: an alias is only ever
      /// deleted, and only a distribution list has members. The menu's delete
      /// names what goes - the row's buttons say "Delete" beside the row, and a
      /// menu is read on its own.
      /// </summary>
      private static void WireList_(ListView list, string openCaption, Action<object> open, Action<object> members, string deleteCaption, Action<object> delete)
      {
         var menu = new ContextMenu();
         void Add(string caption, Action<object> verb)
         {
            var item = new MenuItem { Header = caption };
            item.Click += (_, _) => WithSelected_(list, verb);
            menu.Items.Add(item);
         }

         if (open != null)
            Add(openCaption, open);
         if (members != null)
            Add(L("_Members…"), members);
         if (delete != null)
            Add(deleteCaption, delete);
         list.ContextMenu = menu;

         // No row, no menu. From the keyboard the menu is the selected row's; from
         // the mouse it is the row under the pointer's, which the right button
         // selects - not a blank part of the list's, and not a button's.
         list.ContextMenuOpening += (_, e) =>
         {
            bool fromMouse = e.CursorLeft >= 0 && e.CursorTop >= 0;
            if (list.SelectedItem == null || (fromMouse && RowAt_(list, e.OriginalSource as DependencyObject) == null))
            {
               e.Handled = true;
               return;
            }

            // Where the menu opens. From the mouse WPF puts it at the pointer,
            // over the row the right button selected. From the keyboard WPF
            // centres it on the menu's owner, which is the whole list - at 1280
            // by 800 a card some 460 pixels tall - so the menu floated far from
            // the row it acts on. The menu's own placement is coerced from the
            // owner's attached one once WPF has set the owner, after this event
            // (PopupControlService.CoerceProperty), so the row is named there,
            // and the menu hangs under it like a dropdown. Both are cleared for
            // the mouse, so that the pointer rules again and a row a reload took
            // with it is not held.
            if (fromMouse)
            {
               list.ClearValue(ContextMenuService.PlacementTargetProperty);
               list.ClearValue(ContextMenuService.PlacementProperty);
            }
            else
            {
               ContextMenuService.SetPlacementTarget(list, list.ItemContainerGenerator.ContainerFromItem(list.SelectedItem) as UIElement ?? list);
               ContextMenuService.SetPlacement(list, PlacementMode.Bottom);
            }
         };

         list.KeyDown += (_, e) =>
         {
            if (e.Handled || Keyboard.Modifiers != ModifierKeys.None)
               return;
            if (!(e.OriginalSource is ListViewItem || ReferenceEquals(e.OriginalSource, list)))
               return;

            if (e.Key == Key.Enter && open != null)
            {
               e.Handled = true;
               WithSelected_(list, open);
            }
            else if (e.Key == Key.Delete && delete != null)
            {
               e.Handled = true;
               WithSelected_(list, delete);
            }
         };

         list.MouseDoubleClick += (_, e) =>
         {
            if (open == null || e.ChangedButton != MouseButton.Left || RowAt_(list, e.OriginalSource as DependencyObject) == null)
               return;

            e.Handled = true;
            WithSelected_(list, open);
         };
      }

      /// <summary>
      /// Runs a verb on the selected row and puts the keyboard back on the row
      /// that is selected afterwards, when it was on one before: the reload that
      /// follows every verb rebuilds the rows, and the one the keyboard was on
      /// goes with them, which left a keyboard user nowhere after each Enter.
      /// </summary>
      private static void WithSelected_(ListView list, Action<object> verb)
      {
         object row = list.SelectedItem;
         if (row == null)
            return;

         bool hadKeyboard = list.IsKeyboardFocusWithin;
         verb(row);

         if (hadKeyboard)
            list.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() => FocusSelectedRow_(list)));
      }

      private static void FocusSelectedRow_(ListView list)
      {
         if (!list.IsVisible)
            return;

         if (list.SelectedItem != null && list.ItemContainerGenerator.ContainerFromItem(list.SelectedItem) is ListViewItem row)
            row.Focus();
         else
            list.Focus();
      }

      /// <summary>
      /// The row an event came from, or null when it came from a blank part of
      /// the list or from one of a row's own buttons, which has a click of its own.
      /// </summary>
      private static ListViewItem RowAt_(ListView list, DependencyObject source)
      {
         for (DependencyObject d = source; d != null && !ReferenceEquals(d, list); d = ParentOf_(d))
         {
            if (d is ButtonBase)
               return null;
            if (d is ListViewItem row)
               return row;
         }

         return null;
      }

      private static DependencyObject ParentOf_(DependencyObject d)
         => d is Visual || d is System.Windows.Media.Media3D.Visual3D ? VisualTreeHelper.GetParent(d) : LogicalTreeHelper.GetParent(d);

      /// <summary>
      /// Enter in each of the page's four forms presses the form's button - Add
      /// domain, Create account, Add alias, Add list - as a click does, and does
      /// nothing while the button is disabled; Escape empties what was typed
      /// (Views/FormSubmit). The keyboard user typed the last box and then tabbed
      /// to the button.
      /// </summary>
      private void WireForms_()
      {
         FormSubmit.Bind(AddDomainForm, AddDomainButton, NewDomainBox);
         FormSubmit.Bind(AccountForm, AddAccountButton, NewAccountBox, NewAccountPassword);
         FormSubmit.Bind(AddAliasForm, AddAliasButton, NewAliasName, NewAliasValue);
         FormSubmit.Bind(AddListForm, AddListButton, NewDistAddress);
      }
   }
}
