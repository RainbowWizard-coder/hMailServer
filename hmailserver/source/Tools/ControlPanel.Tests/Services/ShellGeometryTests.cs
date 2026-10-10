// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The shell's geometry (T3.3 work item 7): the sidebar keeps its width when the
   /// help pane opens, and is wide enough at the opening size for every page label
   /// (review findings F021, F022); every top-level row of the tree and the rail is
   /// a glyph, Sign-in &amp; access no longer a second shield beside Spam &amp; virus
   /// filtering (F023); and the breadcrumb is one line however narrow the window
   /// (F047). The WPF-free test project cannot open the pane or draw the bar, so the
   /// arithmetic is held here and the window's wiring by its source.
   /// </summary>
   public class ShellGeometryTests
   {
      /// <summary>At the opening size the tree is 310 pixels, where it was 280 and cut "Web services &amp; autoconfiguration".</summary>
      [Fact]
      public void TheSidebar_IsAShareOfTheWindow_InsideItsClamp()
      {
         Assert.Equal(310, ShellGeometry.SidebarWidth(1280));
         Assert.Equal(DesignTokens.Shell.SidebarMinWidth, ShellGeometry.SidebarWidth(760));
         Assert.Equal(DesignTokens.Shell.SidebarMaxWidth, ShellGeometry.SidebarWidth(2560));
         Assert.Equal(DesignTokens.Shell.SidebarMinWidth, ShellGeometry.SidebarWidth(0));
         Assert.Equal(DesignTokens.Shell.SidebarMinWidth, ShellGeometry.SidebarWidth(double.NaN));

         // Never narrower as the window grows.
         double previous = 0;
         for (double width = 600; width <= 3000; width += 10)
         {
            double sidebar = ShellGeometry.SidebarWidth(width);
            Assert.True(sidebar >= previous, "The sidebar narrows as the window widens at " + width + ".");
            previous = sidebar;
         }
      }

      /// <summary>
      /// The help pane takes its width from the page alone: the tree's width is a
      /// function of the window's, and where the pane would leave the page less than
      /// 512 pixels the sidebar is the rail while it is open.
      /// </summary>
      [Fact]
      public void TheHelpPane_LeavesTheSidebarAlone_OrMakesItTheRailWhereThePageWouldBeTooNarrow()
      {
         // 1280 - 310 - 380 = 590: the tree stays, as wide as without the pane.
         Assert.False(ShellGeometry.RailBesideHelp(1280, 380));
         // 1100 - 266 - 380 = 454: the rail while the pane is open.
         Assert.True(ShellGeometry.RailBesideHelp(1100, 380));
         // The pane widened by its splitter to its 760-pixel cap.
         Assert.True(ShellGeometry.RailBesideHelp(1280, 760));
         Assert.False(ShellGeometry.RailBesideHelp(1920, 760));
         // A pane that is not open asks for nothing.
         Assert.False(ShellGeometry.RailBesideHelp(800, 0));
         Assert.False(ShellGeometry.RailBesideHelp(0, 380));
      }

      [Fact]
      public void TheWindow_SizesTheTreeFromTheWindow_AndAsksAgainAsThePaneOpensClosesAndIsDragged()
      {
         string shell = ConfirmationPatternTests.ReadControlPanelFile("MainWindow.xaml.cs");
         string apply = WithheldRowTests.MemberText(shell, "private void ApplySidebar_()");
         Assert.True(apply != null, "MainWindow.ApplySidebar_ is gone.");
         Assert.Contains("|| RailForHelp_();", apply, StringComparison.Ordinal);
         Assert.Contains("SidebarColumn.Width = new GridLength(ShellGeometry.SidebarWidth(BodyWidth_));", apply, StringComparison.Ordinal);
         Assert.Contains("SizeSidebar_();", apply, StringComparison.Ordinal);
         Assert.DoesNotContain("GridUnitType.Star", apply, StringComparison.Ordinal);

         string open = WithheldRowTests.MemberText(shell, "private void OpenHelp_()");
         string close = WithheldRowTests.MemberText(shell, "private void CloseHelp_()");
         Assert.True(open != null && close != null, "OpenHelp_ or CloseHelp_ is gone.");
         Assert.Contains("HelpPaneMoved_(true);", open, StringComparison.Ordinal);
         Assert.Contains("HelpPaneMoved_(true);", close, StringComparison.Ordinal);
         // The pane's width is set before the sidebar is asked about it.
         Assert.True(open.IndexOf("HelpColumn.Width = ", StringComparison.Ordinal) < open.IndexOf("HelpPaneMoved_(true);", StringComparison.Ordinal),
            "OpenHelp_ asks the sidebar about the pane before the pane has its width.");

         string geometry = ConfirmationPatternTests.ReadControlPanelFile("MainWindow.Geometry.cs");
         Assert.Contains("HelpSplitter.DragCompleted += (s, e) => HelpPaneMoved_(false);", geometry, StringComparison.Ordinal);
         Assert.Contains("ShellGeometry.RailBesideHelp(BodyWidth_, HelpWidth_)", geometry, StringComparison.Ordinal);
         Assert.Contains("WireShellGeometry_();", shell, StringComparison.Ordinal);

         // A glyph of the rail the pane made opens its group's flyout, as below the
         // collapse threshold, where an expansion would squeeze the page.
         string flyout = ConfirmationPatternTests.ReadControlPanelFile("MainWindow.NavFlyout.cs");
         Assert.Contains("private bool TooNarrowForTree_ => ActualWidth < DesignTokens.Shell.CollapseSidebarBelow || RailForHelp_();", flyout, StringComparison.Ordinal);

         // An expansion the administrator asks for beside the pane is kept until the pane closes.
         string toggle = WithheldRowTests.MemberText(shell, "private void SidebarToggle_Click(object sender, RoutedEventArgs e)");
         Assert.True(toggle != null, "MainWindow.SidebarToggle_Click is gone.");
         Assert.Contains("treeBesideHelpChosen_ = !sidebarCollapsedByUser_;", toggle, StringComparison.Ordinal);
      }

      /// <summary>Every top-level row - each group, and Welcome, the Dashboard and About - has a glyph, and only those rows do.</summary>
      [Fact]
      public void EveryTopLevelRow_HasAGlyph_AndNoPageInAGroupDoes()
      {
         foreach (NavNode root in NavigationMap.Roots)
            Assert.False(string.IsNullOrEmpty(root.Icon), root.Title + " has no glyph, so the rail draws its translated initial and its label starts left of the others.");

         foreach (NavNode group in NavigationMap.Roots.Where(node => !node.IsPage))
         {
            foreach (NavNode page in group.Children)
               Assert.True(string.IsNullOrEmpty(page.Icon), page.Title + " carries a glyph inside its group.");
         }

         Assert.Equal("Home24", NavigationMap.Find("welcome").Icon);
         Assert.Equal("DataUsage24", NavigationMap.Find("dashboard").Icon);
         Assert.Equal("Info24", NavigationMap.Find("about").Icon);
      }

      /// <summary>
      /// No two rows of the rail share a glyph, and no two neighbours are drawn from
      /// the same family: Spam &amp; virus filtering's ShieldCheckmark24 and Sign-in &amp;
      /// access's ShieldKeyhole24 were two near-identical shields side by side at 18
      /// pixels.
      /// </summary>
      [Fact]
      public void TheRailsGlyphs_AreDistinct_AndNeighboursAreNotOfOneFamily()
      {
         List<string> icons = NavigationMap.Roots.Select(root => root.Icon).ToList();
         Assert.Equal(icons.Count, icons.Distinct(StringComparer.Ordinal).Count());

         for (int i = 1; i < icons.Count; i++)
         {
            string family = Family_(icons[i]);
            Assert.False(string.Equals(family, Family_(icons[i - 1]), StringComparison.Ordinal),
               NavigationMap.Roots[i - 1].Title + " and " + NavigationMap.Roots[i].Title + " are both drawn as a " + family + ".");
         }

         NavNode signIn = NavigationMap.Roots.Single(root => root.Title == "Sign-in & access");
         Assert.Equal("PersonLock24", signIn.Icon);
      }

      /// <summary>The breadcrumb is one line: the page's name is cut short, never put on a second line, and the Related links keep a gap from the trail.</summary>
      [Fact]
      public void TheBreadcrumb_IsOneLine_AndKeepsAGapBeforeTheRelatedLinks()
      {
         string xaml = ConfirmationPatternTests.ReadControlPanelFile("MainWindow.xaml");
         Assert.Matches(new Regex(@"<DockPanel x:Name=""BreadcrumbHost""[^>]*LastChildFill=""True"""), xaml);
         Assert.DoesNotMatch(new Regex(@"<WrapPanel x:Name=""BreadcrumbHost"""), xaml);
         Assert.Matches(new Regex(@"<WrapPanel x:Name=""RelatedHost""[^>]*Margin=""12,0,0,0"""), xaml);

         string shell = ConfirmationPatternTests.ReadControlPanelFile("MainWindow.xaml.cs");
         string crumb = WithheldRowTests.MemberText(shell, "private void UpdateBreadcrumb(string key)");
         Assert.True(crumb != null, "MainWindow.UpdateBreadcrumb is gone.");
         int here = crumb.IndexOf("var here = new TextBlock", StringComparison.Ordinal);
         Assert.True(here >= 0, "The breadcrumb's page segment is no longer a TextBlock.");
         string segment = crumb.Substring(here, crumb.IndexOf("};", here, StringComparison.Ordinal) - here);
         Assert.Contains("TextTrimming = TextTrimming.CharacterEllipsis", segment, StringComparison.Ordinal);
         Assert.Contains("ToolTip = L(node.Title)", segment, StringComparison.Ordinal);
         // The page's segment is the bar's last part, the one the DockPanel fills with what is left.
         Assert.True(crumb.IndexOf("BreadcrumbHost.Children.Add(here);", StringComparison.Ordinal) > crumb.IndexOf("BreadcrumbHost.Children.Add(BreadcrumbSeparator());", StringComparison.Ordinal),
            "The page's segment is not added after the trail's other parts.");

         // And where the bar leaves less than the group's words beside the help pane,
         // the group's button is cut short with an ellipsis too, never clipped mid-word.
         string group = WithheldRowTests.MemberText(shell, "private FrameworkElement GroupSegment(NavNode group)");
         Assert.True(group != null, "MainWindow.GroupSegment is gone.");
         Assert.Contains("Content = new TextBlock { Text = L(group.Title), TextTrimming = TextTrimming.CharacterEllipsis },", group, StringComparison.Ordinal);
      }

      /// <summary>A glyph's family: its name without the size and without the qualifier after the first word - "ShieldKeyhole24" is a Shield.</summary>
      private static string Family_(string icon)
      {
         Match first = Regex.Match(icon ?? "", "^[A-Z][a-z]+");
         return first.Success ? first.Value : icon;
      }
   }
}
