// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Transport security (review finding F228). Its subtitle said "Outbound mail
   /// authentication and encryption policies." over the cards Authentication results
   /// on inbound mail, Refuse signatures on incoming mail whose expiry has passed and
   /// ARC inbound filtering; the page was one scroll of ten cards, six of them mail
   /// authentication, with Save scrolling away with the header; the ARC card sent the
   /// reader to Anti-spam without a button; and SRS, BATV, DKIM oversigning,
   /// Authentication-Results and DMARC reports were not among its aliases.
   /// </summary>
   public class TransportSecurityTabsTests
   {
      /// <summary>Every card is on a tab, and the tabs come Encryption, Signing, Forwarding, Reports.</summary>
      [Fact]
      public void EveryCard_IsOnATab_InTheOrderOfTheSubject()
      {
         string section = SecuritySection_();

         int cards = Regex.Matches(section, @"cards_\.Add\(new CardDef").Count;
         List<string> tabs = Regex.Matches(section, @"\n\s*Tab = L\(""([^""]+)""\),").Select(m => m.Groups[1].Value).ToList();
         Assert.Equal(10, cards);
         Assert.Equal(cards, tabs.Count);
         Assert.Equal(new[] { "Encryption", "Signing", "Forwarding", "Reports" }, tabs.Distinct().ToArray());

         // Each tab's cards together, so no tab is drawn twice.
         for (int i = 1; i < tabs.Count; i++)
         {
            if (tabs[i] != tabs[i - 1])
               Assert.DoesNotContain(tabs[i], tabs.Take(i));
         }

         foreach ((string title, string tab) in new[]
         {
            ("DANE & DNSSEC", "Encryption"), ("MTA-STS", "Encryption"),
            ("ARC sealing", "Signing"), ("ARC inbound filtering", "Signing"), ("DKIM signature timestamps", "Signing"),
            ("DKIM oversigning", "Signing"), ("Authentication results on inbound mail", "Signing"),
            ("Forwarded mail & bounce protection (SRS / BATV)", "Forwarding"),
            ("TLS reporting (TLS-RPT)", "Reports"), ("DMARC aggregate reporting (rua)", "Reports")
         })
            Assert.Contains("Title = L(\"" + title + "\"),\n                  Tab = L(\"" + tab + "\"),", section.Replace("\r\n", "\n"), StringComparison.Ordinal);
      }

      /// <summary>
      /// A section whose cards name tabs is framed as ServerSettingsView's pages are:
      /// the header and the notice above a tab strip, each tab scrolling under it.
      /// </summary>
      [Fact]
      public void ATabbedSection_KeepsItsHeaderAboveTheTabs()
      {
         string code = Source_();

         string constructor = WithheldRowTests.MemberText(code, "public FeatureSettingsView(Section section)");
         Assert.True(constructor != null, "FeatureSettingsView's constructor is gone.");
         Assert.Contains("if (cards_.Any(card => card.Tab != null))\n            FrameTabs_();", constructor.Replace("\r\n", "\n"), StringComparison.Ordinal);

         string frame = WithheldRowTests.MemberText(code, "private void FrameTabs_()");
         Assert.True(frame != null, "FeatureSettingsView.FrameTabs_ is gone.");
         Assert.Contains("frame.Children.Add(Header);", frame, StringComparison.Ordinal);
         Assert.Contains("frame.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });", frame, StringComparison.Ordinal);
         Assert.Contains("Content = frame;", frame, StringComparison.Ordinal);

         string panel = WithheldRowTests.MemberText(code, "private Panel PanelFor_(CardDef card)");
         Assert.True(panel != null, "FeatureSettingsView.PanelFor_ is gone.");
         Assert.Contains("new ScrollViewer { VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Content = panel }", panel, StringComparison.Ordinal);
         Assert.Contains("MaxWidth = DesignTokens.FormWidth", panel, StringComparison.Ordinal);

         string build = WithheldRowTests.MemberText(code, "private void BuildUi()");
         Assert.True(build != null, "FeatureSettingsView.BuildUi is gone.");
         Assert.Contains("PanelFor_(card).Children.Add(surface);", build, StringComparison.Ordinal);
         Assert.DoesNotContain("CardsPanel.Children.Add(surface);", build, StringComparison.Ordinal);
         // A Reload or a return to the page stays on the tab it was on.
         Assert.Contains("int shownTab = ClearTabs_();", build, StringComparison.Ordinal);
         Assert.Contains("ShowTab_(shownTab);", build, StringComparison.Ordinal);
      }

      /// <summary>The subtitle and the page's description say what all four tabs hold.</summary>
      [Fact]
      public void TheSubtitle_DescribesTheWholePage()
      {
         string purpose = NavigationMap.Find("security").Purpose;
         Assert.Contains("Header.Subtitle = L(\"" + purpose + "\");", SecuritySection_(), StringComparison.Ordinal);
         Assert.DoesNotContain("Outbound", purpose, StringComparison.Ordinal);
         // A subject for each tab, in the tabs' order.
         int at = -1;
         foreach (string subject in new[] { "Encryption", "signatures and results", "forwarded mail", "reports" })
         {
            int next = purpose.IndexOf(subject, StringComparison.Ordinal);
            Assert.True(next > at, "The description does not name '" + subject + "' after what comes before it: " + purpose);
            at = next;
         }

         // The acronyms are the aliases, which the palette searches.
         foreach (string alias in new[] { "SRS", "BATV", "DKIM oversigning", "Authentication-Results", "DMARC reports" })
            Assert.Contains(alias, NavigationMap.Find("security").Aliases);
      }

      /// <summary>
      /// The ARC card's pointer is a button that opens Anti-spam settings on the tab
      /// that holds the trusted sealers - which it does - and the hardening page's
      /// signpost to SRS and BATV opens the Forwarding tab.
      /// </summary>
      [Fact]
      public void ThePointers_OpenTheTabsTheyName()
      {
         Assert.Contains("new ElsewhereSetting(\"antispam\", L(\"ARC inbound filtering and its trusted sealers\"), N(\"Sender auth\"))", SecuritySection_(), StringComparison.Ordinal);
         Assert.Contains("new ElsewhereSetting(\"security\", L(\"SRS, BATV and the plain envelope-sender rewrite for forwarded mail\"), N(\"Forwarding\"))", Source_(), StringComparison.Ordinal);

         string antispam = WithheldRowTests.MemberText(ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs")), "private void BuildAntiSpam()");
         Assert.True(antispam != null, "ServerSettingsView.BuildAntiSpam is gone.");
         int sealers = antispam.IndexOf("Path = \"AntiSpam.ArcTrustedSealers\"", StringComparison.Ordinal);
         int tab = antispam.IndexOf("Tab(L(\"Sender auth\")).Cards.Add(auth);", StringComparison.Ordinal);
         Assert.True(sealers >= 0 && tab > sealers, "The ARC trusted sealers are no longer on Anti-spam settings' Sender auth tab.");

         // The row opens the page on its tab, and names the tab after the page.
         string code = Source_();
         Assert.Contains("?.NavigateToTab(page_, tab_);", code, StringComparison.Ordinal);
         Assert.Contains("destination += \" > \" + L(tab_);", code, StringComparison.Ordinal);

         string open = ReadControlPanelFile("MainWindow.OpenAtTab.cs");
         string navigate = WithheldRowTests.MemberText(open, "public void NavigateToTab(string key, string tab)");
         Assert.True(navigate != null, "MainWindow.NavigateToTab is gone.");
         Assert.Contains("NavigateTo(key);", navigate, StringComparison.Ordinal);
         Assert.Contains("DispatcherPriority.Loaded", navigate, StringComparison.Ordinal);
         Assert.Contains("string.Equals(currentPage_, key, StringComparison.OrdinalIgnoreCase)", navigate, StringComparison.Ordinal);
         Assert.Contains("TabJump.Show(ContentHost.Content as DependencyObject, L(tab));", navigate, StringComparison.Ordinal);
      }

      /// <summary>What the cards hold is found by its name.</summary>
      [Theory]
      [InlineData("SRS")]
      [InlineData("Sender Rewriting Scheme")]
      [InlineData("BATV")]
      [InlineData("DKIM oversigning")]
      [InlineData("Authentication-Results")]
      [InlineData("DMARC reports")]
      [InlineData("Transport security")]
      public void TheSubjectsOfItsCards_FindThePage(string query)
      {
         IReadOnlyList<PaletteRow> rows = PaletteSearch.Query(query, null, null);
         int first = PaletteSearch.FirstSelectable(rows);
         Assert.True(first >= 0, "'" + query + "' matched nothing at all.");
         Assert.Equal("security", rows[first].Page);
      }

      private static string Source_() => ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml.cs"));

      private static string SecuritySection_()
      {
         string code = Source_();
         int start = code.IndexOf("case Section.Security:", StringComparison.Ordinal);
         Assert.True(start >= 0, "FeatureSettingsView has no Security section.");
         int end = code.IndexOf("case Section.Automation:", start, StringComparison.Ordinal);
         Assert.True(end > start, "The Security section's end was not found.");
         return code.Substring(start, end - start);
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
