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
         Assert.Equal(123, WholeRowsFloor.For(0, 40));
         Assert.Equal(123, WholeRowsFloor.For(3, 40));
         Assert.Equal(403, WholeRowsFloor.For(10, 40));
         Assert.Equal(WholeRowsFloor.MostRows * 40 + WholeRowsFloor.Chrome, WholeRowsFloor.For(500, 40));

         XElement list = Xaml_("DomainsView.xaml").Descendants().Single(e => (string)e.Attribute(Xaml + "Name") == "DomainList");
         Assert.Equal(WholeRowsFloor.For(WholeRowsFloor.FewestRows, 40), double.Parse((string)list.Attribute("MinHeight"), CultureInfo.InvariantCulture));

         string code = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml.cs"));
         string floor = WithheldRowTests.MemberText(code, "private void FloorDomainList_()");
         Assert.True(floor != null, "DomainsView.FloorDomainList_ is gone.");
         Assert.Contains("bool oneColumn = !DomainColumn.Children.Contains(DomainCard);", floor, StringComparison.Ordinal);
         Assert.Contains("double floor = oneColumn ? Math.Max(threeRows, WholeRowsFloor.For(DomainList.Items.Count, DomainRowPitch_())) : threeRows;", floor, StringComparison.Ordinal);
         Assert.Contains("private const double DomainRowHeight = 40;", code, StringComparison.Ordinal);

         // A row as it is drawn, with its container's margin: by the container's
         // height alone (36.67 at the render's scale) four domains were floored at
         // 149.67 where they took 154.67, and the fourth still scrolled in the list.
         string pitch = WithheldRowTests.MemberText(code, "private double DomainRowPitch_()");
         Assert.True(pitch != null, "DomainsView.DomainRowPitch_ is gone.");
         Assert.Contains("DomainList.ItemContainerGenerator.ContainerFromIndex(0) is FrameworkElement first && first.ActualHeight > 0", pitch, StringComparison.Ordinal);
         Assert.Contains("WholeRowsFloor.Pitch(first.ActualHeight, first.Margin.Top, first.Margin.Bottom)", pitch, StringComparison.Ordinal);

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

         // And once the rows it has been given are laid out, so that a floor set
         // before there was a row to measure is set again on the rows as drawn.
         Assert.Contains("DomainList.ItemContainerGenerator.StatusChanged += DomainRows_StatusChanged;", code, StringComparison.Ordinal);
         string generated = WithheldRowTests.MemberText(code, "private void DomainRows_StatusChanged(object sender, EventArgs e)");
         Assert.True(generated != null, "DomainsView.DomainRows_StatusChanged is gone.");
         Assert.Contains("GeneratorStatus.ContainersGenerated", generated, StringComparison.Ordinal);
         Assert.Contains("DispatcherPriority.Loaded, new Action(FloorDomainList_)", generated, StringComparison.Ordinal);
      }

      /// <summary>A row's pitch is its container's height and the margin the container keeps, top and bottom.</summary>
      [Fact]
      public void ARowsPitch_CountsItsContainersMargin()
      {
         Assert.Equal(38.67, WholeRowsFloor.Pitch(36.67, 1, 1), 2);
         Assert.Equal(40, WholeRowsFloor.Pitch(40, 0, 0));
         // Four rows of the theme's 36.67 and two pixels: whole, with the list's chrome.
         Assert.True(WholeRowsFloor.For(4, WholeRowsFloor.Pitch(36.67, 0, 2)) >= 4 * 38.67 + 2);
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
