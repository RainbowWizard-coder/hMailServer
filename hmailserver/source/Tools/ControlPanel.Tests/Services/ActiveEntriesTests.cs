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
   /// The SURBL servers and DNS blacklists pages (review finding F268): the pill
   /// counts the entries the server asks, the refusal text is named as a noun,
   /// and both lists show their columns in one order. The pill read "3 blacklists"
   /// while only the active ones are asked; the column and field were "Reject
   /// message", which German translated "Nachricht ablehnen" and Japanese
   /// "メッセージを拒否する", a command; and the two pages ordered their columns
   /// differently.
   /// </summary>
   [Collection("Loc")]
   public class ActiveEntriesTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      [Theory]
      [InlineData(2, 3, "Active: 2 of 3", StatusLevel.Normal)]
      [InlineData(3, 3, "Active: 3 of 3", StatusLevel.Normal)]
      [InlineData(0, 3, "Active: 0 of 3", StatusLevel.Warning)]
      [InlineData(1, 1, "Active: 1 of 1", StatusLevel.Normal)]
      public void ThePillCountsTheEntriesThatAreOnAndWarnsWhenNoneIs(int active, int total, string text, StatusLevel level)
      {
         Assert.Equal(text, ActiveEntries.PillText(active, total));
         Assert.Equal(level, ActiveEntries.PillLevel(active, total));
      }

      [Fact]
      public void AnEmptyListOrACountNotTakenIsNoWarning()
      {
         Assert.Equal(StatusLevel.Normal, ActiveEntries.PillLevel(0, 0));
         Assert.Equal(StatusLevel.Normal, ActiveEntries.PillLevel(-1, 4));
      }

      /// <summary>
      /// The count the list page takes from its rows: only a switch that reads
      /// true is on - false, a value the server did not give and a row without
      /// the property are off - so of these four one is active, and the pill says
      /// so at Normal.
      /// </summary>
      [Fact]
      public void OnlyARowWhoseSwitchIsTrueCountsAsActive()
      {
         var rows = new List<Dictionary<string, object>>
         {
            new() { ["Active"] = true, ["DNSHost"] = "zen.spamhaus.org" },
            new() { ["Active"] = false, ["DNSHost"] = "b.barracudacentral.org" },
            new() { ["Active"] = null, ["DNSHost"] = "bl.spamcop.net" },
            new() { ["DNSHost"] = "dnsbl.example" },
         };

         int active = ActiveEntries.Count(rows, "Active");

         Assert.Equal(1, active);
         Assert.Equal("Active: 1 of 4", ActiveEntries.PillText(active, rows.Count));
         Assert.Equal(StatusLevel.Normal, ActiveEntries.PillLevel(active, rows.Count));
      }

      /// <summary>
      /// A list with entries none of which is on counts 0 and warns: it checks
      /// nothing, and must not read as a list that checks.
      /// </summary>
      [Fact]
      public void AListWhoseSwitchesAreAllOffCountsNoneAndWarns()
      {
         var rows = new List<Dictionary<string, object>>
         {
            new() { ["Active"] = false },
            new() { ["Active"] = false },
            new() { ["Active"] = false },
         };

         int active = ActiveEntries.Count(rows, "Active");

         Assert.Equal(0, active);
         Assert.Equal("Active: 0 of 3", ActiveEntries.PillText(active, rows.Count));
         Assert.Equal(StatusLevel.Warning, ActiveEntries.PillLevel(active, rows.Count));
      }

      /// <summary>
      /// No count is taken for an empty list or for a list without a switch
      /// (every other list page, which counts its entries in its own words), and
      /// either leaves the pill at Normal.
      /// </summary>
      [Fact]
      public void AnEmptyListOrAListWithoutASwitchTakesNoCount()
      {
         var rows = new List<Dictionary<string, object>> { new() { ["Active"] = true } };

         Assert.Equal(-1, ActiveEntries.Count(new List<Dictionary<string, object>>(), "Active"));
         Assert.Equal(-1, ActiveEntries.Count(rows, null));
         Assert.Equal(-1, ActiveEntries.Count(null, "Active"));
         Assert.Equal(StatusLevel.Normal, ActiveEntries.PillLevel(ActiveEntries.Count(rows, null), rows.Count));
         Assert.Equal(StatusLevel.Normal, ActiveEntries.PillLevel(ActiveEntries.Count(new List<Dictionary<string, object>>(), "Active"), 0));
      }

      [Theory]
      [InlineData("public static CollectionEditorView SurblServers()")]
      [InlineData("public static CollectionEditorView DnsBlackLists()")]
      public void BothListsCountTheirActiveEntriesAndShowOneColumnOrder(string factory)
      {
         string spec = Spec(ReadSource("CollectionSpecs.cs"), factory);

         Assert.Contains("ActiveProp = \"Active\"", spec, StringComparison.Ordinal);

         List<string> props = Regex.Matches(spec, "(?<!\\w)Prop = \"(\\w+)\"").Select(m => m.Groups[1].Value).ToList();
         Assert.Equal(new[] { "Active", "DNSHost", "ExpectedResult", "Score", "RejectMessage" }, props);

         Assert.Contains("Label = L(\"Refusal text\"), Hint = L(\"What the sender is told when this list refuses the message\")", spec, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Reject message\")", spec, StringComparison.Ordinal);
      }

      [Fact]
      public void TheListPageShowsTheActiveCountWhereTheSpecAsksForIt()
      {
         string editor = ReadSource("CollectionEditorView.cs");
         Assert.Contains("public string ActiveProp;", editor, StringComparison.Ordinal);
         Assert.Contains("ActiveEntries.Count(rows_.Select(row => row.Values), spec_.ActiveProp)", editor, StringComparison.Ordinal);
         Assert.Contains("countBadge_.Level = ActiveEntries.PillLevel(activeCount, rows_.Count);", editor, StringComparison.Ordinal);
         Assert.Contains("activeCount >= 0 ? ActiveEntries.PillText(activeCount, rows_.Count)", editor, StringComparison.Ordinal);
      }

      /// <summary>
      /// One count, one wording: the spam overview's list rows and its pipeline
      /// rows say "Active: 2 of 3" as the list pages' pill does, where they said
      /// "2 active of 3" and "2 of 3 active", each translated on its own in
      /// seventeen languages. Every catalogue's pipeline sentence holds its own
      /// translation of the pill.
      /// </summary>
      [Fact]
      public void TheSpamOverviewCountsActiveEntriesInThePillsWordsInEveryLanguage()
      {
         string overview = ReadSource("SpamOverviewView.cs");
         Assert.Contains("ActiveEntries.PillText(active.Value, total)", overview, StringComparison.Ordinal);
         Assert.DoesNotContain("active of {1}", overview, StringComparison.Ordinal);

         Dictionary<string, string> english = ReadResx("Strings.resx");
         Assert.False(english.ContainsKey("{0} active of {1}"), "The catalogue still holds \"{0} active of {1}\".");
         Assert.False(english.ContainsKey("{0}. {1} of {2} active."), "The catalogue still holds \"{0}. {1} of {2} active.\".");

         var unlike = new List<string>();
         foreach (string path in Directory.GetFiles(ResourcesDirectory(), "Strings.*.resx"))
         {
            Dictionary<string, string> catalogue = ReadResx(Path.GetFileName(path));
            string pill = catalogue["Active: {0} of {1}"].Replace("{1}", "{2}", StringComparison.Ordinal).Replace("{0}", "{1}", StringComparison.Ordinal);
            string sentence = catalogue["{0}. Active: {1} of {2}."];
            if (!sentence.Contains(pill, StringComparison.Ordinal))
               unlike.Add(Path.GetFileName(path) + ": '" + sentence + "' does not hold '" + pill + "'");
         }

         Assert.True(unlike.Count == 0, string.Join(Environment.NewLine, unlike));
      }

      [Fact]
      public void TheRefusalTextIsANounInGermanAndJapanese()
      {
         Dictionary<string, string> german = ReadResx("Strings.de.resx");
         Dictionary<string, string> japanese = ReadResx("Strings.ja.resx");

         Assert.Equal("Ablehnungstext", german["Refusal text"]);
         Assert.Equal("拒否時のメッセージ", japanese["Refusal text"]);
         Assert.False(german.ContainsKey("Reject message"), "The German catalogue still holds the command.");
      }

      private static string Spec(string specs, string factory)
      {
         int at = specs.IndexOf(factory, StringComparison.Ordinal);
         Assert.True(at >= 0, "No spec " + factory + " in CollectionSpecs.");
         int end = specs.IndexOf("public static CollectionEditorView", at + factory.Length, StringComparison.Ordinal);
         return end > at ? specs.Substring(at, end - at) : specs.Substring(at);
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
