// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F249: Live logs streamed the newest file matching
   /// hmailserver_*.log - as often the AWStats, backup or events log as the day's - kept
   /// it past midnight while saying Streaming, showed every line with the file's quotes,
   /// and put every problem in the grey subtitle over an empty card. LiveLogFiles names
   /// the file the server writes today for each log, finds the next day's once the
   /// server has opened it, and splits a line into the parts the page shows; the page's
   /// own wiring is read from its source, as SelectableTextTests reads it.
   /// </summary>
   public class LiveLogFilesTests
   {
      private static readonly DateTime Day = new(2026, 10, 8, 23, 59, 0);

      [Theory]
      [InlineData(LiveLogKind.Everything, "hmailserver_2026-10-08.log")]
      [InlineData(LiveLogKind.Errors, "ERROR_hmailserver_2026-10-08.log")]
      [InlineData(LiveLogKind.Events, "hmailserver_events.log")]
      [InlineData(LiveLogKind.Backup, "hmailserver_backup.log")]
      [InlineData(LiveLogKind.Smtp, "hmailserver_SMTP_2026-10-08.log")]
      [InlineData(LiveLogKind.Imap, "hmailserver_IMAP_2026-10-08.log")]
      [InlineData(LiveLogKind.Pop3, "hmailserver_POP3_2026-10-08.log")]
      public void EachLog_IsTheFileTheServerWritesThatDay(LiveLogKind kind, string expected)
         => Assert.Equal(expected, LiveLogFiles.FileName(kind, Day));

      [Fact]
      public void TheNextDaysFile_IsTakenOnceTheServerHasOpenedIt()
      {
         string folder = Path.Join("C:", "Logs");
         string today = Path.Join(folder, "hmailserver_2026-10-08.log");
         string tomorrow = Path.Join(folder, "hmailserver_2026-10-09.log");

         // The same day: nothing to change.
         Assert.Null(LiveLogFiles.NextDay(today, LiveLogKind.Everything, Day, _ => true));

         // After midnight, before the server's first line of the day: the old file goes on.
         Assert.Null(LiveLogFiles.NextDay(today, LiveLogKind.Everything, Day.AddMinutes(2), _ => false));

         // Once it is there, it is the file.
         Assert.Equal(tomorrow, LiveLogFiles.NextDay(today, LiveLogKind.Everything, Day.AddMinutes(2), path => path == tomorrow));

         // A log that is not daily has no next day.
         string events = Path.Join(folder, "hmailserver_events.log");
         Assert.Null(LiveLogFiles.NextDay(events, LiveLogKind.Events, Day.AddDays(1), _ => true));
      }

      [Fact]
      public void ALineOfAConversation_LosesItsQuotes_AndItsRemoteHostLeadsTheText()
      {
         LiveLogFiles.Parts parts = LiveLogFiles.Split("\"SMTPD\"\t13252\t41\t\"2026-10-08 14:02:03.123\"\t\"203.0.113.5\"\t\"SENT: 220 mail.example.com ESMTP\"", LiveLogKind.Everything);
         Assert.Equal("14:02:03.123", parts.Time);
         Assert.Equal("SMTPD", parts.Type);
         Assert.Equal("13252", parts.Thread);
         Assert.Equal("41", parts.Session);
         Assert.Equal("203.0.113.5  SENT: 220 mail.example.com ESMTP", parts.Text);
      }

      [Fact]
      public void ALineWithoutASession_KeepsItsTextWhole()
      {
         LiveLogFiles.Parts parts = LiveLogFiles.Split("\"DEBUG\"\t25556\t\"2026-10-09 02:37:06.384\"\t\"AutoBanFirewall::Synchronise - expiry pass: 0 active\"", LiveLogKind.Everything);
         Assert.Equal("02:37:06.384", parts.Time);
         Assert.Equal("DEBUG", parts.Type);
         Assert.Equal("25556", parts.Thread);
         Assert.Equal("", parts.Session);
         Assert.Equal("AutoBanFirewall::Synchronise - expiry pass: 0 active", parts.Text);
      }

      [Fact]
      public void TheEventsAndBackupLogs_KeepTheirDates_AndTheirOwnShapes()
      {
         LiveLogFiles.Parts events = LiveLogFiles.Split("4120\t\"2026-10-08 09:00:00.000\"\t\"Script wrote this\"", LiveLogKind.Events);
         Assert.Equal("2026-10-08 09:00:00.000", events.Time);
         Assert.Equal("4120", events.Thread);
         Assert.Equal("Script wrote this", events.Text);

         LiveLogFiles.Parts backup = LiveLogFiles.Split("2026-10-08 02:00:00.000\tBackup started", LiveLogKind.Backup);
         Assert.Equal("2026-10-08 02:00:00.000", backup.Time);
         Assert.Equal("Backup started", backup.Text);
      }

      [Theory]
      [InlineData("{\"type\":\"SMTPD\",\"message\":\"x\"}")]
      [InlineData("203.0.113.5 - - [08/Oct/2026:14:02:03 +0000] \"EHLO\" 250")]
      [InlineData("\"ERROR\"\tonly two")]
      public void ALineOfAnotherShape_IsShownWhole(string line)
      {
         LiveLogFiles.Parts parts = LiveLogFiles.Split(line, LiveLogKind.Everything);
         Assert.Equal(line, parts.Text);
         Assert.Equal("", parts.Time);
         Assert.Equal("", parts.Type);
      }

      [Fact]
      public void ThePage_SaysItsProblemsInTheListsPlace_AndFollowsTheDay()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "LogsView.xaml.cs"));
         string xaml = ReadControlPanelFile(Path.Join("Views", "LogsView.xaml"));

         // Today's file of the chosen log, never the newest match of a wildcard.
         Assert.DoesNotContain("GetFiles(\"hmailserver_*.log\")", code, StringComparison.Ordinal);
         Assert.Contains("LiveLogFiles.FileName(kind_, DateTime.Today)", code, StringComparison.Ordinal);
         Assert.Contains("LiveLogFiles.NextDay(logFile_, kind_, DateTime.Today, File.Exists)", code, StringComparison.Ordinal);
         Assert.Contains("… continued in {0}", code, StringComparison.Ordinal);

         // Problems in the list's place, not in the subtitle; Logging's page where that cures it.
         Assert.Contains("<scaffold:EmptyState x:Name=\"Problem\"", xaml, StringComparison.Ordinal);
         Assert.DoesNotContain("Header.Subtitle = F(\"Could not read the file", code, StringComparison.Ordinal);
         Assert.Contains("NavigateTo(\"logging\")", code, StringComparison.Ordinal);

         // Paused says since when and how many lines wait; Clear says the file is untouched.
         Assert.Contains("Paused at {0}: {1} new lines of {2} are waiting.", code, StringComparison.Ordinal);
         Assert.Contains("Content=\"{loc:L '_Clear view'}\"", xaml, StringComparison.Ordinal);
         Assert.Contains("The log file is not changed.", xaml, StringComparison.Ordinal);

         // The picker of logs, the protocols' own while the server writes them apart.
         Assert.Contains("x:Name=\"LogPicker\"", xaml, StringComparison.Ordinal);
         Assert.Contains("store.ReadBool(\"SepSvcLogs\", false)", code, StringComparison.Ordinal);
      }

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
