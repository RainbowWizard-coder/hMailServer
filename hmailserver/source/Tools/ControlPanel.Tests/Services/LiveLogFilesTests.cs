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
         Assert.Contains("LiveLogFiles.NextDay(logFile_, kind_, DateTime.Today, path => LiveLogFiles.Access(path) != LiveLogAccess.NotThere)", code, StringComparison.Ordinal);
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

      // ---- what a line is about, and the filter (review finding F250) --------------------

      [Theory]
      [InlineData("\"SMTPD\"\t13252\t41\t\"2026-10-08 14:02:03.123\"\t\"203.0.113.5\"\t\"SENT: 220 mail.example.com ESMTP\"", LiveLogLineKind.Smtp)]
      [InlineData("\"SMTPC\"\t13252\t42\t\"2026-10-08 14:02:03.123\"\t\"198.51.100.7\"\t\"RECEIVED: 250 OK\"", LiveLogLineKind.Smtp)]
      [InlineData("\"IMAPD\"\t13252\t43\t\"2026-10-08 14:02:03.123\"\t\"203.0.113.5\"\t\"SENT: * OK IMAPrev1\"", LiveLogLineKind.Imap)]
      [InlineData("\"POP3D\"\t13252\t44\t\"2026-10-08 14:02:03.123\"\t\"203.0.113.5\"\t\"SENT: +OK POP3\"", LiveLogLineKind.Pop3)]
      [InlineData("\"POP3C\"\t13252\t45\t\"2026-10-08 14:02:03.123\"\t\"198.51.100.9\"\t\"RECEIVED: +OK\"", LiveLogLineKind.Pop3)]
      [InlineData("\"APPLICATION\"\t13252\t\"2026-10-08 14:02:03.123\"\t\"hMailServer: Delivery task started.\"", LiveLogLineKind.Application)]
      [InlineData("\"DEBUG\"\t13252\t\"2026-10-08 14:02:03.123\"\t\"Creating session 41\"", LiveLogLineKind.Debug)]
      [InlineData("\"ERROR\"\t13252\t\"2026-10-08 14:02:03.123\"\t\"Severity: 3 (Medium), Code: HM5155\"", LiveLogLineKind.Error)]
      [InlineData("\"APPLICATION\"\t13252\t\"2026-10-08 14:02:03.123\"\t\"Severity: 2 (High), Code: HM4229\"", LiveLogLineKind.Error)]
      [InlineData("\"TCPIP\"\t13252\t\"2026-10-08 14:02:03.123\"\t\"TCP - 203.0.113.5 connected to 0.0.0.0:25.\"", LiveLogLineKind.Other)]
      [InlineData("{\"type\":\"IMAPD\",\"thread\":13252,\"text\":\"SENT: * OK\"}", LiveLogLineKind.Imap)]
      [InlineData("Backup started at 14:02", LiveLogLineKind.Other)]
      public void ALine_IsOfItsType_TheServerAndTheClientOfAProtocolAlike(string line, LiveLogLineKind expected)
         => Assert.Equal(expected, LiveLogFiles.KindOf(line, LiveLogFiles.Split(line, LiveLogKind.Everything)));

      [Fact]
      public void TheFilter_KeepsTheLinesThatHoldEveryWord_AndSessionNamesTheSessionExactly()
      {
         const string line = "\"SMTPD\"\t13252\t42\t\"2026-10-08 14:02:03.123\"\t\"203.0.113.5\"\t\"RECEIVED: MAIL FROM:<Jane@Example.com>\"";

         Assert.True(LiveLogFiles.MatchesFilter("", line, "42"));
         Assert.True(LiveLogFiles.MatchesFilter("   ", line, "42"));
         Assert.True(LiveLogFiles.MatchesFilter("jane@example.com", line, "42"));
         Assert.True(LiveLogFiles.MatchesFilter("MAIL  jane", line, "42"));
         Assert.False(LiveLogFiles.MatchesFilter("MAIL bob", line, "42"));

         // session=42 is the session, not every line with a 42 in its time or thread.
         Assert.True(LiveLogFiles.MatchesFilter("session=42", line, "42"));
         Assert.True(LiveLogFiles.MatchesFilter("Session=42 jane", line, "42"));
         Assert.False(LiveLogFiles.MatchesFilter("session=4", line, "42"));
         Assert.False(LiveLogFiles.MatchesFilter("session=42", line, ""));
         Assert.True(LiveLogFiles.MatchesFilter("42", line, "7"));
      }

      /// <summary>
      /// The page narrows the lines to what is being followed: a filter over them, a
      /// box per line type drawn in that type's colour - the colours' legend - a way
      /// to wrap the long lines, the count of the lines shown beside the title, and
      /// the newest line clear of the sideways scroll bar. Only Pause and Clear view
      /// were there (review finding F250).
      /// </summary>
      [Fact]
      public void ThePage_FiltersTheLines_ByWordsAndByType_AndCountsTheLinesShown()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "LogsView.xaml.cs"));
         string xaml = ReadControlPanelFile(Path.Join("Views", "LogsView.xaml"));

         Assert.Contains("<scaffold:Toolbar x:Name=\"LineFilter\"", xaml, StringComparison.Ordinal);
         Assert.Contains("SearchTextChanged=\"LineFilter_SearchTextChanged\"", xaml, StringComparison.Ordinal);

         // A box per type, each in its lines' colour, so that the boxes are the legend.
         foreach ((string caption, string kind, string brush) in new[]
         {
            ("_SMTP", "Smtp", "LogSmtpBrush"), ("_IMAP", "Imap", "LogImapBrush"), ("POP_3", "Pop3", "LogPop3Brush"),
            ("_Application", "Application", "LogAppBrush"), ("_Debug", "Debug", "LogDefaultBrush"), ("E_rrors", "Error", "LogErrorBrush")
         })
         {
            Assert.Contains("Content=\"{loc:L '" + caption + "'}\" IsChecked=\"True\" Tag=\"" + kind + "\" Click=\"TypeBox_Click\"", xaml, StringComparison.Ordinal);
            Assert.Contains("Foreground=\"{DynamicResource " + brush + "}\"", xaml, StringComparison.Ordinal);
            Assert.True(Enum.TryParse(kind, out LiveLogLineKind _), kind + " is no line kind.");
         }

         // The lines are a view of those held, filtered as they arrive.
         Assert.Contains("shown_.Filter = line => Shows_((LogLine)line);", code, StringComparison.Ordinal);
         Assert.Contains("LogList.ItemsSource = shown_;", code, StringComparison.Ordinal);
         Assert.Contains("!hidden_.Contains(line.Kind) && LiveLogFiles.MatchesFilter(filter_, line.Text, line.Session)", code, StringComparison.Ordinal);
         Assert.Contains("Kind = kind,", code, StringComparison.Ordinal);
         Assert.Contains("Brush = BrushOf(kind),", code, StringComparison.Ordinal);

         // Wrap lines folds the text at the list's width and stops the sideways scrolling.
         Assert.Contains("Content=\"{loc:L '_Wrap lines'}\"", xaml, StringComparison.Ordinal);
         Assert.Contains("<Setter TargetName=\"MessageText\" Property=\"TextWrapping\" Value=\"Wrap\" />", xaml, StringComparison.Ordinal);
         Assert.Contains("ScrollViewer.SetHorizontalScrollBarVisibility(LogList, wrap ? ScrollBarVisibility.Disabled : ScrollBarVisibility.Auto);", code, StringComparison.Ordinal);

         // The count beside the title, and the newest line kept clear of the sideways bar.
         Assert.Contains("<scaffold:StatusPill x:Name=\"LineCount\" Level=\"Normal\"", xaml, StringComparison.Ordinal);
         Assert.DoesNotContain("LiveSetting", xaml.Substring(xaml.IndexOf("x:Name=\"LineCount\"", StringComparison.Ordinal), 200), StringComparison.Ordinal);
         Assert.Contains("F(\"Shown: {0} of {1}\", shown, total)", code, StringComparison.Ordinal);
         Assert.Contains("scroller.ComputedHorizontalScrollBarVisibility == Visibility.Visible ? DesignTokens.Space.Md : 0", code, StringComparison.Ordinal);
      }

      [Fact]
      public void FollowingTheNewestLine_ScrollsTheLogAlone_NotThePage()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "LogsView.xaml.cs"));
         string xaml = ReadControlPanelFile(Path.Join("Views", "LogsView.xaml"));

         // The log is inside the page's own scroller in the narrowest window, and the
         // list's ScrollIntoView brings a line into view through every scroller above
         // it: the page followed the log, the filter box and Wrap lines pushed off its
         // top at every line streamed (review finding F250).
         Assert.Contains("<ScrollViewer x:Name=\"PageScroll\"", xaml, StringComparison.Ordinal);
         Assert.DoesNotContain("LogList.ScrollIntoView(", code, StringComparison.Ordinal);
         Assert.Contains("if (Scroller_() is ScrollViewer scroller)\r\n            scroller.ScrollToBottom();", code.Replace("\r\n", "\n").Replace("\n", "\r\n"), StringComparison.Ordinal);

         // The streamed lines and the filter applied again both follow the end that way.
         Assert.Contains("if (LiveLogTail.Follows(atEnd, read.Count))\r\n         {\r\n            FollowEnd_();", code.Replace("\r\n", "\n").Replace("\n", "\r\n"), StringComparison.Ordinal);
         Assert.Contains("if (atEnd)\r\n            FollowEnd_();", code.Replace("\r\n", "\n").Replace("\n", "\r\n"), StringComparison.Ordinal);
      }

      // ---- whether a log may be read ---------------------------------------------------

      [Fact]
      public void ALogThere_IsReadable_AndOneNotThereYet_OrInNoFolder_IsNotThere()
      {
         string folder = Path.Join(Path.GetTempPath(), "cp-livelog-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(folder);
         try
         {
            string today = Path.Join(folder, "hmailserver_2026-10-09.log");
            File.WriteAllText(today, "line\r\n");

            Assert.Equal(LiveLogAccess.Readable, LiveLogFiles.TryOpen(today, out FileStream stream));
            using (stream)
               Assert.Equal(6, stream.Length);
            Assert.Equal(LiveLogAccess.Readable, LiveLogFiles.FolderAccess(folder));

            Assert.Equal(LiveLogAccess.NotThere, LiveLogFiles.TryOpen(Path.Join(folder, "hmailserver_2026-10-10.log"), out FileStream none));
            Assert.Null(none);
            Assert.Equal(LiveLogAccess.NotThere, LiveLogFiles.Access(Path.Join(folder, "nowhere", "x.log")));
            Assert.Equal(LiveLogAccess.NotThere, LiveLogFiles.FolderAccess(Path.Join(folder, "nowhere")));
            Assert.Equal(LiveLogAccess.NotThere, LiveLogFiles.FolderAccess(today));   // a file, not a folder
            Assert.Equal(LiveLogAccess.NotThere, LiveLogFiles.FolderAccess(null));
         }
         finally
         {
            Directory.Delete(folder, true);
         }
      }

      /// <summary>
      /// A log folder that belongs to others, as hMailServer.exe /ProtectFolders leaves
      /// the log folder for a Control Panel not run as administrator: opening the file
      /// and listing the folder say that they may not be read, where File.Exists found
      /// no file there and said nothing more (on server1, run without elevation against
      /// a folder of the administrators' and SYSTEM's alone, File.Exists and
      /// FileInfo.Exists were false and the open raised UnauthorizedAccessException),
      /// which is how the page came to say Streaming over a log it could no longer
      /// read. The folder is denied to this process's own account here, so that the
      /// test needs no second account and no elevation.
      /// </summary>
      [Fact]
      public void ALogInAFolderThatBelongsToOthers_IsDenied()
      {
         string folder = Path.Join(Path.GetTempPath(), "cp-livelog-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(folder);
         string today = Path.Join(folder, "hmailserver_2026-10-09.log");
         File.WriteAllText(today, "line\r\n");

         var account = System.Security.Principal.WindowsIdentity.GetCurrent().User;
         var deny = new System.Security.AccessControl.FileSystemAccessRule(account,
            System.Security.AccessControl.FileSystemRights.ListDirectory | System.Security.AccessControl.FileSystemRights.ReadData | System.Security.AccessControl.FileSystemRights.ReadAttributes,
            System.Security.AccessControl.InheritanceFlags.ContainerInherit | System.Security.AccessControl.InheritanceFlags.ObjectInherit,
            System.Security.AccessControl.PropagationFlags.None,
            System.Security.AccessControl.AccessControlType.Deny);
         var directory = new DirectoryInfo(folder);
         System.Security.AccessControl.DirectorySecurity security = directory.GetAccessControl();
         security.AddAccessRule(deny);
         directory.SetAccessControl(security);
         try
         {
            bool bypassed;
            try
            {
               File.ReadAllBytes(today);
               bypassed = true;
            }
            catch (UnauthorizedAccessException)
            {
               bypassed = false;
            }

            Assert.SkipWhen(bypassed, "This account reads the file past a deny entry of its own, so the folder cannot be made another's here.");

            Assert.Equal(LiveLogAccess.Denied, LiveLogFiles.TryOpen(today, out FileStream stream));
            Assert.Null(stream);
            Assert.Equal(LiveLogAccess.Denied, LiveLogFiles.Access(today));

            // Listing the folder opens it for backup, which an account with the backup
            // privilege enabled does past the deny entry; the Control Panel cannot.
            if (!TokenPrivileges.IsEnabled("SeBackupPrivilege"))
               Assert.Equal(LiveLogAccess.Denied, LiveLogFiles.FolderAccess(folder));

            // A file not there yet is not there, in such a folder too: the page says
            // that the folder may not be read, from its listing.
            Assert.Equal(LiveLogAccess.NotThere, LiveLogFiles.Access(Path.Join(folder, "hmailserver_2026-10-10.log")));
         }
         finally
         {
            security = directory.GetAccessControl();
            security.RemoveAccessRuleSpecific(deny);
            directory.SetAccessControl(security);
            Directory.Delete(folder, true);
         }
      }

      /// <summary>
      /// The page opens the log, and looks for neither the folder nor the file first:
      /// Directory.Exists, File.Exists and FileInfo.Exists answer false, and raise
      /// nothing, where they may not look, so a gate ahead of the open kept the page
      /// from ever saying that the log may not be read.
      /// </summary>
      [Fact]
      public void ThePage_OpensTheLog_AndSaysWhenItMayNotBeRead()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "LogsView.xaml.cs"));
         Assert.DoesNotContain("File.Exists(", code, StringComparison.Ordinal);
         Assert.DoesNotContain("File.Exists)", code, StringComparison.Ordinal);
         Assert.DoesNotContain("Directory.Exists(", code, StringComparison.Ordinal);
         Assert.DoesNotContain("new FileInfo(", code, StringComparison.Ordinal);

         string poll = WithheldRowTests.MemberText(code, "private void Poll()");
         Assert.True(poll != null, "LogsView.Poll is gone.");
         int open = poll.IndexOf("switch (LiveLogFiles.TryOpen(logFile_, out FileStream stream))", StringComparison.Ordinal);
         int denied = poll.IndexOf("case LiveLogAccess.Denied:", StringComparison.Ordinal);
         int read = poll.IndexOf("Show_(tail_.Read(stream));", StringComparison.Ordinal);
         Assert.True(open >= 0 && denied > open && read > denied, "Poll no longer opens the log and says Denied before it reads.");
         Assert.Contains("Denied_();", poll.Substring(denied), StringComparison.Ordinal);

         string start = WithheldRowTests.MemberText(code, "private void Open_(string denied, string folder)");
         Assert.True(start != null, "LogsView.Open_ is gone.");
         Assert.Contains("LiveLogFiles.FolderAccess(folder)", start, StringComparison.Ordinal);
         Assert.Contains("switch (LiveLogFiles.Access(logFile_))", start, StringComparison.Ordinal);
         Assert.Contains("Denied_();", start, StringComparison.Ordinal);

         // And no Streaming left in the subtitle over a log no longer read.
         string problem = WithheldRowTests.MemberText(code, "private void Problem_(string text, bool offerLogging)");
         Assert.True(problem != null, "LogsView.Problem_ is gone.");
         Assert.Contains("Header.Subtitle = L(\"The server log as it is written, read from the log folder on this computer.\");", problem, StringComparison.Ordinal);
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
