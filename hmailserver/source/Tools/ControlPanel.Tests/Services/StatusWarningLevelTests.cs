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
         Assert.True(source.Contains("private int AddWarning(StatusLevel level, string text", StringComparison.Ordinal),
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
         // Four checks: the auto-ban count went to the statistics and a failed read
         // to the notice under the header, neither of which is a warning.
         Assert.True(calls.Count >= 4, "The Server status page has fewer configuration warnings than it had.");

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
      /// The levels themselves: the open relay is the one Critical and the three
      /// misconfigurations are Warnings - the same in every language, because a
      /// level is not a word - and each names the page that puts it right, which
      /// its notice opens.
      /// </summary>
      [Theory]
      [InlineData("open relay risk", "StatusLevel.Critical", "ipranges")]
      [InlineData("No public host name is configured", "StatusLevel.Warning", "protocols")]
      [InlineData("Mail from an empty sender address is denied", "StatusLevel.Warning", "protocols")]
      [InlineData("Localhost is currently banned", "StatusLevel.Warning", "ipranges")]
      public void EachWarning_CarriesTheLevelItAlwaysMeant_AndThePageThatFixesIt(string fragment, string level, string page)
      {
         string source = ReadStatusView();
         // A count's warning is a Plural call, its one form first.
         Match call = Regex.Matches(source, @"AddWarning\((StatusLevel\.\w+),\s*(?:[LF]\(|Plural\([^,]+,\s*)""([^""]*)""[^;]*?,\s*""(\w+)""\);")
            .Cast<Match>()
            .FirstOrDefault(m => m.Groups[2].Value.Contains(fragment, StringComparison.Ordinal));
         Assert.True(call != null, "The Server status page no longer warns about \"" + fragment + "\" with the page that fixes it.");
         Assert.Equal(level, call.Groups[1].Value);
         Assert.Equal(page, call.Groups[3].Value);
      }

      /// <summary>
      /// The auto-ban count is a statistic, not a warning: it was the one
      /// "Information" among the configuration warnings on most servers. And a
      /// read that failed is said once, under the header, at the Critical level,
      /// where it was an Information line at the foot of the warnings card under
      /// a page of readouts that each said "-".
      /// </summary>
      [Fact]
      public void TheAutoBanCount_IsAStatistic_AndAFailedRead_IsCriticalUnderTheHeader()
      {
         string source = ReadStatusView();
         Assert.DoesNotContain("auto-ban IP range(s)", source, StringComparison.Ordinal);
         Assert.Contains("SetValue_(AutobanValue, L(\"Auto-ban IP ranges\")", source, StringComparison.Ordinal);
         Assert.DoesNotContain("Could not evaluate all warnings", source, StringComparison.Ordinal);

         string show = WithheldRowTests.MemberText(source, "private void ShowReadFailure_()");
         Assert.True(show != null, "StatusView.ShowReadFailure_ is gone.");
         Assert.Contains("ReadFailure.Show(ReadNotice,", show, StringComparison.Ordinal);
         foreach (string button in new[] { "PauseButton", "CheckUpdateButton", "DownloadUpdateButton", "InstallUpdateButton" })
            Assert.Contains(button, show, StringComparison.Ordinal);

         // A readout that could not be read says so; "-" is what an empty value shows.
         Assert.DoesNotContain("L(\"Version\"), \"-\")", source, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Processed messages\"), \"-\")", source, StringComparison.Ordinal);

         string xaml = ReadStatusViewXaml();
         int notice = xaml.IndexOf("x:Name=\"ReadNotice\"", StringComparison.Ordinal);
         int checks = xaml.IndexOf("{loc:L 'Configuration checks'}", StringComparison.Ordinal);
         int server = xaml.IndexOf("{loc:L 'Server'}", StringComparison.Ordinal);
         Assert.True(notice > 0 && checks > notice && server > checks,
            "The read failure and the configuration checks are not under the header, above the readouts.");
         Assert.DoesNotContain("{loc:L 'Warnings'}", xaml, StringComparison.Ordinal);
      }

      /// <summary>
      /// The auto-ban count is a group of the statistics of its own, under a line
      /// that says what it counts, after the grid of the sessions: it is neither
      /// counted since the start nor a connection, and as a fourth cell of the
      /// three-column grid under "Connected now" it wrapped to a row of its own
      /// there, read as a fourth kind of session.
      /// </summary>
      [Fact]
      public void TheAutoBanCount_IsAGroupOfItsOwn_NotACellOfTheSessions()
      {
         string xaml = ReadStatusViewXaml();
         int connected = xaml.IndexOf("{loc:L 'Connected now'}", StringComparison.Ordinal);
         int pop3 = xaml.IndexOf("x:Name=\"Pop3Value\"", StringComparison.Ordinal);
         int banned = xaml.IndexOf("{loc:L 'Banned now'}", StringComparison.Ordinal);
         int value = xaml.IndexOf("x:Name=\"AutobanValue\"", StringComparison.Ordinal);
         Assert.True(connected > 0 && pop3 > connected && banned > pop3 && value > banned,
            "The auto-ban count is not under a line of its own after the session counts.");

         // The sessions' grid is closed before the count's line, and no grid holds the count.
         int sessionsGrid = xaml.LastIndexOf("<UniformGrid", pop3, StringComparison.Ordinal);
         int sessionsGridEnd = xaml.IndexOf("</UniformGrid>", pop3, StringComparison.Ordinal);
         Assert.True(sessionsGrid > connected && sessionsGridEnd > pop3 && sessionsGridEnd < banned,
            "The auto-ban count's line is inside the grid of the session counts.");
         Assert.True(xaml.LastIndexOf("<UniformGrid", value, StringComparison.Ordinal) < xaml.LastIndexOf("</UniformGrid>", value, StringComparison.Ordinal),
            "The auto-ban count is a cell of a grid of other figures.");
      }

      private static string ReadStatusViewXaml() => ReadStatusView("StatusView.xaml");

      private static string ReadStatusView(string file = "StatusView.xaml.cs")
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", file);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/" + file + " could not be found from the test binaries.");
         return null;
      }
   }
}
