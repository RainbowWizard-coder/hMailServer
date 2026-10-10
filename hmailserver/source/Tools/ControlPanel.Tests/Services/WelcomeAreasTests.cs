// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Welcome page's "Or browse by area" (review finding F276). Its six tiles
   /// were written into the page and were not the sidebar's areas: "Domains &amp;
   /// accounts" beside the group "Accounts &amp; domains", "Server settings" promising
   /// five subjects and opening Protocols, the Dashboard (the next sidebar row) and
   /// Transport security (an expert page). The tiles are now the groups of the tree
   /// the credential sees.
   /// </summary>
   public class WelcomeAreasTests
   {
      /// <summary>One tile per group, in the sidebar's order, with the group's own words and glyph.</summary>
      [Fact]
      public void TheAreas_AreTheSidebarsGroups()
      {
         IReadOnlyList<WelcomeArea> areas = WelcomeAreas.For(null);
         List<NavNode> groups = NavigationMap.Roots.Where(node => !node.IsPage).ToList();

         Assert.Equal(groups.Select(g => g.Title), areas.Select(a => a.Title));
         for (int i = 0; i < groups.Count; i++)
         {
            Assert.Equal(groups[i].Purpose, areas[i].Purpose);
            Assert.Equal(groups[i].Icon, areas[i].Icon);
            Assert.False(string.IsNullOrEmpty(areas[i].Icon), groups[i].Title + " has no glyph for its tile.");

            // The tile opens the group's first page, which shows the group in the sidebar.
            NavNode first = groups[i].Children.First(child => child.IsPage);
            Assert.Equal(first.Key, areas[i].Page);
            Assert.Equal(first.Title, areas[i].PageTitle);
         }
      }

      /// <summary>The rows above and below the groups are not areas.</summary>
      [Fact]
      public void TheTopLevelPages_AreNotAreas()
      {
         IReadOnlyList<WelcomeArea> areas = WelcomeAreas.For(null);
         foreach (string page in new[] { "welcome", "dashboard", "about" })
            Assert.DoesNotContain(areas, area => area.Page == page);

         Assert.DoesNotContain(areas, area => area.Title == "Server settings" || area.Title == "Domains & accounts");
      }

      /// <summary>
      /// A role sees the areas its tree has, each opening the first page of the area
      /// it may open: a domain administrator has Accounts &amp; domains, opening
      /// Domains, and Monitoring &amp; troubleshooting, opening the suppression list.
      /// </summary>
      [Fact]
      public void ARole_SeesTheAreasItsTreeHas()
      {
         static bool DomainAdministrator(string needs) => string.IsNullOrEmpty(needs) || needs == "administer_domain";

         IReadOnlyList<WelcomeArea> areas = WelcomeAreas.For(DomainAdministrator);
         Assert.Equal(new[] { "Accounts & domains", "Monitoring & troubleshooting" }, areas.Select(a => a.Title).ToArray());
         Assert.Equal(new[] { "domains", "suppressions" }, areas.Select(a => a.Page).ToArray());

         List<string> visibleGroups = NavigationMap.VisibleRoots(DomainAdministrator).Where(n => !n.IsPage).Select(n => n.Title).ToList();
         Assert.Equal(visibleGroups, areas.Select(a => a.Title));
      }

      /// <summary>
      /// The page draws the tiles from the map as it is entered, so a sign-in as
      /// another role sees its own; the six tiles written into the page are gone.
      /// </summary>
      [Fact]
      public void TheWelcomePage_DrawsTheAreasFromTheMap()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "WelcomeView.cs"));

         string enter = WithheldRowTests.MemberText(code, "public void OnEnter()");
         Assert.True(enter != null, "WelcomeView.OnEnter is gone.");
         Assert.Contains("ShowAreas_();", enter, StringComparison.Ordinal);

         string show = WithheldRowTests.MemberText(code, "private void ShowAreas_()");
         Assert.True(show != null, "WelcomeView.ShowAreas_ is gone.");
         Assert.Contains("IReadOnlyList<WelcomeArea> areas = WelcomeAreas.For(mayUse);", show, StringComparison.Ordinal);
         // Three to a row, each row as tall as its tallest tile.
         Assert.Contains("areas_.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });", show, StringComparison.Ordinal);
         Assert.Contains("session == null || session.Permissions == null ? null : session.May", show, StringComparison.Ordinal);

         string tile = WithheldRowTests.MemberText(code, "private static Wpf.Ui.Controls.Button Tile(WelcomeArea area)");
         Assert.True(tile != null, "WelcomeView.Tile is gone.");
         Assert.Contains("NavigateTo(area.Page)", tile, StringComparison.Ordinal);
         Assert.Contains("L(area.Title)", tile, StringComparison.Ordinal);
         Assert.Contains("L(area.Purpose)", tile, StringComparison.Ordinal);

         foreach (string gone in new[] { "L(\"Server settings\")", "L(\"Domains & accounts\")", "\"protocols\"));", "L(\"Transport security\")" })
            Assert.DoesNotContain(gone, code, StringComparison.Ordinal);
      }

      /// <summary>The comment over the starting points counts none of them, where it said twelve over thirteen.</summary>
      [Fact]
      public void TheStartingPoints_AreNotMiscounted()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "WelcomeView.cs"));
         Assert.DoesNotContain("the twelve reasons", code, StringComparison.Ordinal);
      }

      private static string ReadControlPanelFile(string relative)
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate) && Directory.Exists(Path.Join(directory.FullName, "ControlPanel", "Views")))
               return File.ReadAllText(candidate);
         }

         throw new FileNotFoundException("The Control Panel's source was not found above " + AppContext.BaseDirectory, relative);
      }
   }
}
