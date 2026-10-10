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

      /// <summary>
      /// Only the header and the strip are pinned (review finding F290, which the tabs
      /// brought to this page): the notice and the line that names the store go into
      /// the tab on screen, above its first card - on the strip's own SelectionChanged,
      /// not a list's, and back under the header before the tabs are cleared - and
      /// under 600 pixels of page the subtitle is the title's tool tip. At 760 by 520
      /// the pinned lines left the cards some 140 pixels, and 40 with the notice of a
      /// page read from the file.
      /// </summary>
      [Fact]
      public void ATabbedSection_PinsOnlyItsHeaderAndStrip()
      {
         string code = Source_();

         string frame = WithheldRowTests.MemberText(code, "private void FrameTabs_()");
         Assert.True(frame != null, "FeatureSettingsView.FrameTabs_ is gone.");
         Assert.Contains("tabHeadHome_ = new ContentControl { Focusable = false, IsTabStop = false, Content = inner };", frame, StringComparison.Ordinal);
         Assert.Contains("Grid.SetRow(tabHeadHome_, 1);", frame, StringComparison.Ordinal);
         Assert.Contains("if (ReferenceEquals(e.OriginalSource, cardTabs_))\n               PlaceTabHead_();", frame.Replace("\r\n", "\n"), StringComparison.Ordinal);
         Assert.Contains("SizeChanged += (s, e) => FitTabbedHeader_(e.NewSize.Height);", frame, StringComparison.Ordinal);

         string panel = WithheldRowTests.MemberText(code, "private Panel PanelFor_(CardDef card)");
         Assert.True(panel != null, "FeatureSettingsView.PanelFor_ is gone.");
         int slot = panel.IndexOf("panel.Children.Add(headSlot);", StringComparison.Ordinal);
         Assert.True(slot >= 0 && panel.IndexOf("tabHeadSlots_.Add(headSlot);", StringComparison.Ordinal) > slot, "A tab has no place for the notice above its first card.");

         string place = WithheldRowTests.MemberText(code, "private void PlaceTabHead_()");
         Assert.True(place != null, "FeatureSettingsView.PlaceTabHead_ is gone.");
         Assert.Contains("ContentControl target = at >= 0 && at < tabHeadSlots_.Count ? tabHeadSlots_[at] : tabHeadHome_;", place, StringComparison.Ordinal);

         string clear = WithheldRowTests.MemberText(code, "private int ClearTabs_()");
         Assert.True(clear != null, "FeatureSettingsView.ClearTabs_ is gone.");
         int back = clear.IndexOf("PlaceTabHead_();", StringComparison.Ordinal);
         Assert.True(back >= 0 && clear.IndexOf("cardTabs_.Items.Clear();", StringComparison.Ordinal) > back, "The notice is not taken out of the tabs before they are cleared.");
         Assert.Contains("PlaceTabHead_();", WithheldRowTests.MemberText(code, "private void ShowTab_(int shown)"), StringComparison.Ordinal);

         string fit = WithheldRowTests.MemberText(code, "private void FitTabbedHeader_(double pageHeight)");
         Assert.True(fit != null, "FeatureSettingsView.FitTabbedHeader_ is gone.");
         Assert.Contains("if (pageHeight > 0 && pageHeight < TabbedSubtitleToolTipBelow)", fit, StringComparison.Ordinal);
         Assert.Contains("subtitle.Visibility = Visibility.Collapsed;", fit, StringComparison.Ordinal);
         Assert.Contains("titleRow.ToolTip = Header.Subtitle;", fit, StringComparison.Ordinal);
         Assert.Contains("subtitle.ClearValue(VisibilityProperty);", fit, StringComparison.Ordinal);
         Assert.Contains("internal const double TabbedSubtitleToolTipBelow = 600;", code, StringComparison.Ordinal);
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
         string shortForm = WithheldRowTests.MemberText(open, "public void NavigateToTab(string key, string tab)");
         Assert.True(shortForm != null, "MainWindow.NavigateToTab is gone.");
         Assert.Contains("NavigateToTab(key, tab, false);", shortForm, StringComparison.Ordinal);
         string navigate = WithheldRowTests.MemberText(open, "private void NavigateToTab(string key, string tab, bool fromSearch)");
         Assert.True(navigate != null, "MainWindow.NavigateToTab(key, tab, fromSearch) is gone.");
         Assert.Contains("NavigateTo(key, fromSearch);", navigate, StringComparison.Ordinal);
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

      /// <summary>
      /// The setup checklist's items open Transport security on the tab whose cards
      /// hold the setting they check - ARC sealing on Signing, TLS reporting on
      /// Reports - where after the split every one of them opened it on Encryption
      /// (review finding F228, the shape of F274). Every item that names a tab names
      /// one its page has: Protocols' SMTP for the host name, Administrative access'
      /// Two-factor, Anti-spam settings' Sender auth and Anti-virus settings' Sandbox,
      /// the tabs their texts name.
      /// </summary>
      [Fact]
      public void TheChecklistsItems_OpenThePageOnTheTabOfTheirSettings()
      {
         Dictionary<string, string> keyTabs = SecuritySettingTabs_();
         string checks = ReadControlPanelFile(Path.Join("Services", "ExternalSetupChecks.cs"));

         var named = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (Match page in Regex.Matches(checks, @"\bPage = ""security"""))
         {
            int start = checks.LastIndexOf("private SetupItem ", page.Index, StringComparison.Ordinal);
            int end = checks.IndexOf("\n      private ", page.Index, StringComparison.Ordinal);
            Assert.True(start >= 0 && end > page.Index, "A setup item for Transport security is not in a method of its own.");
            string method = checks.Substring(start, end - start);
            string name = Regex.Match(method, @"private SetupItem (\w+)\(").Groups[1].Value;

            Match tab = Regex.Match(method, @"\bTab = N\(""([^""]+)""\),");
            Assert.True(tab.Success, name + " opens Transport security without naming the tab its setting is on.");
            Assert.Contains(tab.Groups[1].Value, TabsOf_("security"));
            foreach (Match read in Regex.Matches(method, @"\bini\.Read(?:Bool|Int)?\(""(\w+)"""))
            {
               if (keyTabs.TryGetValue(read.Groups[1].Value, out string holder))
                  Assert.True(holder == tab.Groups[1].Value, name + " checks " + read.Groups[1].Value + ", which is on the " + holder + " tab, and opens " + tab.Groups[1].Value + ".");
            }

            named[name] = tab.Groups[1].Value;
         }

         Assert.Equal("Signing", named["ArcSealingItem"]);
         Assert.Equal("Reports", named["TlsRptItem"]);
         Assert.Equal("Encryption", named["DaneItem"]);

         // Every tab an item names is one of its page's.
         foreach (Match item in Regex.Matches(checks, @"\bTab = N\(""(?<tab>[^""]+)""\),(?:(?!\bPage = "")[\s\S])*\bPage = ""(?<page>\w+)"""))
            Assert.True(TabsOf_(item.Groups["page"].Value).Contains(item.Groups["tab"].Value),
               "A setup item opens " + item.Groups["page"].Value + " on " + item.Groups["tab"].Value + ", a tab it does not have.");
         foreach ((string page, string tab) in new[] { ("protocols", "SMTP"), ("adminaccess", "Two-factor"), ("antispam", "Sender auth"), ("antivirus", "Sandbox") })
            Assert.Matches(new Regex(@"\bTab = N\(""" + Regex.Escape(tab) + @"""\),(?:(?!\bPage = "")[\s\S])*\bPage = """ + page + @""""), checks);

         // The checklist's button and the Dashboard's link open the item's tab.
         string view = ReadControlPanelFile(Path.Join("Views", "ExternalSetupView.cs"));
         Assert.Contains("PageLink(item.Page, item.Tab, ", view, StringComparison.Ordinal);
         Assert.Contains("?.NavigateToTab(page, tab);", WithheldRowTests.MemberText(view, "private static FrameworkElement PageLink(string page, string tab, string caption, string accessibleName, bool primary)"), StringComparison.Ordinal);
         Assert.Contains("?.NavigateToTab(page, item.Tab);", ReadControlPanelFile(Path.Join("Views", "DashboardView.xaml.cs")), StringComparison.Ordinal);
      }

      /// <summary>
      /// The palette's tasks for SRS and BATV open the Forwarding tab, and every task
      /// for the page names the tab it is done on; a task's tab is one its page has.
      /// </summary>
      [Fact]
      public void ThePalettesTasks_OpenThePageOnTheirTab()
      {
         foreach (IntentEntry intent in IntentIndex.Entries.Where(intent => intent.Page == "security"))
            Assert.True(intent.Tab != null, "The task '" + intent.Phrase + "' opens Transport security without naming its tab.");
         foreach (IntentEntry intent in IntentIndex.Entries.Where(intent => intent.Tab != null))
            Assert.True(TabsOf_(intent.Page).Contains(intent.Tab), "The task '" + intent.Phrase + "' opens " + intent.Page + " on " + intent.Tab + ", a tab it does not have.");

         foreach ((string query, string tab) in new[]
         {
            ("stop forwarded mail failing spf", "Forwarding"), ("reject forged bounce messages", "Forwarding"),
            ("prove to other servers that tls is required", "Encryption")
         })
         {
            IReadOnlyList<PaletteRow> rows = PaletteSearch.Query(query, null, null);
            PaletteRow row = rows[PaletteSearch.FirstSelectable(rows)];
            Assert.Equal(PaletteRowKind.Task, row.Kind);
            Assert.Equal("security", row.Page);
            Assert.Equal(tab, row.Tab);
         }

         // The palette hands the tab on, and the window opens the page on it.
         string palette = ReadControlPanelFile(Path.Join("Views", "NavigationPalette.cs"));
         Assert.Contains("SelectedTab = row.Kind == PaletteRowKind.Shortcuts || row.Tour != null ? null : row.Tab;", palette, StringComparison.Ordinal);
         string show = WithheldRowTests.MemberText(ReadControlPanelFile("MainWindow.xaml.cs"), "private void ShowPalette()");
         Assert.True(show != null, "MainWindow.ShowPalette is gone.");
         Assert.Contains("NavigateToTab(palette.SelectedPage, palette.SelectedTab, true);", show, StringComparison.Ordinal);
      }

      /// <summary>
      /// Each of the page's aliases names one of its tabs, and a search that finds the
      /// page by an alias opens it on that tab - SRS and BATV on Forwarding, DKIM
      /// oversigning, Authentication-Results and ARC on Signing, DMARC reports and
      /// TLS-RPT on Reports - while its title, and a query whose best aliases are on
      /// two tabs, open it as it is.
      /// </summary>
      [Fact]
      public void ThePagesAliases_OpenItOnTheirTab()
      {
         NavNode security = NavigationMap.Find("security");
         Assert.True(NavigationMap.AliasTabs.TryGetValue("security", out IReadOnlyDictionary<string, string> aliasTabs), "Transport security's aliases name no tabs.");
         foreach (string alias in security.Aliases)
            Assert.True(aliasTabs.ContainsKey(alias), "Transport security's alias '" + alias + "' names no tab.");
         foreach (KeyValuePair<string, string> entry in aliasTabs)
         {
            Assert.Contains(entry.Key, security.Aliases);
            Assert.Contains(entry.Value, TabsOf_("security"));
         }

         foreach (KeyValuePair<string, IReadOnlyDictionary<string, string>> page in NavigationMap.AliasTabs)
            foreach (KeyValuePair<string, string> entry in page.Value)
               Assert.True(TabsOf_(page.Key).Contains(entry.Value), page.Key + "'s alias '" + entry.Key + "' names " + entry.Value + ", a tab it does not have.");

         foreach ((string query, string tab) in new[]
         {
            ("SRS", "Forwarding"), ("Sender Rewriting Scheme", "Forwarding"), ("BATV", "Forwarding"),
            ("DKIM oversigning", "Signing"), ("Authentication-Results", "Signing"), ("ARC", "Signing"),
            ("DMARC reports", "Reports"), ("TLS-RPT", "Reports"), ("MTA-STS", "Encryption"),
            ("Transport security", null), ("TLS", null)
         })
         {
            PaletteRow row = PaletteSearch.Query(query, null, null).FirstOrDefault(r => r.Kind == PaletteRowKind.Page && r.Page == "security");
            Assert.True(row != null, "'" + query + "' does not find Transport security.");
            Assert.True(tab == row.Tab, "'" + query + "' opens Transport security on " + (row.Tab ?? "the tab it is on") + ", where it should open " + (tab ?? "the tab it is on") + ".");
         }
      }

      /// <summary>Each setting key of the Security section, and the tab its card is on.</summary>
      private static Dictionary<string, string> SecuritySettingTabs_()
      {
         var tabs = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (string card in SecuritySection_().Split("cards_.Add(new CardDef").Skip(1))
         {
            Match tab = Regex.Match(card, @"\bTab = L\(""([^""]+)""\),");
            Assert.True(tab.Success, "A Transport security card names no tab.");
            foreach (Match key in Regex.Matches(card, @"\bKey = ""(\w+)"""))
               tabs[key.Groups[1].Value] = tab.Groups[1].Value;
         }

         return tabs;
      }

      /// <summary>
      /// The English headers of the tabs of the page with navigation key
      /// <paramref name="page"/>: a ServerSettingsView page's Tab(L(...)) calls, a
      /// FeatureSettingsView section's cards' Tab = L(...), from the section the
      /// window's page factory names.
      /// </summary>
      private static HashSet<string> TabsOf_(string page)
      {
         Match factory = Regex.Match(ReadControlPanelFile("MainWindow.xaml.cs"),
            @"pageFactories_\[""" + Regex.Escape(page) + @"""\] = \(\) => new (?<view>ServerSettingsView|FeatureSettingsView)\(\k<view>\.Section\.(?<section>\w+)\);");
         Assert.True(factory.Success, "The page " + page + " is not a page of settings.");
         string section = factory.Groups["section"].Value;

         var tabs = new HashSet<string>(StringComparer.Ordinal);
         if (factory.Groups["view"].Value == "ServerSettingsView")
         {
            string code = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
            Match build = Regex.Match(code, @"case Section\." + section + @": (Build\w+)\(\); break;");
            Assert.True(build.Success, "ServerSettingsView builds no section " + section + ".");
            string body = WithheldRowTests.MemberText(code, "private void " + build.Groups[1].Value + "()");
            foreach (Match tab in Regex.Matches(body ?? "", @"\bTab\(L\(""([^""]+)""\)\)"))
               tabs.Add(tab.Groups[1].Value);
         }
         else
         {
            string code = Source_();
            int start = code.IndexOf("case Section." + section + ":", StringComparison.Ordinal);
            int end = code.IndexOf("case Section.", start + 1, StringComparison.Ordinal);
            string body = start < 0 ? "" : code.Substring(start, (end < 0 ? code.Length : end) - start);
            foreach (Match tab in Regex.Matches(body, @"\bTab = L\(""([^""]+)""\),"))
               tabs.Add(tab.Groups[1].Value);
         }

         return tabs;
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
