// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The rules page had the domains page's shape: a grid held to the window over a
   /// fixed 620 pixels, the list of rules and the row of criteria and actions sharing
   /// what its toolbar and add form left, one to 1.3. In the narrowest window (760 by
   /// 520) the actions list had no height at all and the criteria list less than a
   /// row, so a rule's actions could not be seen; in German, where the toolbar wraps
   /// to three lines and the actions' buttons to four, the criteria list had none
   /// either and Down ran past the 620 pixels, where scrolling could not reach it;
   /// and the match list ran off its card, as it did in the account dialog's Rules
   /// tab. Every test here fails against that page.
   /// </summary>
   public class RulesPageTests
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";
      private static readonly XNamespace Scaffold = "clr-namespace:hMailServer.ControlPanel.Views.Scaffold";

      // Each list and its floor: three rows of 34 pixels, and the rules' column headers.
      // And a pixel for layout rounding: at 135 and 102 the rule grid held two whole rows
      // at 150 and 175 % scaling, its height rounded a device pixel short and its header
      // one long, and the actions grid at 175 %.
      private const double RoundingSlack = 1;

      public static TheoryData<string, double> Lists => new()
      {
         { "RuleGrid", 33 + 3 * 34 },
         { "CriteriaGrid", 3 * 34 },
         { "ActionsGrid", 3 * 34 },
      };

      public static TheoryData<string> ListNames => new() { "RuleGrid", "CriteriaGrid", "ActionsGrid" };

      [Theory]
      [MemberData(nameof(Lists))]
      public void EveryList_IsTheFillingPartOfAFillPanel_WithAFloorOfThreeRows(string list, double floor)
      {
         XElement grid = Named(Page(), list);
         Assert.Equal(Wpf + "DataGrid", grid.Name);
         Assert.True((string)grid.Attribute(Scaffold + "FillPanel.Fill") == "True", list + " is not the filling part of the panel around it, so nothing gives it the height the window leaves.");
         Assert.Equal(Scaffold + "FillPanel", grid.Parent.Name);

         double minHeight = double.Parse((string)grid.Attribute("MinHeight") ?? "0", CultureInfo.InvariantCulture);
         Assert.True(minHeight >= floor + RoundingSlack,
            list + "'s floor is " + minHeight + " pixels, less than its three rows' " + floor + " and " + RoundingSlack + " for layout rounding.");
      }

      [Fact]
      public void TheListsAndTheCriteriaAndActions_ShareTheWindow_OneTo1Point3()
      {
         XElement page = Page();
         XElement scroll = Named(page, "PageScroll");
         XElement panel = scroll.Elements(Scaffold + "FillPanel").SingleOrDefault();
         Assert.True(panel != null, "The page ScrollViewer's content is not a FillPanel, so nothing sizes the lists from the parts around them.");
         Assert.Equal("{Binding ViewportHeight, ElementName=PageScroll}", (string)panel.Attribute("ViewportHeight"));

         List<XElement> filling = panel.Elements().Where(e => (string)e.Attribute(Scaffold + "FillPanel.Fill") == "True").ToList();
         Assert.Equal(2, filling.Count);
         Assert.Contains(filling[0].Descendants(), e => (string)e.Attribute(X + "Name") == "RuleGrid");
         Assert.Equal("RuleParts", (string)filling[1].Attribute(X + "Name"));
         Assert.Equal("1.3", (string)filling[1].Attribute(Scaffold + "FillPanel.Weight"));

         // Floored at what they hold, not at a fixed MinHeight: the actions' buttons
         // wrap to three lines in the narrowest window.
         foreach (XElement part in filling)
            Assert.Null(part.Attribute("MinHeight"));
      }

      [Theory]
      [MemberData(nameof(ListNames))]
      public void NoGridRowAndNoFixedHeight_StandsBetweenAListAndThePage(string list)
      {
         XElement page = Page();
         XElement scroll = Named(page, "PageScroll");
         for (XElement e = Named(page, list).Parent; e != null && e != scroll; e = e.Parent)
         {
            Assert.False(e.Elements(Wpf + "Grid.RowDefinitions").Any(),
               list + " sits in a row of a " + e.Name.LocalName + ", whose height is shared out before the parts beside it are measured.");
            Assert.True(e.Attribute("MinHeight") == null,
               list + " sits in a " + e.Name.LocalName + " with a fixed MinHeight, which cuts off whatever its parts need beyond it.");
         }

         foreach (XElement e in page.Descendants())
         {
            string height = (string)e.Attribute("Height") ?? "";
            Assert.False(height.Contains("ViewportHeight", StringComparison.Ordinal),
               e.Name.LocalName + " is held to the viewport's height, which cuts off whatever its parts need beyond it (the actions' Down).");
         }
      }

      [Fact]
      public void EveryRuleColumn_HasAFloor_SoANarrowWindowScrollsTheGridSideways()
      {
         XElement page = Page();
         // The number, the name, the summary (review finding F309) and whether the rule
         // is on - a template column, its On or Off a pill (StateMarksTests).
         List<XElement> columns = Named(page, "RuleGrid").Descendants()
            .Where(e => e.Name == Wpf + "DataGridTextColumn" || e.Name == Wpf + "DataGridTemplateColumn").ToList();
         Assert.Equal(4, columns.Count);
         foreach (XElement column in columns)
            Assert.True(column.Attribute("MinWidth") != null, "The column " + (string)column.Attribute("Header") + " has no MinWidth, so a narrow window squeezes it to nothing.");

         XElement name = columns.Single(c => (string)c.Attribute("Header") == "{loc:L 'Name'}");
         Assert.True(double.Parse((string)name.Attribute("MinWidth"), CultureInfo.InvariantCulture) >= 120,
            "The name, the one column that says which rule a row is, is narrower than a name.");

         // A criterion or an action longer than its card ends in an ellipsis and is
         // whole in its tool tip, where it was cut off mid-word.
         foreach (string list in new[] { "CriteriaGrid", "ActionsGrid" })
         {
            XElement only = Named(page, list).Descendants(Wpf + "DataGridTextColumn").Single();
            Assert.Equal("{StaticResource GridTrimmedText}", (string)only.Attribute("ElementStyle"));
            Assert.NotNull(only.Attribute("MinWidth"));
         }
      }

      [Fact]
      public void ANarrowWindow_PutsTheCriteriaAndActions_OneAboveTheOther()
      {
         XElement page = Page();
         XElement parts = Named(page, "RuleParts");
         Assert.Equal(Wpf + "UniformGrid", parts.Name);
         Assert.Equal("RuleParts_SizeChanged", (string)parts.Attribute("SizeChanged"));
         Assert.Same(parts, Named(page, "CriteriaCard").Parent);
         Assert.Same(parts, Named(page, "ActionsCard").Parent);

         string code = ReadControlPanelFile(Path.Join("Views", "RulesView.xaml.cs"));
         string arrange = WithheldRowTests.MemberText(code, "private void RuleParts_SizeChanged(object sender, SizeChangedEventArgs e)");
         Assert.True(arrange != null, "RulesView.RuleParts_SizeChanged is gone.");
         Assert.Contains("SideBySide.Fits(e.NewSize.Width, RulePartWeights, minWidths)", arrange, StringComparison.Ordinal);
         Assert.Contains("RuleParts.Columns = sideBySide ? 2 : 1;", arrange, StringComparison.Ordinal);
         // And where a footer's buttons would not fit on one line: Add action, the
         // Primary, wrapped alone at 1280 (the final check of Tier 2, g2 defect 1).
         Assert.Contains("Math.Max(RulePartMinWidth, FooterOnOneLine_(CriteriaFooter, CriteriaCard))", arrange, StringComparison.Ordinal);
         Assert.Contains("Math.Max(RulePartMinWidth, FooterOnOneLine_(ActionsFooter, ActionsCard))", arrange, StringComparison.Ordinal);
         string footer = WithheldRowTests.MemberText(code, "private static double FooterOnOneLine_(Panel footer, Control card)");
         Assert.True(footer != null, "RulesView.FooterOnOneLine_ is gone.");
         Assert.Contains("line += button.DesiredSize.Width;", footer, StringComparison.Ordinal);
         Assert.Equal(Wpf + "WrapPanel", Named(page, "CriteriaFooter").Name);
         Assert.Equal(Wpf + "WrapPanel", Named(page, "ActionsFooter").Name);

         // Room for the IF caption and the 230-pixel match list inside the card's padding.
         Assert.Contains("private const double RulePartMinWidth = 340;", code, StringComparison.Ordinal);
         Assert.Equal("230", (string)Named(page, "MatchMode").Attribute("MinWidth"));
      }

      /// <summary>
      /// A grid's two scroll bars must not depend on each other. Where a grid's rows fitted
      /// only without a horizontal bar and its columns came out a fraction of a pixel wider
      /// than the grid, as layout rounding makes them at 125 and 175 % scaling, the
      /// horizontal bar came, the rows no longer fitted, the vertical bar came, the columns
      /// narrowed to fit, the horizontal bar went, and so on for ever: the page's layout
      /// never settled and the Control Panel stopped answering - with three criteria in the
      /// narrowest window at 175 %, three rules at the reporter's size at 125 %, and four
      /// rules in a 1280 by 800 window at 175 % in 6.3.6. The criteria and actions, one
      /// column whose text ends in an ellipsis, do not scroll sideways; the rules, whose
      /// columns keep their widths and scroll sideways in a narrow window, keep their
      /// vertical bar.
      /// </summary>
      [Fact]
      public void NoGridsScrollBars_CanTakeTurns()
      {
         XElement page = Page();
         foreach (string list in new[] { "CriteriaGrid", "ActionsGrid" })
         {
            Assert.True((string)Named(page, list).Attribute("ScrollViewer.HorizontalScrollBarVisibility") == "Disabled",
               list + " can show a horizontal scroll bar, which can come and go with its vertical one for ever.");
         }

         XElement rules = Named(page, "RuleGrid");
         Assert.True((string)rules.Attribute("ScrollViewer.HorizontalScrollBarVisibility") != "Disabled",
            "The rule grid no longer scrolls sideways, so a narrow window squeezes its columns.");
         Assert.True((string)rules.Attribute("ScrollViewer.VerticalScrollBarVisibility") == "Visible",
            "The rule grid's vertical scroll bar comes and goes, and with it the horizontal one, for ever.");
      }

      // ---- helpers -------------------------------------------------------------------------

      private static XElement Page() => XDocument.Parse(ReadControlPanelFile(Path.Join("Views", "RulesView.xaml"))).Root;

      private static XElement Named(XElement root, string name)
      {
         XElement found = root.Descendants().FirstOrDefault(e => (string)e.Attribute(X + "Name") == name);
         Assert.True(found != null, "RulesView.xaml has no element named " + name + ".");
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
