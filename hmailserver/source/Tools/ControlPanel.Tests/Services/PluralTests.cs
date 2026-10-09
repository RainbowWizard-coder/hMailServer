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
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Loc.Plural: a count in the form its language has for that number (review
   /// finding F282). The Control Panel wrote "4 message(s) waiting for delivery",
   /// which German translated "Nachricht(en)", and its two-form pairs - "1 message
   /// held." and "{0} messages held." - gave Polish, Czech, Russian and Ukrainian
   /// one plural for 2 and for 5, where each has two. The categories are CLDR's,
   /// for whole numbers; the catalogue holds the form for 2 to 4 under the plural
   /// key followed by "|few".
   /// </summary>
   [Collection("Loc")]
   public class PluralTests : IDisposable
   {
      private const string One = "{0} message held.";
      private const string Other = "{0} messages held.";

      public void Dispose() => Loc.Reset();

      [Theory]
      [InlineData(1, "1 message held.")]
      [InlineData(0, "0 messages held.")]
      [InlineData(2, "2 messages held.")]
      [InlineData(21, "21 messages held.")]
      public void EnglishSaysOneForOneAndThePluralForEveryOtherNumber(long count, string expected)
      {
         Assert.Equal(expected, Loc.Plural(count, One, Other));
      }

      [Theory]
      [InlineData("pl", 1, "one")]
      [InlineData("pl", 2, "few")]
      [InlineData("pl", 4, "few")]
      [InlineData("pl", 5, "many")]
      [InlineData("pl", 12, "many")]
      [InlineData("pl", 14, "many")]
      [InlineData("pl", 21, "many")]
      [InlineData("pl", 22, "few")]
      [InlineData("pl", 0, "many")]
      [InlineData("ru", 1, "one")]
      [InlineData("ru", 21, "one")]
      [InlineData("ru", 11, "many")]
      [InlineData("ru", 3, "few")]
      [InlineData("ru", 13, "many")]
      [InlineData("ru", 104, "few")]
      [InlineData("ru", 111, "many")]
      [InlineData("uk", 31, "one")]
      [InlineData("uk", 24, "few")]
      [InlineData("cs", 1, "one")]
      [InlineData("cs", 3, "few")]
      [InlineData("cs", 5, "other")]
      [InlineData("cs", 22, "other")]
      [InlineData("fr", 0, "one")]
      [InlineData("fr", 1, "one")]
      [InlineData("fr", 2, "other")]
      [InlineData("fr", 1000000, "many")]
      [InlineData("pt-BR", 0, "one")]
      [InlineData("de", 0, "other")]
      [InlineData("de", 1, "one")]
      [InlineData("es", 2000000, "many")]
      [InlineData("ja", 1, "other")]
      [InlineData("zh-Hans", 1, "other")]
      [InlineData("tr", 1, "one")]
      [InlineData("en", 1, "one")]
      [InlineData("ko", 1, "one")]
      public void TheCategoryIsCldrsForAWholeNumber(string language, long count, string category)
      {
         Assert.Equal(category, Loc.PluralCategory(language, count));
      }

      [Theory]
      [InlineData(1, "1 wiadomość wstrzymana.")]
      [InlineData(2, "2 wiadomości wstrzymane.")]
      [InlineData(22, "22 wiadomości wstrzymane.")]
      [InlineData(5, "5 wiadomości wstrzymanych.")]
      [InlineData(12, "12 wiadomości wstrzymanych.")]
      [InlineData(21, "21 wiadomości wstrzymanych.")]
      public void PolishTakesItsFormForTwoToFourFromTheFewEntry(long count, string expected)
      {
         var polish = new Dictionary<string, string>
         {
            [One] = "{0} wiadomość wstrzymana.",
            [Other] = "{0} wiadomości wstrzymanych.",
            [Other + "|few"] = "{0} wiadomości wstrzymane.",
         };
         Loc.Use((text, culture) => polish.TryGetValue(text, out string value) ? value : null, CultureInfo.GetCultureInfo("pl-PL"));

         Assert.Equal(expected, Loc.Plural(count, One, Other));
      }

      [Theory]
      [InlineData(21, "21 сообщение задержано.")]
      [InlineData(3, "3 сообщения задержаны.")]
      [InlineData(11, "11 сообщений задержано.")]
      public void RussianSaysTwentyOneAsItSaysOne(long count, string expected)
      {
         var russian = new Dictionary<string, string>
         {
            [One] = "{0} сообщение задержано.",
            [Other] = "{0} сообщений задержано.",
            [Other + "|few"] = "{0} сообщения задержаны.",
         };
         Loc.Use((text, culture) => russian.TryGetValue(text, out string value) ? value : null, CultureInfo.GetCultureInfo("ru"));

         Assert.Equal(expected, Loc.Plural(count, One, Other));
      }

      [Fact]
      public void AMissingFewFormFallsBackToThePluralAndNeverToEnglish()
      {
         var czech = new Dictionary<string, string> { [One] = "{0} zpráva pozdržena.", [Other] = "{0} zpráv pozdrženo." };
         Loc.Use((text, culture) => czech.TryGetValue(text, out string value) ? value : null, CultureInfo.GetCultureInfo("cs"));

         Assert.Equal("3 zpráv pozdrženo.", Loc.Plural(3, One, Other));
      }

      [Fact]
      public void JapaneseHasOneFormForEveryNumber()
      {
         var japanese = new Dictionary<string, string> { [One] = "保留中 {0} 件 (単数)", [Other] = "保留中のメッセージ {0} 件。" };
         Loc.Use((text, culture) => japanese.TryGetValue(text, out string value) ? value : null, CultureInfo.GetCultureInfo("ja-JP"));

         Assert.Equal("保留中のメッセージ 1 件。", Loc.Plural(1, One, Other));
      }

      [Fact]
      public void ALanguageWithNoCatalogueCountsInEnglish()
      {
         Loc.Use((text, culture) => null, CultureInfo.GetCultureInfo("ko"));

         Assert.Equal("1 message held.", Loc.Plural(1, One, Other));
         Assert.Equal("3 messages held.", Loc.Plural(3, One, Other));
      }

      [Fact]
      public void PluralFFillsTheChosenFormWithItsArguments()
      {
         Assert.Equal("Saved 1 setting at 12:00.", Loc.PluralF(1, "Saved {0} setting at {1}.", "Saved {0} settings at {1}.", 1, "12:00"));
         Assert.Equal("2 of 5 changes failed.", Loc.PluralF(2, "{0} of {1} changes failed.", "{0} of {1} changes failed.", 2, 5));
      }

      /// <summary>
      /// The catalogues as committed: a "|few" entry only in the four languages
      /// that count two to four apart, each the form of a key the English
      /// catalogue holds, with that key's placeholders.
      /// </summary>
      [Fact]
      public void FewFormsAreOnlyWhereALanguageHasThemAndKeepTheirPlaceholders()
      {
         string resources = ResourcesDirectory();
         if (resources == null)
            return;   // sources not available (a packaged drop)

         HashSet<string> english = ReadResx(Path.Join(resources, "Strings.resx")).Keys.ToHashSet(StringComparer.Ordinal);
         var withFew = new HashSet<string> { "cs", "pl", "ru", "uk" };
         var placeholders = new Regex(@"\{(\d+)(?:[:,][^}]*)?\}");
         var problems = new List<string>();
         int fewForms = 0;
         foreach (string path in Directory.GetFiles(resources, "Strings.*.resx"))
         {
            string tag = Path.GetFileNameWithoutExtension(path).Substring("Strings.".Length);
            foreach ((string key, string value) in ReadResx(path).Where(pair => pair.Key.EndsWith("|few", StringComparison.Ordinal)))
            {
               fewForms++;
               string plural = key.Substring(0, key.Length - "|few".Length);
               if (!withFew.Contains(tag))
                  problems.Add(tag + " has a |few form, which its language does not count with: " + key);
               if (!english.Contains(plural))
                  problems.Add(tag + ": " + key + " is the form of no English key");
               var wanted = placeholders.Matches(plural).Select(m => m.Groups[1].Value).ToHashSet();
               var found = placeholders.Matches(value).Select(m => m.Groups[1].Value).ToHashSet();
               if (value.Length > 0 && !wanted.SetEquals(found))
                  problems.Add(tag + ": the placeholders of " + key + " differ in " + value);
            }
         }

         Assert.True(problems.Count == 0, string.Join(Environment.NewLine, problems));
         Assert.True(fewForms >= 4 * 50, "Fewer |few forms than the plurals the source asks for: " + fewForms + ".");
      }

      private static Dictionary<string, string> ReadResx(string path)
      {
         var result = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (XElement data in XDocument.Load(path).Root.Elements("data"))
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
         return null;
      }
   }
}
