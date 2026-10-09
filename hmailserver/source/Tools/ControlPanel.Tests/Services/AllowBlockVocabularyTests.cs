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
   /// The allow and block lists in one vocabulary (review finding F214, the owner's
   /// decision on the allow and block words). The lists had about five names each -
   /// white list, whitelist, white-list and allow list; blacklist, black list, block
   /// list, blocklist and real-time blackhole lists - and German drifted between
   /// Whitelist, Positivliste and Weißliste. The anti-spam white list is Spam
   /// exemptions, the greylisting white list Greylisting exemptions, the domain white
   /// and black list Allowed and blocked domains, the DNS blacklists DNS block lists
   /// (DNSBL), and the quarantine allows and blocks a sender; each old page title
   /// stays a search alias.
   /// </summary>
   public class AllowBlockVocabularyTests
   {
      [Theory]
      [InlineData("spamwhitelist", "Spam exemptions", "White list|Anti-spam white list")]
      [InlineData("greylistwhitelist", "Greylisting exemptions", "Greylisting white list")]
      [InlineData("domainpolicylist", "Allowed and blocked domains", "Domain white and black list")]
      [InlineData("dnsbl", "DNS block lists (DNSBL)", "DNS blacklists|DNS blacklists (DNSBL)")]
      public void EachListIsNamedInTheAllowAndBlockWordsAndKeepsItsOldNameAsAnAlias(string key, string title, string oldNames)
      {
         NavNode page = NavigationMap.Find(key);
         Assert.NotNull(page);
         Assert.Equal(title, page.Title);
         foreach (string old in oldNames.Split('|'))
            Assert.Contains(old, page.Aliases);
      }

      [Theory]
      [InlineData("CollectionSpecs.cs", "Spam exemptions")]
      [InlineData("CollectionSpecs.cs", "Greylisting exemptions")]
      [InlineData("CollectionSpecs.cs", "DNS block lists (DNSBL)")]
      [InlineData("DomainPolicyListView.cs", "Allowed and blocked domains")]
      public void EachPagesHeadingIsItsNavigationTitle(string view, string title)
      {
         Assert.Contains("Title = L(\"" + title + "\")", ReadSource(view), StringComparison.Ordinal);
      }

      [Fact]
      public void NoEnglishTextNamesAListWhiteOrBlack()
      {
         var words = new Regex(@"white[ -]?list|black[ -]?list|blackhole|blocklist|\bwhite\b|\bblack\b", RegexOptions.IgnoreCase);
         List<string> found = ReadResx("Strings.resx").Keys
            .Where(key => words.IsMatch(key.Replace("SpamTestDNSBlackLists", "", StringComparison.Ordinal)))
            .ToList();
         Assert.True(found.Count == 0, "Still white or black: " + string.Join(" | ", found));
      }

      /// <summary>
      /// A list's new name is a set of entries, not a list, so a sentence that
      /// needs a list says so: the quarantine dialog offered "The sender's
      /// domain, on the allowed and blocked domains" under "The sender's address,
      /// on the server's allow and block list", and the notes said "a sender on
      /// the spam exemptions" and "is on the greylisting exemptions" (review
      /// finding F214, second pass).
      /// </summary>
      [Fact]
      public void NoEnglishTextPutsAThingOnANameThatIsNotAList()
      {
         var onAName = new Regex(@"\bon the (spam exemptions|greylisting exemptions|allowed and blocked domains)\b", RegexOptions.IgnoreCase);
         Dictionary<string, string> english = ReadResx("Strings.resx");
         List<string> found = english.Keys.Where(key => onAName.IsMatch(key)).ToList();
         Assert.True(found.Count == 0, "On a name that is not a list: " + string.Join(" | ", found));

         Assert.True(english.ContainsKey("The sender's _domain, on the list of allowed and blocked domains"));
         Assert.True(english.ContainsKey("The _HELO name's domain, on the list of allowed and blocked domains"));
         Assert.True(english.ContainsKey("the connecting address matches one of the greylisting exemptions (entries: {0})"));
         Assert.Contains("L(\"The sender's _domain, on the list of allowed and blocked domains\")", ReadSource("QuarantineListDialog.cs"), StringComparison.Ordinal);
      }

      [Fact]
      public void GermanSaysZulassenAndSperrenAndNothingElse()
      {
         var words = new Regex(@"whitelist|blacklist|positivliste|weißliste|weiße liste|weißen liste|schwarzliste|weiß-", RegexOptions.IgnoreCase);
         List<string> found = ReadResx("Strings.de.resx")
            .Where(pair => words.IsMatch(pair.Value.Replace("SpamTestDNSBlackLists", "", StringComparison.Ordinal)))
            .Select(pair => pair.Key + " = " + pair.Value)
            .ToList();
         Assert.True(found.Count == 0, string.Join(Environment.NewLine, found));

         Dictionary<string, string> german = ReadResx("Strings.de.resx");
         Assert.Equal("Zulassungs- und Sperrliste", german["Allow and block list"]);
         Assert.Equal("Zugelassene und gesperrte Domains", german["Allowed and blocked domains"]);
         Assert.Equal("DNS-Sperrlisten (DNSBL)", german["DNS block lists (DNSBL)"]);
         Assert.Equal("Spam-Ausnahmen", german["Spam exemptions"]);
      }

      [Fact]
      public void EveryLanguageNamesTheFourListsAndTellsTheGreylistingPagesApart()
      {
         string[] titles = { "Spam exemptions", "Greylisting exemptions", "Allowed and blocked domains", "DNS block lists (DNSBL)" };
         var problems = new List<string>();
         foreach (string path in Directory.GetFiles(ResourcesDirectory(), "Strings.*.resx"))
         {
            string tag = Path.GetFileNameWithoutExtension(path).Substring("Strings.".Length);
            Dictionary<string, string> catalogue = ReadResx(Path.GetFileName(path));
            foreach (string title in titles)
            {
               if (!catalogue.TryGetValue(title, out string value) || string.IsNullOrWhiteSpace(value))
                  problems.Add(tag + ": no " + title);
            }

            // The two greylisting lists sit side by side in the navigation: the
            // exemptions by address and the bypass by domain must read apart.
            if (catalogue.TryGetValue("Greylisting exemptions", out string exemptions)
                && catalogue.TryGetValue("Greylisting bypass list", out string bypass)
                && SameStem(exemptions, bypass))
            {
                problems.Add(tag + ": '" + exemptions + "' and '" + bypass + "' read alike");
            }
         }

         Assert.True(problems.Count == 0, string.Join(Environment.NewLine, problems));
      }

      // The word for greylisting in each language, which both titles share.
      private static readonly string[] GreylistingWords =
      {
         "greylistingu", "greylisting", "gråstlistning", "grålisting", "harmaalistauksen", "liste grise",
         "серых списков", "сірого списку", "グレイリスティング", "灰名单",
      };

      // Two titles read alike when, the greylisting word aside, they share a word's
      // first four letters (Greylisting-Ausnahmen and Greylisting-Ausnahmeliste did).
      private static bool SameStem(string a, string b)
      {
         static IEnumerable<string> Words(string s)
         {
            string rest = s.ToLowerInvariant();
            foreach (string word in GreylistingWords)
               rest = rest.Replace(word, " ", StringComparison.Ordinal);
            return Regex.Split(rest, @"[\s\-]+").Where(w => w.Length >= 4).Select(w => w.Substring(0, 4));
         }

         return Words(a).Intersect(Words(b)).Any();
      }

      private static Dictionary<string, string> ReadResx(string file)
      {
         var result = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (XElement data in XDocument.Load(Path.Join(ResourcesDirectory(), file)).Root.Elements("data"))
            result[(string)data.Attribute("name")] = data.Element("value")?.Value ?? "";
         return result;
      }

      private static string ResourcesDirectory()
      {
         string dir = AppContext.BaseDirectory;
         for (int i = 0; i < 8 && dir != null; i++)
         {
            string candidate = Path.Join(dir, "ControlPanel", "Resources");
            if (File.Exists(Path.Join(candidate, "Strings.resx")))
               return candidate;
            dir = Path.GetDirectoryName(dir);
         }

         Assert.Fail("ControlPanel/Resources could not be found from the test binaries.");
         return null;
      }

      private static string ReadSource(string view)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", view);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/" + view + " could not be found from the test binaries.");
         return null;
      }
   }
}
