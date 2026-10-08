// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Threading;
using hMailServer.ControlPanel.Services;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel
{
   /// <summary>
   /// The group flyout: the list of a group's pages that a rail glyph opens
   /// beside itself, and the breadcrumb's group button under itself, while the
   /// sidebar is the rail. The arrow keys move through it, Enter or a click
   /// opens a page, Escape or a click outside closes it, and the keyboard goes
   /// back to what opened it.
   ///
   /// Below <see cref="DesignTokens.Shell.CollapseSidebarBelow"/> the sidebar is
   /// the rail whatever the administrator chose, and the tree cannot be shown: a
   /// glyph's click used to clear the collapse preference, apply the sidebar -
   /// which found the window too narrow and kept the rail - and expand the group
   /// inside the collapsed tree, so nothing happened on screen, while the glyph
   /// stayed checked and the rail marked a group that did not hold the page on
   /// screen. The breadcrumb's group button expanded the same hidden tree. In a
   /// maximised window narrower than the threshold - 1280 by 800 at 150 % is 853
   /// pixels - the toggle could not widen the window either, and the only ways
   /// to a page were Ctrl+K and the Related links. Above the threshold a glyph
   /// still expands the sidebar on its group, which is a visible result.
   /// </summary>
   public partial class MainWindow
   {
      private Popup groupFlyout_;
      private ListBox groupFlyoutList_;
      private TextBlock groupFlyoutTitle_;

      /// <summary>
      /// True while the window is too narrow for the tree, so that expanding the
      /// sidebar on a group is not possible and the group's pages open in the
      /// flyout instead.
      /// </summary>
      private bool TooNarrowForTree_ => ActualWidth < DesignTokens.Shell.CollapseSidebarBelow;

      /// <summary>
      /// Opens the flyout for <paramref name="group"/> beside <paramref name="anchor"/>,
      /// the page on screen marked where it is one of the group's, and the keyboard
      /// on that row or the first.
      /// </summary>
      private void ShowGroupFlyout_(NavNode group, UIElement anchor, PlacementMode placement)
      {
         BuildGroupFlyout_();

         string title = L(group.Title);
         groupFlyoutTitle_.Text = title;
         AutomationProperties.SetName(groupFlyoutList_, title);
         AutomationProperties.SetAutomationId(groupFlyoutList_, "navflyout-" + NavigationMap.Slug(group.Title));

         groupFlyoutList_.Items.Clear();
         foreach (NavNode page in VisiblePagesOf_(group))
         {
            // The sidebar's page row by another control (App.xaml, NavFlyoutItem):
            // the hover fill, the brand pill on the page on screen, the focus
            // colour round the row the keyboard is on.
            var row = new ListBoxItem
            {
               Content = new TextBlock { Text = L(page.Title), TextTrimming = TextTrimming.CharacterEllipsis },
               Tag = page.Key,
               ToolTip = string.IsNullOrEmpty(page.Purpose) ? L(page.Title) : L(page.Title) + " - " + L(page.Purpose),
               IsSelected = string.Equals(page.Key, currentPage_, StringComparison.OrdinalIgnoreCase)
            };
            row.SetResourceReference(StyleProperty, "NavFlyoutItem");
            AutomationProperties.SetName(row, L(page.Title));
            AutomationProperties.SetAutomationId(row, "navflyout-" + page.Key);
            if (!string.IsNullOrEmpty(page.Purpose))
               AutomationProperties.SetHelpText(row, L(page.Purpose));

            string key = page.Key;
            row.PreviewMouseLeftButtonUp += (s, e) =>
            {
               e.Handled = true;
               OpenFromFlyout_(key);
            };
            groupFlyoutList_.Items.Add(row);
         }

         if (groupFlyoutList_.Items.Count == 0)
            return;

         groupFlyout_.PlacementTarget = anchor;
         groupFlyout_.Placement = placement;
         groupFlyout_.HorizontalOffset = placement == PlacementMode.Right ? DesignTokens.Space.Xs : 0;
         groupFlyout_.VerticalOffset = placement == PlacementMode.Bottom ? DesignTokens.Space.Xs : 0;
         groupFlyout_.IsOpen = true;

         // After the popup has laid its rows out: a row cannot take the keyboard
         // before it is in a window.
         Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
         {
            if (!groupFlyout_.IsOpen)
               return;

            ListBoxItem first = groupFlyoutList_.SelectedItem as ListBoxItem ?? groupFlyoutList_.Items.OfType<ListBoxItem>().FirstOrDefault();
            first?.Focus();
         }));
      }

      /// <summary>
      /// The group's pages as the signed-in credential may see them - the same
      /// rule as <see cref="VisibleRoots_"/>, since the breadcrumb hands over the
      /// map's own node, whose children are not filtered.
      /// </summary>
      private static IEnumerable<NavNode> VisiblePagesOf_(NavNode group)
      {
         ServerSession session = ServerSession.Current;
         Func<string, bool> mayUse = session == null || session.Permissions == null ? null : session.May;
         return group.Children.Where(child => child.IsPage && (mayUse == null || mayUse(child.Needs)));
      }

      private void OpenFromFlyout_(string key)
      {
         groupFlyout_.IsOpen = false;
         NavigateTo(key);
      }

      private void BuildGroupFlyout_()
      {
         if (groupFlyout_ != null)
            return;

         groupFlyoutTitle_ = new TextBlock
         {
            FontSize = Typography.Body,
            FontWeight = FontWeights.SemiBold,
            TextTrimming = TextTrimming.CharacterEllipsis,
            Margin = new Thickness(DesignTokens.Space.Sm, DesignTokens.Space.Xs, DesignTokens.Space.Sm, DesignTokens.Space.Sm)
         };
         groupFlyoutTitle_.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorPrimaryBrush");

         groupFlyoutList_ = new ListBox
         {
            BorderThickness = new Thickness(0),
            Background = System.Windows.Media.Brushes.Transparent,
            MaxHeight = 12 * DesignTokens.Shell.NavRowHeight
         };
         ScrollViewer.SetHorizontalScrollBarVisibility(groupFlyoutList_, ScrollBarVisibility.Disabled);
         groupFlyoutList_.PreviewKeyDown += (s, e) =>
         {
            if (e.Key == Key.Enter && Keyboard.Modifiers == ModifierKeys.None)
            {
               // The row the keyboard is on, and only failing that the selected
               // one. Focus is not selection here: the pill means the page on
               // screen, so the flyout puts the keyboard on its first row without
               // selecting it, and a ListBox selects a row that took the focus
               // only when the keyboard moved there from inside the list, and
               // answers Enter only with KeyboardNavigation.AcceptsReturn - so an
               // Enter on the first row of every group but the one holding the
               // page on screen used to do nothing.
               ListBoxItem row = Keyboard.FocusedElement as ListBoxItem;
               if (row == null || !ReferenceEquals(ItemsControl.ItemsControlFromItemContainer(row), groupFlyoutList_))
                  row = groupFlyoutList_.SelectedItem as ListBoxItem;

               if (row?.Tag is string key)
               {
                  e.Handled = true;
                  OpenFromFlyout_(key);
               }
            }
            else if (e.Key == Key.Escape)
            {
               e.Handled = true;
               groupFlyout_.IsOpen = false;
            }
         };

         var body = new StackPanel();
         body.Children.Add(groupFlyoutTitle_);
         body.Children.Add(groupFlyoutList_);

         var frame = new Border
         {
            Padding = new Thickness(DesignTokens.Space.Sm),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(DesignTokens.Radius.Card),
            MinWidth = 220,
            MaxWidth = DesignTokens.Shell.SidebarMaxWidth,
            Child = body
         };
         frame.SetResourceReference(Border.BackgroundProperty, "ApplicationBackgroundBrush");
         frame.SetResourceReference(Border.BorderBrushProperty, "ControlStrokeColorDefaultBrush");
         AutomationProperties.SetAutomationId(frame, "nav-flyout");

         groupFlyout_ = new Popup
         {
            StaysOpen = false,
            AllowsTransparency = true,
            Child = frame
         };

         // The keyboard goes back to the glyph or the button that opened the
         // flyout, whichever way it closed: Escape, a choice, or a click outside.
         groupFlyout_.Closed += (s, e) => FocusAfterFlyout_();
      }

      /// <summary>
      /// Puts the keyboard back on what opened the flyout. A Popup raises Closed
      /// from a timer after IsOpen went false (Popup.HideWindow), so a page chosen
      /// from the breadcrumb's flyout is on screen by then and the group button
      /// that anchored the flyout has gone with the breadcrumb NavigateTo rebuilt:
      /// focusing it failed, nothing else took the keyboard, and the next Tab
      /// started from the window's first stop. The rebuilt breadcrumb carries the
      /// group's button again, found by its automation id; failing that, the rail
      /// glyph that is marked is the group of the page on screen. A rail glyph
      /// outlives the navigation and takes the keyboard itself.
      /// </summary>
      private void FocusAfterFlyout_()
      {
         var anchor = groupFlyout_.PlacementTarget as UIElement;
         if (anchor != null && anchor.IsVisible && anchor.Focus())
            return;

         string id = anchor == null ? null : AutomationProperties.GetAutomationId(anchor);
         UIElement rebuilt = string.IsNullOrEmpty(id) || BreadcrumbHost == null
            ? null
            : BreadcrumbHost.Children.OfType<UIElement>().FirstOrDefault(child => AutomationProperties.GetAutomationId(child) == id);
         if (rebuilt != null && rebuilt.IsVisible && rebuilt.Focus())
            return;

         railItems_.FirstOrDefault(item => item.IsChecked == true)?.Focus();
      }
   }
}
