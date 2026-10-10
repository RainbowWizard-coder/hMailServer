// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Three small alignment fixes (T3.3 work item 10): the sign-in card's
   /// two-factor link starts on the card's text column (review finding F032); the
   /// Domains page's domains card has a title, so its search shares a line with the
   /// accounts card's (F100); and in the one column of a narrow window the domains
   /// list shows its rows whole so that only the page scrolls, while every list of
   /// the page keeps its rows clear of its own scroll bar (F101).
   /// </summary>
   public class AlignmentFixesTests
   {
      private static readonly XNamespace Ui = "http://schemas.lepo.co/wpfui/2022/xaml";
      private static readonly XNamespace Xaml = "http://schemas.microsoft.com/winfx/2006/xaml";

      private static XElement Xaml_(string file)
         => XElement.Parse(ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", file)));

      /// <summary>The link's margin takes back the left padding it sets, so its words start where the captions and boxes do.</summary>
      [Fact]
      public void TheSignInCardsLink_StartsOnTheCardsTextColumn()
      {
         XElement link = Xaml_("ConnectView.xaml").Descendants(Ui + "HyperlinkButton").Single();
         double padding = double.Parse(((string)link.Attribute("Padding")).Split(',')[0], CultureInfo.InvariantCulture);
         double margin = double.Parse(((string)link.Attribute("Margin")).Split(',')[0], CultureInfo.InvariantCulture);
         Assert.True(padding > 0, "The link sets no padding of its own, so its margin cannot be held to it.");
         Assert.Equal(-padding, margin);
      }

      [Fact]
      public void TheDomainsCard_HasATitle_AsTheAccountsCardDoes()
      {
         XElement page = Xaml_("DomainsView.xaml");
         XElement card = page.Descendants().Single(e => (string)e.Attribute(Xaml + "Name") == "DomainCard");
         Assert.Equal("{loc:L 'Domains'}", (string)card.Attribute("Title"));
         XElement accounts = page.Descendants().Single(e => (string)e.Attribute(Xaml + "Name") == "AccountsCard");
         Assert.NotNull(accounts.Attribute("Title"));
      }

      /// <summary>Three rows beside the accounts, as many as it shows up to 25 in one column; the XAML's floor is the three rows'.</summary>
      [Fact]
      public void TheDomainsList_ShowsItsRowsWhole_InOneColumn()
      {
         Assert.Equal(123, ListRowsFloor.For(0, 40));
         Assert.Equal(123, ListRowsFloor.For(3, 40));
         Assert.Equal(403, ListRowsFloor.For(10, 40));
         Assert.Equal(ListRowsFloor.MostRows * 40 + ListRowsFloor.Chrome, ListRowsFloor.For(500, 40));

         XElement list = Xaml_("DomainsView.xaml").Descendants().Single(e => (string)e.Attribute(Xaml + "Name") == "DomainList");
         Assert.Equal(ListRowsFloor.For(ListRowsFloor.FewestRows, 40), double.Parse((string)list.Attribute("MinHeight"), CultureInfo.InvariantCulture));

         string code = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml.cs"));
         string floor = WithheldRowTests.MemberText(code, "private void FloorDomainList_()");
         Assert.True(floor != null, "DomainsView.FloorDomainList_ is gone.");
         Assert.Contains("bool oneColumn = !DomainColumn.Children.Contains(DomainCard);", floor, StringComparison.Ordinal);
         Assert.Contains("double floor = oneColumn ? Math.Max(threeRows, ListRowsFloor.For(DomainList.Items.Count, row)) : threeRows;", floor, StringComparison.Ordinal);
         // A row as it is drawn, where the 40 pixels the floor of three rows is built on are not what it measures.
         Assert.Contains("DomainList.ItemContainerGenerator.ContainerFromIndex(0) is FrameworkElement first && first.ActualHeight > 0", floor, StringComparison.Ordinal);
         Assert.Contains("private const double DomainRowHeight = 40;", code, StringComparison.Ordinal);

         // Again as the columns change, as the list is read and as its search filters it.
         foreach (string member in new[]
            {
               "private void PageColumns_SizeChanged(object sender, SizeChangedEventArgs e)",
               "private void ReloadDomains(string select = null)",
               "private void DomainSearch_TextChanged(object sender, EventArgs e)"
            })
         {
            string body = WithheldRowTests.MemberText(code, member);
            Assert.True(body != null && body.Contains("FloorDomainList_();", StringComparison.Ordinal), member + " no longer floors the domains list.");
         }
      }

      [Fact]
      public void EveryListOfTheDomainsPage_KeepsItsRowsClearOfItsBar()
      {
         string code = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml.cs"));
         Assert.Contains("foreach (ListView list in new[] { DomainList, AccountList, AliasList, DistList })", code, StringComparison.Ordinal);
         Assert.Contains("ScrollBarInset.ForList(list);", code, StringComparison.Ordinal);

         string inset = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Services", "ScrollBarInset.cs"));
         string forList = WithheldRowTests.MemberText(inset, "public static void ForList(Control list)");
         Assert.True(forList != null, "ScrollBarInset.ForList is gone.");
         Assert.Contains("SetIsEnabled(viewer, true);", forList, StringComparison.Ordinal);
         Assert.Contains("list.Loaded += Enable;", forList, StringComparison.Ordinal);
      }
   }
}
