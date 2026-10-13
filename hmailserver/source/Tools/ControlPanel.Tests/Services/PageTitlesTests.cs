// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A page's heading is its navigation title (review findings F215, F025 and
   /// F080; the owner's decision of 9 October 2026 on page names). Each page set
   /// its own title, and eight said something other than the sidebar and the
   /// breadcrumb above them: "Anti-spam" under "Anti-spam settings", "Domains &amp;
   /// accounts" under "Domains", "Automatic certificates (ACME)" under
   /// "Certificates (ACME)", "Web services &amp; client autoconfiguration" under
   /// "Web services &amp; autoconfiguration". A page now names its navigation key
   /// in its header (PageHeader.PageKey), and the header takes the map's title for
   /// it, so the two cannot drift; and where a page has nothing more to say than
   /// its navigation description, its subtitle is that description, in the same
   /// words.
   ///
   /// The test project builds against ControlPanel.Core, which has no WPF, so the
   /// pages are held to the rule by their sources: every page MainWindow builds
   /// names its key in a header, no header of a page sets a title of its own, and
   /// the header takes its title and its default subtitle from the map.
   /// </summary>
   public class PageTitlesTests
   {
      [Fact]
      public void EveryPageNamesItsNavigationKeyInItsHeader()
      {
         var registered = new HashSet<string>(
            Regex.Matches(ReadControlPanelFile("MainWindow.xaml.cs"), "pageFactories_\\[\"([^\"]+)\"\\]").Select(m => m.Groups[1].Value),
            StringComparer.Ordinal);
         Assert.True(registered.Count >= 60, "Only " + registered.Count + " page factories were found in MainWindow; the scan needs updating.");

         var named = new List<string>();
         foreach (string file in ViewSources())
         {
            // Not the comments: PageHeader's own documentation shows a key.
            string text = string.Join("\n", File.ReadAllLines(file).Where(line => !line.TrimStart().StartsWith("//", StringComparison.Ordinal)));
            named.AddRange(Regex.Matches(text, "PageKey\\s*=\\s*\"([a-z]+)\"").Select(m => m.Groups[1].Value));
         }

         List<string> unnamed = registered.Where(key => !named.Contains(key)).OrderBy(key => key).ToList();
         Assert.True(unnamed.Count == 0, "These pages do not name their key in a header, so their title is not the navigation map's: " + string.Join(", ", unnamed));

         List<string> unknown = named.Where(key => NavigationMap.Find(key) == null).Distinct().OrderBy(key => key).ToList();
         Assert.True(unknown.Count == 0, "A header names a key the navigation map does not have: " + string.Join(", ", unknown));

         List<string> twice = named.GroupBy(key => key).Where(g => g.Count() > 1).Select(g => g.Key).OrderBy(key => key).ToList();
         Assert.True(twice.Count == 0, "Two headers name the same page: " + string.Join(", ", twice));
      }

      /// <summary>
      /// No header of a page sets its title: a title set beside the key would say
      /// whatever it says, which is how the eight came to disagree. A list embedded
      /// in a dialog's tab has no page header, and keeps its spec's own title.
      /// </summary>
      [Fact]
      public void NoPageHeaderSetsATitleOfItsOwn()
      {
         var offenders = new List<string>();
         foreach (string file in ViewSources())
         {
            string text = File.ReadAllText(file);
            string name = Path.GetFileName(file);

            if (file.EndsWith(".xaml", StringComparison.OrdinalIgnoreCase))
            {
               foreach (Match header in Regex.Matches(text, "<scaffold:PageHeader\\b[^>]*>"))
               {
                  if (Regex.IsMatch(header.Value, "\\sTitle=\""))
                     offenders.Add(name + ": " + header.Value);
               }
               continue;
            }

            foreach (Match header in Regex.Matches(text, "new (?:Scaffold\\.)?PageHeader\\b[^{;]*\\{[^}]*\\}"))
            {
               if (Regex.IsMatch(header.Value, "\\bTitle\\s*="))
                  offenders.Add(name + ": " + header.Value.Replace("\n", " "));
            }

            foreach (Match assignment in Regex.Matches(text, "\\b(?:Header|header_|HeaderPanel)\\.Title\\s*=[^;]*;"))
               offenders.Add(name + ": " + assignment.Value);
         }

         Assert.True(offenders.Count == 0, "A page header sets a title of its own: " + string.Join(" | ", offenders));
      }

      /// <summary>
      /// The header takes the map's title for its key, translated where it is
      /// shown, and the navigation description for a subtitle the page left unset;
      /// the list pages take the same title for their grid's name.
      /// </summary>
      [Fact]
      public void TheHeaderTakesItsTitleAndDefaultSubtitleFromTheMap()
      {
         string header = ReadControlPanelFile(Path.Join("Views", "Scaffold", "PageHeader.cs"));
         Assert.Contains("header.Title = L(Services.NavigationMap.TitleOf(key));", header, StringComparison.Ordinal);
         Assert.Contains("Services.NavigationMap.Find(((PageHeader)d).PageKey)?.Purpose", header, StringComparison.Ordinal);
         Assert.Contains("new PropertyMetadata(null, OnSubtitleChanged, SubtitleOrDescription_)", header, StringComparison.Ordinal);
         Assert.Contains("header.CoerceValue(SubtitleProperty);", header, StringComparison.Ordinal);

         string editor = ReadControlPanelFile(Path.Join("Views", "CollectionEditorView.cs"));
         Assert.Contains("spec.Title = L(NavigationMap.TitleOf(spec.PageKey));", editor, StringComparison.Ordinal);
         Assert.Contains("PageKey = spec_.PageKey,", editor, StringComparison.Ordinal);
      }

      /// <summary>
      /// The pairs the review found: the clearer name is the title once, and the
      /// other is a search alias (the owner's decision on page names), so that a
      /// note or a habit that names the other still finds the page.
      /// </summary>
      [Theory]
      [InlineData("acme", "Automatic certificates (ACME)", "Certificates (ACME)")]
      [InlineData("webservices", "Web services & autoconfiguration", "Web services & client autoconfiguration")]
      [InlineData("antispam", "Anti-spam settings", "Anti-spam")]
      [InlineData("antivirus", "Anti-virus settings", "Anti-virus")]
      [InlineData("domains", "Domains", "Domains & accounts")]
      [InlineData("rules", "Rules", "Global rules")]
      [InlineData("spamwhitelist", "Spam exemptions", "Anti-spam white list")]
      [InlineData("dnsbl", "DNS block lists (DNSBL)", "DNS blacklists (DNSBL)")]
      [InlineData("certs", "TLS certificates", "SSL certificates")]
      [InlineData("tls", "TLS versions & ciphers", "SSL/TLS")]
      public void ThePageIsCalledOneThingAndTheOtherNameFindsIt(string key, string title, string alias)
      {
         NavNode page = NavigationMap.Find(key);
         Assert.NotNull(page);
         Assert.Equal(title, page.Title);
         Assert.Contains(alias, page.Aliases);

         List<PaletteRow> rows = PaletteSearch.Query(alias, null, null).Where(r => r.Kind == PaletteRowKind.Page).ToList();
         Assert.Contains(key, rows.Select(r => r.Page));
      }

      /// <summary>
      /// The old ACME title is still a way in: its documented path resolves, and
      /// it is not a heading anywhere any more.
      /// </summary>
      [Fact]
      public void TheOldAcmeTitleStillResolves()
      {
         Assert.Equal("acme", NavigationMap.LegacyPaths["Settings > Security > Certificates (ACME)"]);
         string feature = ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml.cs"));
         Assert.Contains("Header.PageKey = \"acme\";", feature, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Web services & client autoconfiguration\")", feature, StringComparison.Ordinal);
      }

      /// <summary>
      /// A page whose subtitle only said its navigation description again in
      /// other words - "Real-time block lists checked against the connecting IP
      /// address." under a description of "Block lists checked against the IP
      /// address that is connecting." - leaves it unset and shows the description;
      /// the Domains page's subtitle says "accounts", as its page does, where it
      /// said "mailboxes" (F080).
      /// </summary>
      [Fact]
      public void APageWithNothingMoreToSayShowsItsDescription()
      {
         string specs = ReadControlPanelFile(Path.Join("Views", "CollectionSpecs.cs"));
         foreach (string key in new[] { "dnsbl", "spamwhitelist", "greylistwhitelist" })
         {
            Match spec = Regex.Match(specs, "PageKey = \"" + key + "\",(?<rest>[\\s\\S]{0,600})");
            Assert.True(spec.Success, key + " is not a collection page.");
            Assert.DoesNotMatch("^\\s*Subtitle\\s*=", spec.Groups["rest"].Value);
         }

         // Routes names its key and sets no subtitle: its description is shown. The
         // header holds its count and Refresh (the list page pattern, F288).
         XElement routes = XDocument.Parse(ReadControlPanelFile(Path.Join("Views", "RoutesView.xaml"))).Root
            .Descendants().Single(e => e.Name.LocalName == "PageHeader");
         Assert.Equal("routes", (string)routes.Attribute("PageKey"));
         Assert.Null(routes.Attribute("Subtitle"));

         string domains = ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml"));
         Assert.Contains("The domains this server hosts, with their accounts, aliases and distribution lists.", domains, StringComparison.Ordinal);
         Assert.DoesNotContain("their mailboxes", domains, StringComparison.Ordinal);
      }

      private static IEnumerable<string> ViewSources()
      {
         string views = Path.Join(FindControlPanelDirectory(), "Views");
         return Directory.EnumerateFiles(views, "*.cs", SearchOption.AllDirectories)
            .Concat(Directory.EnumerateFiles(views, "*.xaml", SearchOption.AllDirectories))
            .Where(file => !file.Contains(Path.DirectorySeparatorChar + "obj" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            .OrderBy(file => file, StringComparer.Ordinal);
      }

      private static string ReadControlPanelFile(string relativePath)
      {
         string path = Path.Join(FindControlPanelDirectory(), relativePath);
         Assert.True(File.Exists(path), path + " could not be found from the test binaries.");
         return File.ReadAllText(path);
      }

      private static string FindControlPanelDirectory()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel");
            if (Directory.Exists(Path.Join(candidate, "Views")))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("The Control Panel sources could not be found from the test binaries.");
         return null;
      }
   }
}
