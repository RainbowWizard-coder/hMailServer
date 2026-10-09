// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The list pages that kept the shape the IP ranges page lost its list to (work
   /// item 79): the list in a star row of the page's grid between rows sized to
   /// their content, which a window shorter than those rows gives nothing. In the
   /// smallest window the Control Panel allows (760 by 520) the ports page had no
   /// grid at all between its toolbar and its warning, the profiles page lost its
   /// table and nothing scrolled, the diagnostics page's results card had no
   /// height, the greylisting bypass list showed its header row alone, the
   /// quarantine, domain policy and routes pages one row or half a row, the
   /// discovery page's lines no room for a row, and the suppression list's bounce
   /// counts were a sliver; and selecting a certificate on the SSL certificates
   /// page, which shows the details card under the list, shrank the list to a
   /// strip of six pixels in a 1280 by 800 window. Every test here fails against
   /// those pages: each list is the filling part of a <c>FillPanel</c> in the
   /// page's scroll viewer, with a floor of three rows (four for the certificates,
   /// so that the selected one is seen among its neighbours), and no grid row
   /// stands between a list and the page.
   /// </summary>
   public class ListPagesFillTests
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";
      private static readonly XNamespace Scaffold = "clr-namespace:hMailServer.ControlPanel.Views.Scaffold";

      // The floor of a list card: the card's padding (16) and border (2), the
      // column headers (33) and three rows of AppNavRowHeight (34); and the empty
      // state that takes the rows' place when there are none, 139 inside the
      // same padding and border. The certificates keep four rows.
      private const double ThreeRows = 16 + 2 + 33 + 3 * 34;
      private const double FourRows = 16 + 2 + 33 + 4 * 34;
      private const double EmptyState = 16 + 2 + 139;

      public static TheoryData<string, string, double> XamlPages => new()
      {
         { "SslCertificatesView.xaml", "CertGrid", FourRows },
         { "TcpIpPortsView.xaml", "PortGrid", ThreeRows },
         { "RoutesView.xaml", "RouteGrid", ThreeRows },
      };

      [Theory]
      [MemberData(nameof(XamlPages))]
      public void TheList_IsTheFillingPartOfAFillPanel_InThePageScrollViewer(string file, string grid, double rows)
      {
         XElement page = Page(file);
         XElement scroll = Named(page, file, "PageScroll");
         Assert.Equal(Wpf + "ScrollViewer", scroll.Name);
         Assert.Equal("Auto", (string)scroll.Attribute("VerticalScrollBarVisibility"));
         Assert.Equal("Disabled", (string)scroll.Attribute("HorizontalScrollBarVisibility"));

         XElement panel = scroll.Elements(Scaffold + "FillPanel").SingleOrDefault();
         Assert.True(panel != null, file + ": the page ScrollViewer's content is not a FillPanel, so nothing sizes the list from the parts around it.");
         Assert.Equal("{Binding ViewportHeight, ElementName=PageScroll}", (string)panel.Attribute("ViewportHeight"));

         List<XElement> filling = panel.Elements().Where(e => (string)e.Attribute(Scaffold + "FillPanel.Fill") == "True").ToList();
         Assert.True(filling.Count == 1, file + ": the FillPanel has " + filling.Count + " filling parts; the list's card is to be the one.");
         Assert.Contains(filling[0].Descendants(Wpf + "DataGrid"), g => (string)g.Attribute(X + "Name") == grid);

         double floor = double.Parse((string)filling[0].Attribute("MinHeight") ?? "0", CultureInfo.InvariantCulture);
         Assert.True(floor >= rows, file + ": the list's floor is " + floor + " pixels, less than " + rows + ".");
         Assert.True(floor >= EmptyState, file + ": the list's floor is " + floor + " pixels, which cuts off the empty state's sentence.");
      }

      [Theory]
      [MemberData(nameof(XamlPages))]
      public void NoGridRowAndNoFixedHeight_StandsBetweenTheListAndThePage(string file, string grid, double rows)
      {
         Assert.True(rows > 0);
         XElement page = Page(file);
         XElement list = Named(page, file, grid);
         XElement scroll = Named(page, file, "PageScroll");

         for (XElement e = list.Parent; e != null && e != scroll; e = e.Parent)
         {
            Assert.False(e.Elements(Wpf + "Grid.RowDefinitions").Any(),
               file + ": the list sits in a row of a " + e.Name.LocalName + ", which a window shorter than the other rows gives nothing.");
         }

         foreach (XElement e in page.Descendants())
         {
            string height = (string)e.Attribute("Height") ?? "";
            Assert.False(height.Contains("ViewportHeight", StringComparison.Ordinal),
               file + ": " + e.Name.LocalName + " is held to the viewport's height, which cuts off whatever its parts need beyond it.");
         }
      }

      /// <summary>Every part of the page is under the page header, in the one scroll viewer.</summary>
      [Theory]
      [MemberData(nameof(XamlPages))]
      public void ThePageHeader_IsTheOnlyPartOutsideTheScrollViewer(string file, string grid, double rows)
      {
         Assert.True(rows > 0 && grid.Length > 0);
         XElement page = Page(file);
         XElement root = page.Elements(Wpf + "Grid").Single();
         List<XElement> parts = root.Elements().Where(e => e.Name != Wpf + "Grid.RowDefinitions").ToList();
         Assert.Equal(2, parts.Count);
         Assert.Equal(Scaffold + "PageHeader", parts[0].Name);
         Assert.Equal(Wpf + "ScrollViewer", parts[1].Name);
      }

      // The pages built in code, each with the member that lays it out and the
      // line that floors its list: the list's card at the list card's floor, or,
      // on the profiles page, whose card holds a summary and the steps as well as
      // the table, the table itself at three rows and the card at what it holds.
      public static TheoryData<string, string, string> CodePages => new()
      {
         { "ProfilesView.cs", "private void Build()", "FillPanel.Filling(rows_, 0)" },
         { "UtilityViews.cs", "public DiagnosticsView()", "FillPanel.Filling(card, FillPanel.ListCardFloor)" },
         { "CollectionEditorView.cs", "private void Build()", "FillPanel.Filling(card, FillPanel.ListCardFloor)" },
         { "QuarantineView.cs", "private void Build()", "FillPanel.Filling(card, FillPanel.ListCardFloor)" },
         { "DomainPolicyListView.cs", "private void Build()", "FillPanel.Filling(listCard, FillPanel.ListCardFloor)" },
         { "DiscoveryView.cs", "private void Build_()", "FillPanel.Filling(casesCard, FillPanel.ListCardFloor)" },
         { "SuppressionListView.cs", "private void Build()", "FillPanel.Filling(listCard, FillPanel.ListCardFloor, 2)" },
      };

      /// <summary>
      /// The page's parts go in a FillPanel bound to the page's scroll viewer
      /// (FillPanel.ScrollingPage), the list is a filling part with a floor of
      /// three rows, and the one star row of the page's grid is the scroll
      /// viewer's.
      /// </summary>
      [Theory]
      [MemberData(nameof(CodePages))]
      public void TheList_IsTheFillingPartOfAFillPanel_InThePageScrollViewer_BuiltInCode(string file, string member, string floor)
      {
         string code = ReadControlPanelFile(Path.Join("Views", file));
         string layout = WithheldRowTests.MemberText(code, member);
         Assert.True(layout != null, file + ": " + member + " is gone.");

         Assert.Contains("FillPanel.ScrollingPage(out", layout, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"Grid\.SetRow\(scroll, \d\);"), layout);
         Assert.Contains(floor, layout, StringComparison.Ordinal);

         int starRows = Regex.Matches(layout, @"RowDefinition \{ Height = new GridLength\(\d+, GridUnitType\.Star\) \}").Count;
         Assert.True(starRows == 1, file + ": the page's grid has " + starRows + " star rows; the scroll viewer's is to be the one, so that no list is given a short window's leftovers.");
      }

      // Every page whose scroll viewer is written in XAML, the domains and rules
      // pages among them: their lists are several, but the one scroll viewer is the
      // same.
      public static TheoryData<string> ScrollingXamlPages => new()
      {
         "SslCertificatesView.xaml", "TcpIpPortsView.xaml", "RoutesView.xaml", "IPRangesView.xaml", "DomainsView.xaml", "RulesView.xaml",
      };

      /// <summary>
      /// The page's scroll bar is drawn over the content, so the scroll viewer keeps
      /// 12 pixels (AppSpaceMd) clear of it on the right: without them its thumb lay
      /// over the right edge of the cards, the notices and the toolbar's last button
      /// wherever a page scrolled - the SSL certificates page with a certificate
      /// selected in a 1280 by 800 window, and every page in the smallest one.
      /// </summary>
      [Theory]
      [MemberData(nameof(ScrollingXamlPages))]
      public void ThePageScrollViewer_KeepsClearOfItsBar(string file)
      {
         XElement scroll = Named(Page(file), file, "PageScroll");
         Assert.Equal("0,0,12,0", (string)scroll.Attribute("Padding"));
      }

      [Fact]
      public void TheScrollingPageBuiltInCode_KeepsClearOfItsBar()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "Scaffold", "FillPanel.cs"));
         string member = WithheldRowTests.MemberText(code, "public static ScrollViewer ScrollingPage(out FillPanel parts)");
         Assert.True(member != null, "FillPanel.ScrollingPage is gone.");
         Assert.Contains("Padding = new Thickness(0, 0, DesignTokens.Space.Md, 0)", member, StringComparison.Ordinal);
      }

      /// <summary>The profiles table's own floor: the column headers, three rows and a pixel for layout rounding.</summary>
      [Fact]
      public void TheProfilesTable_IsFlooredAtThreeRows_AndItsCardAtWhatItHolds()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "ProfilesView.cs"));
         Assert.Contains("MinHeight = 136", code, StringComparison.Ordinal);
         Assert.Contains("FillPanel.Filling(card, 0)", code, StringComparison.Ordinal);
      }

      /// <summary>No list keeps a MinHeight of its own: the card around it is floored, and measured at the height it is given, by the FillPanel.</summary>
      [Theory]
      [InlineData("DiscoveryView.cs")]
      [InlineData("SuppressionListView.cs")]
      [InlineData("DomainPolicyListView.cs")]
      public void TheGrids_HaveNoMinHeightOfTheirOwn(string file)
      {
         string code = ReadControlPanelFile(Path.Join("Views", file));
         Assert.DoesNotContain("MinHeight = 160", code, StringComparison.Ordinal);
         Assert.DoesNotContain("MinHeight = 120", code, StringComparison.Ordinal);
      }

      // ---- helpers -------------------------------------------------------------------------

      private static XElement Page(string file) => XDocument.Parse(ReadControlPanelFile(Path.Join("Views", file))).Root;

      private static XElement Named(XElement root, string file, string name)
      {
         XElement found = root.Descendants().FirstOrDefault(e => (string)e.Attribute(X + "Name") == name);
         Assert.True(found != null, file + " has no element named " + name + ".");
         return found;
      }

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
