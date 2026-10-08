// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The navigation rail leads somewhere in a narrow window. Below
   /// DesignTokens.Shell.CollapseSidebarBelow the sidebar is the rail whatever
   /// the administrator chose, and a glyph's click used to clear the collapse
   /// preference, apply the sidebar - which found the window too narrow and kept
   /// the rail - and expand the group inside the tree that was not shown: nothing
   /// happened on screen, the glyph stayed checked, and the rail marked a group
   /// that did not hold the page on screen. The breadcrumb's group button
   /// expanded the same hidden tree, and in a maximised window narrower than the
   /// threshold the toggle could not widen the window either. The WPF-free test
   /// project cannot click a glyph, so these tests hold the shell's source to the
   /// shape that fixes it: a glyph opens the group's pages in a flyout where the
   /// tree cannot be shown, the breadcrumb's group button does the same while the
   /// rail shows, the flyout is a list of rows named by key that the keyboard
   /// moves through and leaves, and a click never leaves the rail marking the
   /// wrong group.
   /// </summary>
   public class NavigationFlyoutTests
   {
      [Fact]
      public void ARailGlyph_OpensTheGroupsPages_WhereTheTreeCannotBeShown()
      {
         string shell = ReadControlPanelFile("MainWindow.xaml.cs");
         string rail = WithheldRowTests.MemberText(shell, "private void BuildNavRail_()");
         Assert.True(rail != null, "MainWindow.BuildNavRail_ is gone.");

         int narrow = rail.IndexOf("if (TooNarrowForTree_)", StringComparison.Ordinal);
         int flyout = rail.IndexOf("ShowGroupFlyout_(group, item", StringComparison.Ordinal);
         int reveal = rail.IndexOf("RevealGroup(title)", StringComparison.Ordinal);
         Assert.True(narrow >= 0 && flyout > narrow, "A rail glyph's click no longer opens the group's pages where the tree cannot be shown.");
         Assert.True(reveal > flyout, "Above the threshold a rail glyph no longer expands the sidebar on its group.");

         // The click checked the glyph; the mark goes back to the group of the
         // page on screen, whichever way the click went.
         int mark = rail.IndexOf("UpdateRail_(currentPage_)", StringComparison.Ordinal);
         Assert.True(mark > reveal, "A rail glyph's click leaves the glyph checked, so the rail marks a group that does not hold the page on screen.");
      }

      [Fact]
      public void TheBreadcrumbsGroupButton_OpensTheFlyout_WhileTheRailShows()
      {
         string shell = ReadControlPanelFile("MainWindow.xaml.cs");
         string segment = WithheldRowTests.MemberText(shell, "private FrameworkElement GroupSegment(NavNode group)");
         Assert.True(segment != null, "MainWindow.GroupSegment is gone.");

         int rail = segment.IndexOf("if (railShowing_)", StringComparison.Ordinal);
         int flyout = segment.IndexOf("ShowGroupFlyout_(group, button", StringComparison.Ordinal);
         int reveal = segment.IndexOf("RevealGroup(group.Title)", StringComparison.Ordinal);
         Assert.True(rail >= 0 && flyout > rail && reveal > flyout,
            "The breadcrumb's group button expands the hidden tree while the rail shows, which does nothing on screen.");
      }

      [Fact]
      public void TheFlyout_IsAListOfTheGroupsPages_TheKeyboardMovesThroughAndLeaves()
      {
         string flyout = ReadControlPanelFile("MainWindow.NavFlyout.cs");

         // The threshold is the sidebar's own, so the two cannot disagree.
         Assert.Contains("ActualWidth < DesignTokens.Shell.CollapseSidebarBelow", flyout, StringComparison.Ordinal);

         // One row per page the credential may see, named and identified by its key,
         // so that automation and a screen reader find a page row on screen.
         Assert.Contains("\"navflyout-\" + page.Key", flyout, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetName(row, L(page.Title))", flyout, StringComparison.Ordinal);
         Assert.Contains("VisiblePagesOf_(group)", flyout, StringComparison.Ordinal);
         Assert.Contains("mayUse(child.Needs)", flyout, StringComparison.Ordinal);

         // Enter opens, Escape closes, a click outside closes, and the keyboard
         // goes back to what opened it.
         Assert.Contains("e.Key == Key.Enter", flyout, StringComparison.Ordinal);
         Assert.Contains("e.Key == Key.Escape", flyout, StringComparison.Ordinal);
         Assert.Contains("StaysOpen = false", flyout, StringComparison.Ordinal);
         Assert.Contains("groupFlyout_.Closed += (s, e) => FocusAfterFlyout_()", flyout, StringComparison.Ordinal);
         Assert.Contains("PreviewMouseLeftButtonUp", flyout, StringComparison.Ordinal);

         // Enter opens the row the keyboard is on, and only failing that the
         // selected one: the flyout focuses its first row without selecting it,
         // a ListBox selects a focused row only when the keyboard came from
         // inside the list, and it ignores Enter without AcceptsReturn - so an
         // Enter that read the selection alone did nothing in every group but
         // the one holding the page on screen.
         int enter = flyout.IndexOf("e.Key == Key.Enter", StringComparison.Ordinal);
         int focused = flyout.IndexOf("Keyboard.FocusedElement as ListBoxItem", enter, StringComparison.Ordinal);
         int selected = flyout.IndexOf("groupFlyoutList_.SelectedItem as ListBoxItem", enter, StringComparison.Ordinal);
         Assert.True(focused > enter && selected > focused, "Enter in the flyout no longer opens the row the keyboard is on before the selected one.");

         // Popup.Closed is raised from a timer, after a page chosen from the
         // breadcrumb's flyout has rebuilt the breadcrumb and detached the group
         // button that anchored it: the keyboard goes to the anchor while it is
         // on screen, else to the breadcrumb's rebuilt button of the same id,
         // else to the rail glyph that is marked.
         string back = WithheldRowTests.MemberText(flyout, "private void FocusAfterFlyout_()");
         Assert.True(back != null, "MainWindow.FocusAfterFlyout_ is gone.");
         int anchor = back.IndexOf("anchor.IsVisible && anchor.Focus()", StringComparison.Ordinal);
         int rebuilt = back.IndexOf("BreadcrumbHost.Children.OfType<UIElement>().FirstOrDefault(child => AutomationProperties.GetAutomationId(child) == id)", StringComparison.Ordinal);
         int glyph = back.IndexOf("railItems_.FirstOrDefault(item => item.IsChecked == true)?.Focus()", StringComparison.Ordinal);
         Assert.True(anchor >= 0 && rebuilt > anchor && glyph > rebuilt, "After the flyout closes the keyboard no longer goes to the anchor, then the rebuilt breadcrumb button, then the marked rail glyph.");

         // Opened, a row takes the keyboard: the page on screen's, or the first.
         Assert.Contains("first?.Focus()", flyout, StringComparison.Ordinal);

         // A row is the sidebar's page row by another control: the same fill,
         // pill and focus colour, from App.xaml, not the theme's accent-filled
         // list item.
         Assert.Contains("row.SetResourceReference(StyleProperty, \"NavFlyoutItem\")", flyout, StringComparison.Ordinal);
         string app = ReadControlPanelFile("App.xaml");
         int style = app.IndexOf("<Style x:Key=\"NavFlyoutItem\" TargetType=\"ListBoxItem\">", StringComparison.Ordinal);
         Assert.True(style >= 0, "App.xaml has no NavFlyoutItem style.");
         string body = app.Substring(style, app.IndexOf("</Style>", style, StringComparison.Ordinal) - style);
         foreach (string token in new[] { "AppNavRowHeight", "AppControlCornerRadius", "AppBrandBrush", "AppFocusBrush", "SubtleFillColorSecondaryBrush" })
            Assert.Contains(token, body, StringComparison.Ordinal);
      }

      // ---- helpers -------------------------------------------------------------------------

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
