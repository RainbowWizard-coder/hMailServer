// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Every configuration warning on the Server status page carries its level as
   /// a StatusLevel, never as a word. It carried a word - "Critical", "High",
   /// "Medium", "Info" - passed through the catalogue and then matched back
   /// against the English, so in German "Kritisch" and "Mittel" matched nothing
   /// and fell to the mapping's default, Warning: the open relay was demoted and
   /// the auto-ban count promoted, on the one page that says "this server may be
   /// an open relay", and Japanese escaped only because its catalogue left those
   /// words in English. A level cannot be translated, so there is no mapping left
   /// to get wrong in any language.
   ///
   /// Scanned rather than rendered, as UnreadRowTests is: this assembly has no
   /// WPF. Each test fails against the tree before the fix.
   /// </summary>
   public class StatusWarningLevelTests
   {
      [Fact]
      public void AddWarning_TakesALevel_NotAWord()
      {
         string source = ReadStatusView();
         Assert.True(source.Contains("private int AddWarning(StatusLevel level, string text)", StringComparison.Ordinal),
            "StatusView.AddWarning no longer takes a StatusLevel.");
         Assert.False(source.Contains("AddWarning(string severity", StringComparison.Ordinal),
            "StatusView.AddWarning takes a severity word, which the catalogue translates and nothing can then map.");
         Assert.False(source.Contains("ForConfigurationWarning", StringComparison.Ordinal),
            "StatusView still maps a word onto a level.");
      }

      [Fact]
      public void EveryWarning_NamesItsLevel_AndNoSeverityWordIsTranslated()
      {
         string source = ReadStatusView();
         MatchCollection calls = Regex.Matches(source, @"(?<!int )AddWarning\(([^,]+),");
         Assert.True(calls.Count >= 6, "The Server status page has fewer configuration warnings than it had.");

         foreach (Match call in calls)
         {
            string level = call.Groups[1].Value.Trim();
            Assert.True(level.StartsWith("StatusLevel.", StringComparison.Ordinal),
               "A configuration warning is added with " + level + " rather than a StatusLevel.");
         }

         foreach (string word in new[] { "L(\"Critical\")", "L(\"High\")", "L(\"Medium\")", "L(\"Info\")" })
            Assert.False(source.Contains(word, StringComparison.Ordinal), "StatusView translates the severity word " + word + ", which no mapping can then recognise.");
      }

      /// <summary>
      /// The levels themselves: the open relay is the one Critical, the three
      /// misconfigurations are Warnings, and the auto-ban count and "could not
      /// evaluate" are Information - the same in every language, because a level
      /// is not a word.
      /// </summary>
      [Theory]
      [InlineData("open relay risk", "StatusLevel.Critical")]
      [InlineData("No public host name is configured", "StatusLevel.Warning")]
      [InlineData("Mail from an empty sender address is denied", "StatusLevel.Warning")]
      [InlineData("Localhost is currently banned", "StatusLevel.Warning")]
      [InlineData("auto-ban IP range in all", "StatusLevel.Information")]
      [InlineData("Could not evaluate all warnings", "StatusLevel.Information")]
      public void EachWarning_CarriesTheLevelItAlwaysMeant(string fragment, string level)
      {
         string source = ReadStatusView();
         // A count's warning is a Plural call, its one form first.
         Match call = Regex.Matches(source, @"AddWarning\((StatusLevel\.\w+),\s*(?:[LF]\(|Plural\([^,]+,\s*)""([^""]*)""")
            .Cast<Match>()
            .FirstOrDefault(m => m.Groups[2].Value.Contains(fragment, StringComparison.Ordinal));
         Assert.True(call != null, "The Server status page no longer warns about \"" + fragment + "\".");
         Assert.Equal(level, call.Groups[1].Value);
      }

      private static string ReadStatusView()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "StatusView.xaml.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/StatusView.xaml.cs could not be found from the test binaries.");
         return null;
      }
   }
}
