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
   /// The domains page as the IP ranges fix (work item 79) left it: a grid held to
   /// the window over a fixed 640 pixels, its accounts card and its row of alias and
   /// list cards sharing that three to two. In the narrowest window the Control
   /// Panel allows (760 by 520) the alias form ran past those 640 pixels, where
   /// scrolling could not reach it, the alias list had no height at all, Create
   /// account ran off the right edge of its card, the alias and list cards were
   /// 104 and 116 pixels wide, and every domain row's Properties and Delete were
   /// off the list's right edge. Every test here fails against that page: each list
   /// is the filling part of a FillPanel with a floor of three rows, the cards share
   /// the window through FillPanels bound to the page's scroll viewer, no grid row
   /// and no fixed height stands between a list and the page, a row is the list's
   /// width with its name cut short, Create account goes under the password where
   /// it does not fit beside it, and a narrow window puts the columns, and then the
   /// alias and list cards, one above the other.
   /// </summary>
   public class DomainsPageTests
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";
      private static readonly XNamespace Scaffold = "clr-namespace:hMailServer.ControlPanel.Views.Scaffold";

      // Each list and the height of one of its rows, as the page draws them.
      // A ListView keeps a pixel above and below its rows (its template's border,
      // Bd, has a padding of 1), so three whole rows need two pixels more: at 120,
      // 114 and 93 the domains, accounts and alias lists' viewports held two whole
      // rows, a third cut short, and scrolled to the end a blank one. And one more
      // for layout rounding: two more alone still held two whole rows at 125, 150,
      // 175 or 225 % scaling, where the list's height rounds down a device pixel.
      private const double ListChrome = 2;
      private const double RoundingSlack = 1;

      public static TheoryData<string, double> Lists => new()
      {
         { "DomainList", 40 },
         { "AccountList", 38 },
         // Two lines: the alias, and under it the address it delivers to (F067).
         { "AliasList", 36 },
         { "DistList", 31 },
      };

      public static TheoryData<string> ListNames => new() { "DomainList", "AccountList", "AliasList", "DistList" };

      [Theory]
      [MemberData(nameof(Lists))]
      public void EveryList_IsTheFillingPartOfAFillPanel_WithAFloorOfThreeRows(string list, double rowHeight)
      {
         XElement view = Named(Page(), list);
         Assert.Equal(Wpf + "ListView", view.Name);
         Assert.True((string)view.Attribute(Scaffold + "FillPanel.Fill") == "True", list + " is not the filling part of the panel around it, so nothing gives it the height the window leaves.");
         Assert.Equal(Scaffold + "FillPanel", view.Parent.Name);

         double floor = double.Parse((string)view.Attribute("MinHeight") ?? "0", CultureInfo.InvariantCulture);
         Assert.True(floor >= 3 * rowHeight + ListChrome + RoundingSlack,
            list + "'s floor is " + floor + " pixels, less than three rows of " + rowHeight + ", the list's own " + ListChrome + " and " + RoundingSlack
            + " for layout rounding, so it shows two whole rows at some scalings or all of them.");
      }

      [Fact]
      public void TheCards_ShareTheWindow_ThroughFillPanelsBoundToThePageScrollViewer()
      {
         XElement page = Page();
         XElement scroll = Named(page, "PageScroll");
         Assert.Equal("Auto", (string)scroll.Attribute("VerticalScrollBarVisibility"));

         foreach (string column in new[] { "DomainColumn", "DetailColumn" })
         {
            XElement panel = Named(page, column);
            Assert.Equal(Scaffold + "FillPanel", panel.Name);
            Assert.Equal("{Binding ViewportHeight, ElementName=PageScroll}", (string)panel.Attribute("ViewportHeight"));
         }

         // The domains card fills its column; the accounts card and the alias and
         // list cards share theirs three to two, as the grid's two rows did.
         Assert.Equal("True", (string)Named(page, "DomainCard").Attribute(Scaffold + "FillPanel.Fill"));
         XElement accounts = Named(page, "AccountsCard");
         XElement listCards = Named(page, "ListCards");
         Assert.Same(Named(page, "DetailColumn"), accounts.Parent);
         Assert.Same(accounts.Parent, listCards.Parent);
         Assert.Equal("True", (string)accounts.Attribute(Scaffold + "FillPanel.Fill"));
         Assert.Equal("True", (string)listCards.Attribute(Scaffold + "FillPanel.Fill"));
         Assert.Equal("3", (string)accounts.Attribute(Scaffold + "FillPanel.Weight"));
         Assert.Equal("2", (string)listCards.Attribute(Scaffold + "FillPanel.Weight"));

         // A filling card without a MinHeight is floored at what it holds - its
         // toolbar, its form and its list's three rows - which a fixed MinHeight
         // on the card would not know.
         foreach (string card in new[] { "DomainCard", "AccountsCard", "ListCards" })
            Assert.Null(Named(page, card).Attribute("MinHeight"));
      }

      /// <summary>
      /// The shape that cut the alias form off: a grid held to the viewport's height
      /// over a fixed MinHeight, between the page's scroll viewer and the lists, its
      /// rows sharing a height that knew nothing of the forms in them.
      /// </summary>
      [Theory]
      [MemberData(nameof(ListNames))]
      public void NoGridRowAndNoFixedHeight_StandsBetweenAListAndThePage(string list)
      {
         XElement page = Page();
         XElement scroll = Named(page, "PageScroll");
         for (XElement e = Named(page, list).Parent; e != null && e != scroll; e = e.Parent)
         {
            Assert.False(e.Elements(Wpf + "Grid.RowDefinitions").Any(),
               list + " sits in a row of a " + e.Name.LocalName + ", whose height is shared out before the forms beside it are measured.");
            Assert.True(e.Attribute("MinHeight") == null,
               list + " sits in a " + e.Name.LocalName + " with a fixed MinHeight, which cuts off whatever its parts need beyond it.");
         }

         foreach (XElement e in page.Descendants())
         {
            string height = (string)e.Attribute("Height") ?? "";
            Assert.False(height.Contains("ViewportHeight", StringComparison.Ordinal),
               e.Name.LocalName + " is held to the viewport's height, which cuts off whatever its parts need beyond it (the alias form).");
         }
      }

      /// <summary>
      /// A row is the list's width, never wider: scrolled sideways, one long domain
      /// name put every row's Properties and Delete off the list's right edge, and in
      /// the narrowest window every row's. A name longer than the room ends in an
      /// ellipsis, and is whole in its tool tip: GridTrimmedText, or a style of the
      /// page's own based on it (the domain and account names' RowName, which
      /// also follows the row's Active flag).
      /// </summary>
      [Theory]
      [MemberData(nameof(ListNames))]
      public void ARow_IsTheListsWidth_AndItsNameIsCutShort_SoItsButtonsStayOnIt(string list)
      {
         XElement page = Page();
         XElement view = Named(page, list);
         Assert.Equal("Disabled", (string)view.Attribute("ScrollViewer.HorizontalScrollBarVisibility"));

         XElement template = view.Descendants(Wpf + "DataTemplate").Single();
         // Every line of words the row binds: the alias row has two, the alias and
         // the address it delivers to (F067), each cut short on its own.
         var names = template.Descendants(Wpf + "TextBlock")
            .Where(t => ((string)t.Attribute("Text") ?? "").StartsWith("{Binding", StringComparison.Ordinal)).ToList();
         Assert.NotEmpty(names);
         foreach (XElement name in names)
         {
            string style = (string)name.Attribute("Style");
            if (style != "{StaticResource GridTrimmedText}")
            {
               Match key = Regex.Match(style ?? "", @"^\{StaticResource (\w+)\}$");
               Assert.True(key.Success, list + "'s name has no trimming style.");
               XElement own = page.Element(Wpf + "UserControl.Resources")?.Elements(Wpf + "Style")
                  .SingleOrDefault(s => (string)s.Attribute(X + "Key") == key.Groups[1].Value);
               Assert.True(own != null, list + "'s name style " + key.Groups[1].Value + " is not a resource of the page.");
               Assert.Equal("{StaticResource GridTrimmedText}", (string)own.Attribute("BasedOn"));
            }
            Assert.DoesNotContain(name.Ancestors(), a => a.Name == Wpf + "StackPanel" && (string)a.Attribute("Orientation") == "Horizontal");
         }
      }

      /// <summary>
      /// The rows carry a four-pixel margin of their own, and their heights are
      /// what the floors above are built on, so the page narrows the list row's
      /// inset to six pixels at the sides and nothing above and below - in its
      /// own resources, based on the application's row, and not in App.xaml,
      /// where the same six left the incoming relays' bare rows three pixels
      /// from the pill. No list on the page styles its items otherwise.
      /// </summary>
      [Theory]
      [MemberData(nameof(ListNames))]
      public void TheRows_NarrowTheListRowsInset_InThePagesOwnResources(string list)
      {
         XElement page = Page();
         XElement own = Assert.Single(page.Element(Wpf + "UserControl.Resources").Elements(Wpf + "Style"),
            s => (string)s.Attribute("TargetType") == "ListViewItem" && s.Attribute(X + "Key") == null);
         Assert.Equal("{StaticResource {x:Type ListViewItem}}", (string)own.Attribute("BasedOn"));
         XElement setter = Assert.Single(own.Elements());
         Assert.Equal(Wpf + "Setter", setter.Name);
         Assert.Equal("Padding", (string)setter.Attribute("Property"));
         Assert.Equal("6,0", (string)setter.Attribute("Value"));

         XElement view = Named(page, list);
         Assert.Null(view.Attribute("ItemContainerStyle"));
         Assert.Empty(view.Elements(Wpf + "ListView.ItemContainerStyle"));
         XElement row = view.Descendants(Wpf + "DataTemplate").Single().Elements().Single();
         Assert.StartsWith("4,", (string)row.Attribute("Margin"), StringComparison.Ordinal);
      }

      [Fact]
      public void CreateAccount_GoesUnderThePassword_WhereItDoesNotFitBesideIt()
      {
         XElement page = Page();
         XElement buttons = Named(page, "AccountButtons");
         Assert.Equal(Wpf + "WrapPanel", buttons.Name);
         Assert.Contains(buttons.Elements(), b => (string)b.Attribute("AutomationProperties.AutomationId") == "domains-accounts");
         Assert.Contains(buttons.Elements(), b => (string)b.Attribute("Click") == "GenerateAccountPassword_Click");
         Assert.Equal("AccountForm_SizeChanged", (string)Named(page, "AccountForm").Attribute("SizeChanged"));

         string code = CodeBehind();
         string wrap = WithheldRowTests.MemberText(code, "private void AccountForm_SizeChanged(object sender, SizeChangedEventArgs e)");
         Assert.True(wrap != null, "DomainsView.AccountForm_SizeChanged is gone.");
         foreach (string step in new[] { "PasswordMinWidth", "Grid.SetRow(AccountButtonsLine, beside ? 1 : 3);", "Grid.SetColumnSpan(AccountButtonsLine, beside ? 1 : 2);",
                                         "AccountButtonsCaption.SharedSizeGroup = beside ? \"PasswordLine\" : null;" })
            Assert.True(wrap.Contains(step, StringComparison.Ordinal), "AccountForm_SizeChanged no longer does " + step);
      }

      [Fact]
      public void ANarrowWindow_PutsTheColumns_AndThenTheAliasAndListCards_OneAboveTheOther()
      {
         XElement page = Page();
         Assert.Equal("PageColumns_SizeChanged", (string)Named(page, "PageColumns").Attribute("SizeChanged"));
         Assert.Equal("ListCards_SizeChanged", (string)Named(page, "ListCards").Attribute("SizeChanged"));
         Assert.Equal(Wpf + "UniformGrid", Named(page, "ListCards").Name);

         string code = CodeBehind();
         string columns = WithheldRowTests.MemberText(code, "private void PageColumns_SizeChanged(object sender, SizeChangedEventArgs e)");
         Assert.True(columns != null && columns.Contains("SideBySide.Fits(e.NewSize.Width, ColumnWeights, ColumnMinWidths)", StringComparison.Ordinal),
            "The page's two columns no longer give way to one where they would be narrower than their minimums.");

         // One column is one FillPanel, so that the domains card shares the window
         // with the others rather than taking one of its own.
         string arrange = WithheldRowTests.MemberText(code, "private void ArrangeColumns(bool twoColumns)");
         Assert.True(arrange != null && arrange.Contains("DetailColumn.Children.Insert(0, DomainCard);", StringComparison.Ordinal),
            "In one column the domains card is no longer the first part of the column's FillPanel.");

         string cards = WithheldRowTests.MemberText(code, "private void ListCards_SizeChanged(object sender, SizeChangedEventArgs e)");
         Assert.True(cards != null && cards.Contains("ListCards.Columns = sideBySide ? 2 : 1;", StringComparison.Ordinal),
            "The alias and list cards no longer go one above the other where they would be too narrow side by side.");

         foreach (string minimum in new[] { "DomainColumnMinWidth = 280", "DetailColumnMinWidth = 400", "ListCardMinWidth = 270", "PasswordMinWidth = 120" })
            Assert.Contains("private const double " + minimum + ";", code, StringComparison.Ordinal);
      }

      // ---- helpers -------------------------------------------------------------------------

      private static XElement Page() => XDocument.Parse(ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml"))).Root;

      private static string CodeBehind() => ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml.cs"));

      private static XElement Named(XElement root, string name)
      {
         XElement found = root.Descendants().FirstOrDefault(e => (string)e.Attribute(X + "Name") == name);
         Assert.True(found != null, "DomainsView.xaml has no element named " + name + ".");
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
