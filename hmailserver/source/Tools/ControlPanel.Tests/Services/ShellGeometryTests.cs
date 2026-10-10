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

      /// <summary>
      /// The keyboard stays in the navigation when the tree and the rail change
      /// places under it: F1 on a tree row in a window of 1000 to 1180 pixels makes
      /// the tree the rail beside the help pane, and the row the keyboard was on was
      /// collapsed with nothing told, so that WPF put the keyboard on the window and
      /// the next Tab started from the command bar. Once laid out it goes to the
      /// navigation's own place for the page, as F6 takes it there - unless it is on
      /// something shown already, as closing the pane leaves it on the help button.
      /// </summary>
      [Fact]
      public void TheKeyboard_StaysInTheNavigation_WhenTheTreeAndTheRailChangePlaces()
      {
         string shell = ConfirmationPatternTests.ReadControlPanelFile("MainWindow.xaml.cs");
         string apply = WithheldRowTests.MemberText(shell, "private void ApplySidebar_()");
         Assert.True(apply != null, "MainWindow.ApplySidebar_ is gone.");
         int asked = apply.IndexOf("bool keyboardInSidebar = SidebarPane.IsKeyboardFocusWithin;", StringComparison.Ordinal);
         int switched = apply.IndexOf("NavTree.Visibility = rail ? Visibility.Collapsed : Visibility.Visible;", StringComparison.Ordinal);
         int kept = apply.IndexOf("Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Loaded, new Action(KeepKeyboardInNavigation_));", StringComparison.Ordinal);
         Assert.True(asked >= 0 && switched >= 0 && kept >= 0, "ApplySidebar_ no longer keeps the keyboard in the navigation as the tree and the rail change places.");
         Assert.True(asked < switched && switched < kept, "ApplySidebar_ asks where the keyboard is after the tree and the rail have changed places, or hands it on before.");
         Assert.Contains("if (keyboardInSidebar)", apply, StringComparison.Ordinal);

         string keep = WithheldRowTests.MemberText(shell, "private void KeepKeyboardInNavigation_()");
         Assert.True(keep != null, "MainWindow.KeepKeyboardInNavigation_ is gone.");
         Assert.Contains("if (Keyboard.FocusedElement is UIElement { IsVisible: true } focused && !ReferenceEquals(focused, this))", keep, StringComparison.Ordinal);
         Assert.Contains("IInputElement target = NavigationTarget_();", keep, StringComparison.Ordinal);
         Assert.Contains("Keyboard.Focus(target);", keep, StringComparison.Ordinal);
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
         Assert.Matches(new Regex(@"<scaffold:BreadcrumbTrailPanel x:Name=""BreadcrumbHost"""), xaml);
         Assert.DoesNotMatch(new Regex(@"<WrapPanel x:Name=""BreadcrumbHost"""), xaml);
         // A DockPanel gave the group all the bar had and the page's name none.
         Assert.DoesNotMatch(new Regex(@"<DockPanel x:Name=""BreadcrumbHost"""), xaml);
         string panel = ConfirmationPatternTests.ReadControlPanelFile(System.IO.Path.Join("Views", "Scaffold", "BreadcrumbTrailPanel.cs"));
         Assert.Contains("double[] widths = ShellGeometry.BreadcrumbWidths(availableSize.Width, desired, parts);", panel, StringComparison.Ordinal);
         Assert.Contains("index == InternalChildren.Count - 1 ? BreadcrumbPart.Page", panel, StringComparison.Ordinal);
         Assert.Contains("child is TextBlock ? BreadcrumbPart.Fixed", panel, StringComparison.Ordinal);
         Assert.Contains("child is ButtonBase ? BreadcrumbPart.Group", panel, StringComparison.Ordinal);
         Assert.Contains(": BreadcrumbPart.Marker;", panel, StringComparison.Ordinal);
         // Only a part that is cut is measured again, in its share: measured again in
         // its own width a group's words came out short and were cut with room to spare.
         Assert.Matches(new Regex(@"if \(widths\[i\] < desired\[i\]\)\s+children\[i\]\.Measure\(new Size\(Math\.Max\(0, widths\[i\]\), availableSize\.Height\)\);"), panel);
         // And each is placed at the width it measured to, the next straight after it.
         Assert.Contains("double width = children[i].DesiredSize.Width;", panel, StringComparison.Ordinal);
         Assert.Contains("children[i].Arrange(new Rect(x, 0, width, finalSize.Height));", panel, StringComparison.Ordinal);
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
         // And the search's marker before the trail, cut with the groups.
         string marker = WithheldRowTests.MemberText(shell, "private static FrameworkElement SearchMarker()");
         Assert.True(marker != null, "MainWindow.SearchMarker is gone.");
         Assert.Contains("TextTrimming = TextTrimming.CharacterEllipsis", marker, StringComparison.Ordinal);
      }

      /// <summary>
      /// The page's name has the priority on a line too short for the trail: the
      /// groups are cut first, to 48 pixels, then the page's name to 96, then the
      /// groups to their ellipsis, and the page's name last. Beside the help pane in
      /// a window of 760 the bar left the trail about 100 pixels, the group took
      /// them all, and the page's name was given none (review finding F047).
      /// </summary>
      [Fact]
      public void TheBreadcrumb_CutsTheGroupsBeforeThePagesName()
      {
         BreadcrumbPart[] trail = { BreadcrumbPart.Group, BreadcrumbPart.Fixed, BreadcrumbPart.Page };

         // Room for all: each as wide as it is.
         Assert.Equal(new[] { 170.0, 15, 185 }, ShellGeometry.BreadcrumbWidths(400, new[] { 170.0, 15, 185 }, trail));
         Assert.Equal(new[] { 170.0, 15, 185 }, ShellGeometry.BreadcrumbWidths(double.PositiveInfinity, new[] { 170.0, 15, 185 }, trail));

         // Short by 70: the group alone gives way, and the page's name is whole.
         Assert.Equal(new[] { 100.0, 15, 185 }, ShellGeometry.BreadcrumbWidths(300, new[] { 170.0, 15, 185 }, trail));

         // Short by 170: the group to its floor, then the page's name cut by what is still to give.
         Assert.Equal(new[] { 48.0, 15, 137 }, ShellGeometry.BreadcrumbWidths(200, new[] { 170.0, 15, 185 }, trail));

         // Beside the help pane at 760, "Accounts & domains > Domains": the page's
         // name whole, the group under its floor rather than the page's name cut.
         Assert.Equal(new[] { 36.0, 15, 50 }, ShellGeometry.BreadcrumbWidths(101, new[] { 120.0, 15, 50 }, trail));

         // "Certificates & encryption > Automatic certificates (ACME)" there: the
         // group to its ellipsis, the page's name what is left - never nothing.
         double[] acme = ShellGeometry.BreadcrumbWidths(101, new[] { 170.0, 15, 185 }, trail);
         Assert.Equal(ShellGeometry.BreadcrumbGroupLeast, acme[0]);
         Assert.Equal(15, acme[1]);
         Assert.Equal(62, acme[2], 6);

         // The search's marker and a group, cut first, share each cut by what each can give.
         BreadcrumbPart[] found = { BreadcrumbPart.Marker, BreadcrumbPart.Group, BreadcrumbPart.Fixed, BreadcrumbPart.Page };
         double[] shared = ShellGeometry.BreadcrumbWidths(311, new[] { 98.0, 148, 15, 100 }, found);
         Assert.Equal(98 - 50 * 50.0 / 150, shared[0], 6);
         Assert.Equal(148 - 50 * 100.0 / 150, shared[1], 6);
         Assert.Equal(100, shared[3]);

         // Shorter, the marker is left out before the page's name is cut; the group
         // keeps its floor.
         double[] marked = ShellGeometry.BreadcrumbWidths(200, new[] { 98.0, 148, 15, 185 }, found).Select(width => Math.Round(width, 6)).ToArray();
         Assert.Equal(new[] { 0.0, ShellGeometry.BreadcrumbGroupFloor, 15, 137 }, marked);
         // And beside the help pane at 760 the marker is gone, the group at its
         // ellipsis and the page's name what is left, as without the marker.
         Assert.Equal(new[] { 0.0, ShellGeometry.BreadcrumbGroupLeast, 15, 62 },
            ShellGeometry.BreadcrumbWidths(101, new[] { 98.0, 170, 15, 185 }, found).Select(width => Math.Round(width, 6)).ToArray());

         // Whatever the width, no part is wider than it is, none is negative, and the
         // line is filled no further than it is long while the parts can give.
         for (double available = 0; available <= 420; available += 7)
         {
            double[] widths = ShellGeometry.BreadcrumbWidths(available, new[] { 170.0, 15, 185 }, trail);
            Assert.All(widths, width => Assert.True(width >= 0));
            Assert.True(widths[0] <= 170 && widths[2] <= 185);
            Assert.Equal(15, widths[1]);
            if (available >= 15 + ShellGeometry.BreadcrumbGroupLeast)
               Assert.True(widths.Sum() <= available + 1e-9, "The trail overruns a line of " + available + ".");
            // The page's name is never cut while a group is wider than its floor.
            if (widths[2] < 185)
               Assert.True(widths[0] <= ShellGeometry.BreadcrumbGroupFloor + 1e-9, "The page's name was cut before the group at " + available + ".");
         }
      }

      /// <summary>
      /// No part is ever given less than nothing, nor less than the floor of the
      /// cut that took from it, whatever the widths: in widths of thirds of a pixel,
      /// as at 150 %, a share worked out to a hair under zero - the search's marker
      /// at -7.1E-15 in a trail of 273 pixels - the trail's panel asked for a Size
      /// of negative width, and the layout threw, which ends the program; the
      /// render harness met it on a page opened from search beside the help pane.
      /// </summary>
      [Fact]
      public void TheBreadcrumbsWidths_AreNeverNegative_NorUnderTheirFloors()
      {
         BreadcrumbPart[] found = { BreadcrumbPart.Marker, BreadcrumbPart.Group, BreadcrumbPart.Fixed, BreadcrumbPart.Page };

         // Three that came out under zero.
         Assert.All(ShellGeometry.BreadcrumbWidths(273.0, new[] { 95.33333333333333, 195.0, 8.0, 223.66666666666666 }, found), width => Assert.True(width >= 0, width.ToString("R", System.Globalization.CultureInfo.InvariantCulture)));
         Assert.All(ShellGeometry.BreadcrumbWidths(9.0, new[] { 88.33333333333333, 112.66666666666667, 8.0, 180.33333333333334 }, found), width => Assert.True(width >= 0, width.ToString("R", System.Globalization.CultureInfo.InvariantCulture)));
         Assert.All(ShellGeometry.BreadcrumbWidths(9.666666666666666, new[] { 115.33333333333333, 21.666666666666668, 8.0, 198.66666666666666 }, found), width => Assert.True(width >= 0, width.ToString("R", System.Globalization.CultureInfo.InvariantCulture)));

         double[][] trails =
         {
            new[] { 98.6667, 142, 8, 162.6667 },
            new[] { 118, 175.3333, 8, 178.0001 },
            new[] { 112.6667, 91.3333, 8, 104 },
            new[] { 0.3333, 47.9999, 8, 96.0001 },
         };
         foreach (double[] desired in trails)
         {
            for (double available = 0; available <= 520; available += 1.0 / 3)
            {
               double[] widths = ShellGeometry.BreadcrumbWidths(available, desired, found);
               for (int i = 0; i < widths.Length; i++)
               {
                  Assert.True(widths[i] >= 0, "Part " + i + " is " + widths[i] + " wide in " + available + ".");
                  Assert.True(widths[i] <= desired[i], "Part " + i + " is wider than it is in " + available + ".");
               }

               // The group under its ellipsis never, the page's name under its floor
               // only once the group is at its ellipsis.
               Assert.True(widths[1] >= Math.Min(desired[1], ShellGeometry.BreadcrumbGroupLeast) - 1e-9, "The group went under its ellipsis in " + available + ".");
               if (widths[3] < Math.Min(desired[3], ShellGeometry.BreadcrumbPageFloor) - 1e-9)
                  Assert.True(widths[1] <= ShellGeometry.BreadcrumbGroupLeast + 1e-9, "The page's name went under its floor before the group reached its ellipsis in " + available + ".");
            }
         }
      }

      /// <summary>
      /// A page the wider sidebar narrowed keeps what it would have cut to nothing:
      /// the audit trail's Detail, a star column with no floor, went from about 45
      /// pixels to 17 at 1280 - its header "D..." and its cells a letter - so it is
      /// floored at 120 and the grid scrolls sideways, keeping its upright bar; and
      /// the columns before it are floored at their words as each page of the trail
      /// is shown, since with Detail floored a grid narrowed after it was laid out
      /// wider took what Detail lacked from them and cut 24 of their cells at 1280.
      /// </summary>
      [Fact]
      public void TheAuditTrail_BesideTheWiderSidebar_ScrollsSidewaysRatherThanCutDetailToALetter()
      {
         string page = ConfirmationPatternTests.ReadControlPanelFile(System.IO.Path.Join("Views", "AuditTrailView.cs"));
         Match floor = Regex.Match(page, @"detail_?\.MinWidth = (?:Math\.Max\()?(\d+)");
         Assert.True(floor.Success, "The audit trail's Detail column has no floor.");
         Assert.True(int.Parse(floor.Groups[1].Value, System.Globalization.CultureInfo.InvariantCulture) >= 120, "The audit trail's Detail column is floored under 120 pixels.");
         Assert.Contains("ScrollViewer.SetVerticalScrollBarVisibility(rows_, ScrollBarVisibility.Visible);", page, StringComparison.Ordinal);
         string reload = WithheldRowTests.MemberText(page, "private void Reload()");
         Assert.True(reload != null, "AuditTrailView.Reload is gone.");
         Assert.Contains("GridStyles.FloorAtTheirWords(rows_, shown, detail_);", reload, StringComparison.Ordinal);
         Assert.True(reload.IndexOf("rows_.ItemsSource = shown;", StringComparison.Ordinal) < reload.IndexOf("GridStyles.FloorAtTheirWords(", StringComparison.Ordinal),
            "The audit trail floors its columns before the rows they are floored at are shown.");
      }

      /// <summary>A glyph's family: its name without the size and without the qualifier after the first word - "ShieldKeyhole24" is a Shield.</summary>
      private static string Family_(string icon)
      {
         Match first = Regex.Match(icon ?? "", "^[A-Z][a-z]+");
         return first.Success ? first.Value : icon;
      }
   }
}
