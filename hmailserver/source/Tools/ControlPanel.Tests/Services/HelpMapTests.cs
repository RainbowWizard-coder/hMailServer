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
   /// The help map holds every page of the navigation to a document, and every
   /// entry to a document and a heading that exist. build/check-help-map.py
   /// holds the same table, and the Deck's and the webmail's, to the documents
   /// from outside; this holds it from inside, where a page added to
   /// NavigationMap without an entry here fails a build of the tests rather
   /// than opening a pane on nothing.
   /// </summary>
   public class HelpMapTests
   {
      [Fact]
      public void EveryNavigationPageHasAnEntryAndNothingElseDoes()
      {
         var pages = NavigationMap.Pages.Select(p => p.Key).ToHashSet(StringComparer.OrdinalIgnoreCase);
         var mapped = HelpMap.Pages.Keys.ToHashSet(StringComparer.OrdinalIgnoreCase);

         Assert.Empty(pages.Except(mapped, StringComparer.OrdinalIgnoreCase));
         Assert.Empty(mapped.Except(pages, StringComparer.OrdinalIgnoreCase));
      }

      /// <summary>Three lookups: a document alone, a document with a heading, and the dashboard's.</summary>
      [Theory]
      [InlineData("backup", "Backup.md", null)]
      [InlineData("rules", "MailFlowRules.md", null)]
      [InlineData("dashboard", "SetupChecklist.md", "Where it is")]
      public void ThePageIsMappedToItsDocument(string key, string doc, string heading)
      {
         HelpTarget target = HelpMap.For(key);

         Assert.NotNull(target);
         Assert.Equal(doc, target.Doc);
         Assert.Equal(heading, target.Heading);
         Assert.Equal(doc.Substring(0, doc.Length - 3), target.Name);
      }

      /// <summary>
      /// The pages review finding F270 (with F015 and F078) found opening a
      /// document about something else open one about themselves, or say that
      /// none covers them yet - never the unrelated one.
      /// </summary>
      [Theory]
      [InlineData("domains", "DomainsAndAccounts.md", null)]
      [InlineData("publicfolders", "PublicFoldersAndGroups.md", null)]
      [InlineData("groups", "PublicFoldersAndGroups.md", "Groups")]
      [InlineData("certs", "SecureDefaults.md", "The certificate")]
      [InlineData("acme", "SecureDefaults.md", "The certificate")]
      [InlineData("tlsoverview", "SecureDefaults.md", "What a new installation starts with")]
      [InlineData("greylistwhitelist", "Greylisting.md", "What greylisting does")]
      [InlineData("hardening", "DiagnosingStalledMail.md", "Settings that bound each stage")]
      [InlineData("adminaccess", "SecretsAtRest.md", "The administrator password")]
      [InlineData("externalsetup", "SetupChecklist.md", null)]
      // Its front-end proxies' reasons since review finding F273 moved the card there.
      [InlineData("relays", "SettingsReasons.md", "Incoming relays")]
      public void APageTheReviewFoundMismatched_OpensADocumentAboutIt(string key, string doc, string heading)
      {
         HelpTarget target = HelpMap.For(key);
         Assert.True(target.IsGuide, key + " has no guide.");
         Assert.Equal(doc, target.Doc);
         Assert.Equal(heading, target.Heading);
      }

      [Theory]
      [InlineData("sendout")]
      [InlineData("dnsbl")]
      [InlineData("surbl")]
      [InlineData("spamoverview")]
      [InlineData("ipranges")]
      [InlineData("virusoverview")]
      [InlineData("messagetrace")]
      public void APageNoDocumentIsAbout_HasNoGuide(string key)
      {
         HelpTarget target = HelpMap.For(key);
         Assert.NotNull(target);
         Assert.False(target.IsGuide);
         Assert.Null(target.Doc);
         Assert.Null(target.Name);
      }

      /// <summary>The documents the review found opened from the wrong pages are no unrelated page's help.</summary>
      [Theory]
      [InlineData("Backup.md", "backup")]
      [InlineData("DomainTransforms.md", "")]
      [InlineData("CountryBlocking.md", "")]
      [InlineData("Directory.md", "")]
      [InlineData("Calendar.md", "")]
      [InlineData("DistributionLists.md", "")]
      [InlineData("ReputationWatch.md", "")]
      [InlineData("SealedMail.md", "")]
      [InlineData("ServiceAccount.md", "")]
      [InlineData("FollowConnection.md", "")]
      public void ADocumentAboutSomethingElse_IsNoLongerAnUnrelatedPagesHelp(string doc, string pages)
      {
         string[] expected = pages.Length == 0 ? Array.Empty<string>() : pages.Split(',');
         string[] mapped = HelpMap.Pages.Where(p => p.Value.Doc == doc).Select(p => p.Key).OrderBy(k => k, StringComparer.Ordinal).ToArray();
         Assert.Equal(expected, mapped);
      }

      /// <summary>A tab with a document of its own opens it; the page's other tabs keep the page's.</summary>
      [Theory]
      [InlineData("api", "SNMP", "Snmp.md")]
      [InlineData("api", "Alerts & webhooks", "AlertsAndAuditTrail.md")]
      [InlineData("api", "Shutdown", "HighAvailabilityRunbook.md")]
      [InlineData("protocols", "ManageSieve", "SettingsReasons.md")]
      [InlineData("protocols", "SMTP", "Protocols.md")]
      [InlineData("logging", "Audit trail", "AlertsAndAuditTrail.md")]
      [InlineData("logging", "Logging", "Syslog.md")]
      [InlineData("logging", null, "Syslog.md")]
      [InlineData("antispam", "Greylisting", "Greylisting.md")]
      [InlineData("antispam", "Spam traps", "SpamTraps.md")]
      [InlineData("antivirus", "Sandbox", "Sandbox.md")]
      [InlineData("autoban", "Countries", "CountryBlocking.md")]
      [InlineData("autoban", "Compromised accounts", "CompromisedAccounts.md")]
      [InlineData("autoban", "Auto-ban", "AutoBan.md")]
      [InlineData("performance", "Message store", "MessageStore.md")]
      [InlineData("webmail", "Encryption", "OpenPGP.md")]
      [InlineData("webmail", "Sign-in", "PasskeysAndSingleSignOn.md")]
      public void ATab_OpensItsOwnDocumentOrThePages(string key, string tab, string doc)
      {
         Assert.Equal(doc, HelpMap.For(key, tab).Doc);
      }

      [Fact]
      public void ATabNoDocumentIsAbout_HasNoGuide_AndTabsOfAPageAreListed()
      {
         Assert.False(HelpMap.For("webmail", "Notifications").IsGuide);
         Assert.False(HelpMap.For("adminaccess", "Two-factor").IsGuide);
         Assert.Null(HelpMap.For("no-such-page", "SNMP"));
         Assert.Contains("SNMP", HelpMap.TabsOf("api"));
         Assert.DoesNotContain("SNMP", HelpMap.TabsOf("logging"));
         Assert.Equal("Protocols", HelpMap.For("protocols", "ManageSieve").Heading);
         Assert.Empty(HelpMap.TabsOf("domains"));
         Assert.Empty(HelpMap.TabsOf(null));
      }

      /// <summary>
      /// Every tab entry names a tab its page builds: a settings page of
      /// ServerSettingsView whose section's builder makes a tab of that English
      /// header (Tab(L("SNMP"))), as build/check-help-map.py reads them.
      /// </summary>
      [Fact]
      public void EveryTabEntry_NamesATabItsPageBuilds()
      {
         string controlPanel = FindControlPanel();
         string shell = File.ReadAllText(Path.Combine(controlPanel, "MainWindow.xaml.cs"));
         string view = File.ReadAllText(Path.Combine(controlPanel, "Views", "ServerSettingsView.xaml.cs"));

         var sections = Regex.Matches(shell, @"pageFactories_\[""(\w+)""\]\s*=\s*\(\)\s*=>\s*new ServerSettingsView\(ServerSettingsView\.Section\.(\w+)\)")
            .ToDictionary(m => m.Groups[1].Value, m => m.Groups[2].Value, StringComparer.OrdinalIgnoreCase);
         var builders = Regex.Matches(view, @"case Section\.(\w+):\s*(Build\w+)\(\);")
            .ToDictionary(m => m.Groups[1].Value, m => m.Groups[2].Value);

         var broken = new List<string>();
         foreach (string entry in HelpMap.Tabs.Keys)
         {
            string page = entry.Substring(0, entry.IndexOf('#'));
            string tab = entry.Substring(entry.IndexOf('#') + 1);
            if (NavigationMap.Find(page) == null || !sections.TryGetValue(page, out string section) || !builders.TryGetValue(section, out string builder))
            {
               broken.Add(entry + " (no settings page)");
               continue;
            }

            int start = view.IndexOf("private void " + builder + "()", StringComparison.Ordinal);
            int end = view.IndexOf("private void Build", start + 1, StringComparison.Ordinal);
            string body = view.Substring(start, (end < 0 ? view.Length : end) - start);
            if (!body.Contains("Tab(L(\"" + tab + "\"))", StringComparison.Ordinal))
               broken.Add(entry + " (" + builder + " builds no such tab)");
         }

         Assert.True(broken.Count == 0, "Tab entries for tabs that are not built: " + string.Join(", ", broken));
      }

      /// <summary>
      /// The pane shows a page with no guide as its title, its purpose and a
      /// sentence saying so, over every document; and the window tells it the
      /// tab as well as the page, again as the page's tab strip changes tab.
      /// </summary>
      [Fact]
      public void ThePaneSaysWhenThereIsNoGuide_AndFollowsTheTab()
      {
         string controlPanel = FindControlPanel();
         // The pane is two files, its list and its Back in HelpPane.List.cs.
         string pane = File.ReadAllText(Path.Combine(controlPanel, "Views", "HelpPane.cs"))
            + File.ReadAllText(Path.Combine(controlPanel, "Views", "HelpPane.List.cs"));
         Assert.Contains("public void ShowPage(string navigationKey, string tab)", pane, StringComparison.Ordinal);
         Assert.Contains("HelpMap.For(navigationKey, tab)", pane, StringComparison.Ordinal);
         Assert.Contains("if (!target.IsGuide)", pane, StringComparison.Ordinal);
         Assert.Contains("L(\"No guide covers this page yet. Every document is listed below.\")", pane, StringComparison.Ordinal);
         Assert.Contains("L(page.Purpose)", pane, StringComparison.Ordinal);

         string tabs = File.ReadAllText(Path.Combine(controlPanel, "MainWindow.HelpTabs.cs"));
         Assert.Contains("Selector.SelectionChangedEvent", tabs, StringComparison.Ordinal);
         Assert.Contains("e.OriginalSource is TabControl", tabs, StringComparison.Ordinal);
         Assert.Contains("HelpPanel.ShowPage(currentPage_, HelpTabOf_(currentPage_));", tabs, StringComparison.Ordinal);

         string shell = File.ReadAllText(Path.Combine(controlPanel, "MainWindow.xaml.cs"));
         Assert.Contains("WireHelpTabs_();", shell, StringComparison.Ordinal);
         Assert.Contains("ShowHelpForNewPage_(key);", shell, StringComparison.Ordinal);
         Assert.DoesNotContain("HelpPanel.ShowPage(key)", shell, StringComparison.Ordinal);
      }

      /// <summary>
      /// A document the pane is given starts at its top, and moves to the page's
      /// heading once it has been laid out. The viewer's ScrollViewer keeps its
      /// offset when the document is replaced, and a block's BringIntoView asked
      /// before the new document is laid out does nothing, so the pane reset to the
      /// top by asking it of the first block, and reached the heading straight after
      /// the document was given, both in vain: a page with no guide opened at the
      /// foot of the list of documents after a long document's heading, a tab's
      /// document part-way down. Every document now goes through ShowFromTheTop_,
      /// which sets the ScrollViewer itself back, and at Loaded priority, after the
      /// layout, the heading's line is put at the top of the pane by the
      /// ScrollViewer's offset - BringIntoView scrolls as little as shows the block,
      /// which leaves a heading already on screen where it is.
      /// </summary>
      [Fact]
      public void ADocumentOpensAtItsTop_AndAtItsHeadingOnceLaidOut()
      {
         string pane = File.ReadAllText(Path.Combine(FindControlPanel(), "Views", "HelpPane.cs"));

         Assert.Single(Regex.Matches(pane, @"viewer_\.Document\s*=(?!=)"));
         string top = WithheldRowTests.MemberText(pane, "private void ShowFromTheTop_(FlowDocument document)");
         Assert.True(top != null, "HelpPane no longer gives the viewer its documents through ShowFromTheTop_.");
         Assert.Contains("viewer_.Document = document;", top, StringComparison.Ordinal);
         Assert.Contains("DocumentScroller_()?.ScrollToHome();", top, StringComparison.Ordinal);
         Assert.Contains("ShowFromTheTop_(DocumentList_(intro));", pane, StringComparison.Ordinal);
         Assert.Contains("ShowFromTheTop_(built.Document);", pane, StringComparison.Ordinal);
         Assert.Contains("ShowFromTheTop_(DocumentList_(null));", pane, StringComparison.Ordinal);

         string scroller = WithheldRowTests.MemberText(pane, "private ScrollViewer DocumentScroller_()");
         Assert.True(scroller != null && scroller.Contains("\"PART_ContentHost\"", StringComparison.Ordinal),
            "HelpPane no longer finds the viewer's own ScrollViewer.");

         string heading = WithheldRowTests.MemberText(pane, "private void ScrollToHeading_(string heading)");
         Assert.True(heading != null, "HelpPane no longer scrolls to a heading through ScrollToHeading_.");
         Assert.Contains("DispatcherPriority.Loaded", heading, StringComparison.Ordinal);
         Assert.Contains("block.ContentStart.GetCharacterRect(LogicalDirection.Forward)", heading, StringComparison.Ordinal);
         Assert.Contains("scroller.ScrollToVerticalOffset(scroller.VerticalOffset + line.Top);", heading, StringComparison.Ordinal);
         Assert.Contains("ReferenceEquals(viewer_.Document, document)", heading, StringComparison.Ordinal);
         Assert.DoesNotContain("FirstBlock", pane, StringComparison.Ordinal);
      }

      [Fact]
      public void AKeyThatIsNotAPageHasNoHelp()
      {
         Assert.Null(HelpMap.For("no-such-page"));
         Assert.Null(HelpMap.For(null));
      }

      [Fact]
      public void EveryEntryNamesADocumentThatExistsAndAHeadingItHas()
      {
         string docs = FindDocs();
         foreach (KeyValuePair<string, HelpTarget> entry in HelpMap.Pages.Concat(HelpMap.Tabs))
         {
            if (!entry.Value.IsGuide)
               continue;

            string path = Path.Combine(docs, entry.Value.Doc);
            Assert.True(File.Exists(path), entry.Key + " names " + entry.Value.Doc + ", which is not in " + docs);

            if (entry.Value.Heading == null)
               continue;

            IReadOnlyList<MdHeading> headings = MarkdownBlocks.Headings(File.ReadAllText(path));
            Assert.Contains(headings, h => h.Text == entry.Value.Heading);
         }
      }

      /// <summary>
      /// The help of a card that review finding F273 moved is reached from where it is
      /// now. The Migrations card is on the backup page, whose help is Backup.md and
      /// which has no tabs to open another document on - its first tab strip is the
      /// card's own, which an entry for it would take the whole page with - so Backup.md
      /// sends the reader to Migration.md, which Advanced's Migrations tab opened. The
      /// Shutdown tab of Monitoring &amp; alerts opens the runbook that says what the drain
      /// on a stop is for, where it opened a section of the reasons document that says
      /// nothing of it; and ManageSieve's tab of Protocols the reasons document's
      /// Protocols section, which holds its reason.
      /// </summary>
      [Fact]
      public void TheMovedCards_KeepTheirGuides()
      {
         string docs = FindDocs();
         Assert.DoesNotContain("Migrations", HelpMap.TabsOf("advanced"));
         Assert.Equal("Backup.md", HelpMap.For("backup").Doc);
         Assert.Empty(HelpMap.TabsOf("backup"));
         string backup = File.ReadAllText(Path.Combine(docs, "Backup.md"));
         Assert.Contains("[Migration.md](Migration.md)", backup, StringComparison.Ordinal);
         Assert.Contains("(Migration.md#outlook-data-files-pst)", backup, StringComparison.Ordinal);

         Assert.Contains("ShutdownDrainSeconds", Section_(Path.Combine(docs, "HighAvailabilityRunbook.md"), HelpMap.For("api", "Shutdown").Heading), StringComparison.Ordinal);
         Assert.Contains("ManageSieve", Section_(Path.Combine(docs, "SettingsReasons.md"), HelpMap.For("protocols", "ManageSieve").Heading), StringComparison.Ordinal);
         Assert.DoesNotContain("HighAvailabilityRunbook.md", HelpMap.ReferenceOnly);
      }

      /// <summary>The text of a document from the heading named to the next heading of its level or above.</summary>
      private static string Section_(string path, string heading)
      {
         string text = File.ReadAllText(path).Replace("\r\n", "\n");
         IReadOnlyList<MdHeading> headings = MarkdownBlocks.Headings(text);
         int at = headings.ToList().FindIndex(h => h.Text == heading);
         Assert.True(at >= 0, Path.GetFileName(path) + " has no heading '" + heading + "'.");
         int start = text.IndexOf("## " + heading, StringComparison.Ordinal);
         if (start < 0)
            start = text.IndexOf(heading + "\n", StringComparison.Ordinal);
         int end = at + 1 < headings.Count ? text.IndexOf(headings[at + 1].Text, start + heading.Length, StringComparison.Ordinal) : -1;
         return end < 0 ? text.Substring(start) : text.Substring(start, end - start);
      }

      [Fact]
      public void AReferenceOnlyDocumentExistsAndIsMappedByNoPage()
      {
         string docs = FindDocs();
         var named = HelpMap.Pages.Values.Concat(HelpMap.Tabs.Values).Where(t => t.IsGuide).Select(t => t.Doc).ToHashSet(StringComparer.Ordinal);
         foreach (string doc in HelpMap.ReferenceOnly)
         {
            Assert.True(File.Exists(Path.Combine(docs, doc)), doc + " is listed as reference-only but is not a document");
            Assert.DoesNotContain(doc, named);
         }
      }

      /// <summary>
      /// The program's embedded copy of the documentation is the directory: the
      /// same files but README.md and DeckParity.md, in ordinal order as the
      /// server lists them, each the file's bytes with CRLF made LF. A broken
      /// Exclude or LogicalName in the project file shows here and nowhere else.
      /// </summary>
      [Fact]
      public void TheEmbeddedCopyIsTheDocumentsDirectory()
      {
         string docs = FindDocs();
         string[] expected = Directory.GetFiles(docs, "*.md")
            .Select(Path.GetFileName)
            .Where(f => f != "README.md" && f != "DeckParity.md")
            .Select(f => f.Substring(0, f.Length - 3))
            .OrderBy(n => n, StringComparer.Ordinal)
            .ToArray();

         Assert.NotEmpty(expected);
         Assert.Equal(expected, HelpPages.Names.ToArray());
         Assert.False(HelpPages.Has("README"));
         Assert.False(HelpPages.Has("DeckParity"));

         foreach (string name in expected)
         {
            string file = File.ReadAllText(Path.Combine(docs, name + ".md")).Replace("\r\n", "\n");
            Assert.Equal(file, HelpPages.Read(name));
         }

         Assert.Equal("Backup", HelpPages.Find("backup"));
         Assert.Null(HelpPages.Read("NoSuchDocument"));
      }

      /// <summary>
      /// The wiki base URL the pane hands to the shell is kept only when it is
      /// a web address: the rule HelpPane's setter applies, held here because the
      /// setter itself is WPF and the test project has no window.
      /// </summary>
      [Theory]
      [InlineData("https://gitlab.com/hmailserver/hmailserver/-/wikis/", "https://gitlab.com/hmailserver/hmailserver/-/wikis/")]
      [InlineData("  HTTP://wiki.example.test/  ", "HTTP://wiki.example.test/")]
      [InlineData("", "")]
      [InlineData(null, "")]
      [InlineData("C:\\Windows\\System32\\calc.exe", "")]
      [InlineData("file:///C:/x", "")]
      [InlineData("ftp://wiki.example.test/", "")]
      [InlineData("wiki.example.test", "")]
      [InlineData("javascript:alert(1)", "")]
      public void TheWikiBaseUrlIsKeptOnlyWhenItIsAWebAddress(string value, string expected)
      {
         Assert.Equal(expected, HelpPages.WikiBaseUrlOrEmpty(value));
      }

      /// <summary>
      /// "Open on the wiki" opens the wiki's page made from the document, by the
      /// wiki's own name for it - the wiki has no page called Backup, and a link
      /// made of the document's name opened GitLab's "page does not exist" for
      /// every document but two - with the base the server holds, the one shipped
      /// up to 6.3.5 included (GitLab redirects it to the group's path); and no
      /// link at all for a document the wiki has no page for, for no base, and for
      /// a base that is not a web address.
      /// </summary>
      [Theory]
      [InlineData("https://gitlab.com/hmailserver/hmailserver/-/wikis/", "Backup", "https://gitlab.com/hmailserver/hmailserver/-/wikis/Backup-and-Restore")]
      [InlineData("https://gitlab.com/hmailserver/hmailserver/-/wikis/", "DiagnosingStalledMail", "https://gitlab.com/hmailserver/hmailserver/-/wikis/Diagnosing-Stalled-Mail")]
      [InlineData("https://gitlab.com/hmailserver/hmailserver/-/wikis/", "backup", "https://gitlab.com/hmailserver/hmailserver/-/wikis/Backup-and-Restore")]
      [InlineData("https://gitlab.com/hmailserver/hmailserver/-/wikis/", "OfflineMail", "https://gitlab.com/hmailserver/hmailserver/-/wikis/The-Webmail")]
      [InlineData("https://gitlab.com/Progressiverobot/hmailserver/-/wikis/", "SecureDefaults", "https://gitlab.com/Progressiverobot/hmailserver/-/wikis/Secure-Defaults")]
      [InlineData("  https://wiki.example.test/w/  ", "Migration", "https://wiki.example.test/w/Migration")]
      [InlineData("https://gitlab.com/hmailserver/hmailserver/-/wikis/", "SetupChecklist", null)]
      [InlineData("https://gitlab.com/hmailserver/hmailserver/-/wikis/", "SettingsReasons", null)]
      [InlineData("https://gitlab.com/hmailserver/hmailserver/-/wikis/", "NoSuchDocument", null)]
      [InlineData("https://gitlab.com/hmailserver/hmailserver/-/wikis/", null, null)]
      [InlineData("", "Backup", null)]
      [InlineData(null, "Backup", null)]
      [InlineData("javascript:alert(1)//", "Backup", null)]
      [InlineData("file:///C:/wiki/", "Backup", null)]
      public void TheWikiLinkIsTheWikisOwnPageForTheDocument(string baseUrl, string name, string expected)
      {
         Assert.Equal(expected, HelpPages.WikiUrl(baseUrl, name));
      }

      /// <summary>
      /// Every document the program carries either opens a page the wiki has - one
      /// of those build/help-wiki-pages.txt lists, which build/check-help-map.py
      /// --wiki holds to the wiki's repository - or has no wiki link: none opens
      /// the base with its own name unless that is the wiki's name for its page
      /// too. And the program's table is the file's, line for line, so a table
      /// edited without running build/generate-help-pages.py fails here as well as
      /// in its --check.
      /// </summary>
      [Fact]
      public void EveryDocumentOpensAPageTheWikiHas_OrNoWikiLinkAtAll()
      {
         const string Base = "https://gitlab.com/hmailserver/hmailserver/-/wikis/";
         Dictionary<string, string> table = ReadWikiTable_(out HashSet<string> wikiPages);

         var wrong = new List<string>();
         int linked = 0;
         foreach (string name in HelpPages.Names)
         {
            string url = HelpPages.WikiUrl(Base, name);
            if (url == null)
            {
               if (table.ContainsKey(name))
                  wrong.Add(name + ": the table gives it " + table[name] + ", and it has no link");
               continue;
            }

            linked++;
            string page = url.Substring(Base.Length);
            if (!wikiPages.Contains(page))
               wrong.Add(name + ": opens " + page + ", which is not a page of the wiki");
            if (!table.TryGetValue(name, out string listed) || listed != page)
               wrong.Add(name + ": opens " + page + ", where the table gives " + (listed ?? "no page"));
         }

         Assert.True(wrong.Count == 0, string.Join("; ", wrong));
         Assert.Equal(table.Count, linked);
         Assert.Equal(table.OrderBy(e => e.Key, StringComparer.Ordinal), HelpWikiPages.ByDocument.OrderBy(e => e.Key, StringComparer.Ordinal));
      }

      /// <summary>
      /// The pane's foot link is made by HelpPages.WikiUrl - shown, given its tool
      /// tip and opened from the one address - and never of the base and the
      /// document's own name, which is what it was made of until October 2026. A
      /// source test because the pane is WPF and this project has no window, as
      /// the scroll's test above is.
      /// </summary>
      [Fact]
      public void ThePaneOpensTheWikisPage_NotTheDocumentsName()
      {
         string pane = File.ReadAllText(Path.Combine(FindControlPanel(), "Views", "HelpPane.cs"));

         Assert.Contains("HelpPages.WikiUrl(wikiBaseUrl_, shown_)", pane, StringComparison.Ordinal);
         Assert.DoesNotContain("wikiBaseUrl_ + shown_", pane, StringComparison.Ordinal);
         string update = WithheldRowTests.MemberText(pane, "private void UpdateWikiLink_()");
         string open = WithheldRowTests.MemberText(pane, "private void OpenWiki_()");
         Assert.True(update != null && open != null, "HelpPane no longer draws and opens its wiki link in UpdateWikiLink_ and OpenWiki_.");
         Assert.Contains("WikiUrl_()", update, StringComparison.Ordinal);
         Assert.Contains("url == null ? Visibility.Collapsed : Visibility.Visible", update, StringComparison.Ordinal);
         Assert.Contains("wikiButton_.ToolTip = url;", update, StringComparison.Ordinal);
         Assert.Contains("WikiUrl_()", open, StringComparison.Ordinal);
         Assert.Contains("new ProcessStartInfo(url)", open, StringComparison.Ordinal);
      }

      /// <summary>
      /// build/help-wiki-pages.txt as build/generate-help-pages.py reads it: every
      /// line not blank or a comment, a page of the wiki and then the documents it
      /// was made from. The documents' names without .md, to the page; and every
      /// page of the wiki the file lists.
      /// </summary>
      private static Dictionary<string, string> ReadWikiTable_(out HashSet<string> pages)
      {
         string file = null;
         for (var at = new DirectoryInfo(AppContext.BaseDirectory); at != null && file == null; at = at.Parent)
         {
            string candidate = Path.Combine(at.FullName, "build", "help-wiki-pages.txt");
            if (File.Exists(candidate))
               file = candidate;
         }
         Assert.True(file != null, "build/help-wiki-pages.txt is not above " + AppContext.BaseDirectory);

         var table = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
         pages = new HashSet<string>(StringComparer.Ordinal);
         foreach (string line in File.ReadAllLines(file))
         {
            int comment = line.IndexOf('#');
            string[] fields = (comment < 0 ? line : line.Substring(0, comment)).Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
            if (fields.Length == 0)
               continue;
            pages.Add(fields[0]);
            foreach (string doc in fields.Skip(1))
               table.Add(doc.EndsWith(".md", StringComparison.Ordinal) ? doc.Substring(0, doc.Length - 3) : doc, fields[0]);
         }
         return table;
      }

      private static string FindControlPanel()
      {
         for (var at = new DirectoryInfo(AppContext.BaseDirectory); at != null; at = at.Parent)
         {
            string candidate = Path.Combine(at.FullName, "ControlPanel");
            if (Directory.Exists(Path.Combine(candidate, "Views")))
               return candidate;
         }

         throw new DirectoryNotFoundException("the Control Panel's sources are not above " + AppContext.BaseDirectory);
      }

      private static string FindDocs()
      {
         for (var at = new DirectoryInfo(AppContext.BaseDirectory); at != null; at = at.Parent)
         {
            string candidate = Path.Combine(at.FullName, "hmailserver", "docs");
            if (Directory.Exists(candidate))
               return candidate;
         }

         throw new DirectoryNotFoundException("the documentation is not above " + AppContext.BaseDirectory);
      }
   }
}
