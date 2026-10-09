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
   /// Review finding F257, the Backup &amp; restore page. Every outcome went to one
   /// notice above the first card, a screen or more above the restore cards' buttons,
   /// and only Save settings raised a toast; a mailbox restore's outcome printed the
   /// server's raw state token, and its JSON when it could not be read; the restores
   /// wanted an archive's path or name and a message's id typed by hand, the id
   /// copied out of a monospace text box; the browse buttons opened the pickers of
   /// whichever machine the Control Panel ran on; and "No passphrase is set: the
   /// archive is not encrypted…" was an Information notice. These hold the archive
   /// list, the outcome in words at its level, the messages as rows, and the page's
   /// shape: an outcome notice by the buttons of each card, a toast for each job
   /// started, the browse buttons only on the server itself, the warning at Warning.
   /// </summary>
   public class BackupRestoreTests
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";
      private static readonly XNamespace Scaffold = "clr-namespace:hMailServer.ControlPanel.Views.Scaffold";

      [Theory]
      [InlineData("HMBackup 2026-10-08 020000.7z", true)]
      [InlineData("hmbackup 2026-10-08 235959.7Z", true)]
      [InlineData("HMBackup 2026-02-30 020000.7z", false)]   // no such day
      [InlineData("HMBackup 2026-10-08 020000.zip", false)]
      [InlineData("HMBackup 2026-10-8 020000.7z", false)]
      [InlineData("HMBackup 2026-10-08 02000.7z", false)]
      [InlineData("my copy of HMBackup 2026-10-08 020000.7z", false)]
      [InlineData(null, false)]
      public void AnArchive_IsANameTheServerWrites(string name, bool archive)
         => Assert.Equal(archive, BackupRestoreReport.TryParseArchiveName(name, out _));

      [Fact]
      public void TheArchives_AreTheServersOwnNames_NewestFirst_WithTheirPathAndSize()
      {
         List<BackupRestoreReport.Archive> archives = BackupRestoreReport.Archives(@"D:\Backups\hMailServer\", new[]
         {
            ("HMBackup 2026-10-06 020000.7z", 1_572_864L),
            ("notes.txt", 12L),
            ("HMBackup 2026-10-08 020000.7z", 1_048_576L),
            ("HMBackup 2026-10-07 020000.7z", 3L),
            ("HMBackup 2026-10-07 020000.7z.part", 99L),
         });

         Assert.Equal(new[] { "HMBackup 2026-10-08 020000.7z", "HMBackup 2026-10-07 020000.7z", "HMBackup 2026-10-06 020000.7z" }, archives.Select(a => a.Name));
         Assert.Equal(@"D:\Backups\hMailServer\HMBackup 2026-10-08 020000.7z", archives[0].Path);
         Assert.Equal(new DateTime(2026, 10, 8, 2, 0, 0), archives[0].Taken);
         Assert.Equal(1_048_576L, archives[0].Size);
         Assert.Empty(BackupRestoreReport.Archives("D:\\", null));
      }

      [Fact]
      public void ASize_IsInMegabytesToOnePlace_NeverNoneForAFileWithBytes()
      {
         Assert.Equal(0.0.ToString("N1", CultureInfo.CurrentCulture), BackupRestoreReport.Megabytes(0));
         Assert.Equal(0.1.ToString("N1", CultureInfo.CurrentCulture), BackupRestoreReport.Megabytes(3));
         Assert.Equal(1.5.ToString("N1", CultureInfo.CurrentCulture), BackupRestoreReport.Megabytes(1_572_864));
      }

      [Theory]
      [InlineData("""{"job":7,"state":"queued","folders":0,"added":0,"skipped":0,"failed":0,"error":""}""", StatusLevel.Information, "is queued")]
      [InlineData("""{"job":7,"state":"running","folders":3,"added":40,"skipped":2,"failed":0,"error":""}""", StatusLevel.Information, "is running")]
      [InlineData("""{"job":7,"state":"done","folders":3,"added":41,"skipped":2,"failed":0,"error":""}""", StatusLevel.Good, "is done.")]
      [InlineData("""{"job":7,"state":"done","folders":3,"added":39,"skipped":2,"failed":2,"error":""}""", StatusLevel.Warning, "could not be put back")]
      [InlineData("""{"job":7,"state":"failed","error":"No such account in the archive."}""", StatusLevel.Critical, "No such account in the archive.")]
      [InlineData("""{"job":7,"state":"failed","error":""}""", StatusLevel.Critical, "The backup log says why.")]
      [InlineData("""{"job":7,"state":"paused"}""", StatusLevel.Warning, "does not know")]
      public void AJobsOutcome_IsAtItsLevel_InWords(string json, StatusLevel level, string words)
      {
         BackupRestoreReport.JobOutcome outcome = BackupRestoreReport.Job(json);
         Assert.Equal(level, outcome.Level);
         Assert.Equal(7, outcome.Job);
         Assert.Contains(words, outcome.Text, StringComparison.Ordinal);
         Assert.Contains("7", outcome.Text, StringComparison.Ordinal);
         Assert.DoesNotContain("{", outcome.Text, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("")]
      [InlineData("Job 7 is running")]
      [InlineData("[1,2,3]")]
      public void AnAnswerThatCannotBeRead_IsSaidSo_NeverShownAsItCame(string json)
      {
         BackupRestoreReport.JobOutcome outcome = BackupRestoreReport.Job(json);
         Assert.Equal(StatusLevel.Critical, outcome.Level);
         Assert.Equal("The server's answer about the restore could not be read.", outcome.Text);
      }

      [Fact]
      public void TheArchivedMessages_AreRowsToChooseFrom_WithTheServersNote()
      {
         const string json = """
            {"archive":"HMBackup 2026-10-08 020000.7z","address":"jane@example.com","folders":[
              {"path":"INBOX","messages":[{"id":1042,"folder":"INBOX","subject":"Quarterly figures","from":"cfo@example.com","date":"2026-10-07 09:15:00","size":2048}]},
              {"path":"Sent","messages":[{"id":1043,"folder":"Sent","subject":"Re: Quarterly figures","from":"jane@example.com","date":"not a date","size":1024}]}],
             "messages":2,"subjects":true,"subjects_note":"Subjects are read for a listing of at most 500 messages."}
            """;
         List<BackupRestoreReport.ArchivedMessage> messages = BackupRestoreReport.ArchivedMessages(json, out string note, out string error);
         Assert.Null(error);
         Assert.Equal(new long[] { 1042, 1043 }, messages.Select(m => m.Id));
         Assert.Equal("Quarterly figures", messages[0].Subject);
         Assert.Equal("Sent", messages[1].Folder);
         Assert.Equal(new DateTime(2026, 10, 7, 9, 15, 0).ToString("g", CultureInfo.CurrentCulture), messages[0].Date);
         Assert.Equal("not a date", messages[1].Date);
         Assert.Equal("Subjects are read for a listing of at most 500 messages.", note);
         Assert.StartsWith("Quarterly figures", messages[0].ToString(), StringComparison.Ordinal);

         Assert.Null(BackupRestoreReport.ArchivedMessages("{", out _, out string unread));
         Assert.Equal("The server's answer about the archive could not be read.", unread);
      }

      // ---- the page -----------------------------------------------------------------------

      /// <summary>Each card says what its buttons did, in its own footer, beside them.</summary>
      [Theory]
      [InlineData("BackupOutcome", "StartBackup_Click")]
      [InlineData("ScheduleOutcome", "SaveSchedule_Click")]
      [InlineData("RestoreOutcome", "StartRestore_Click")]
      [InlineData("MailboxOutcome", "RestoreMailbox_Click")]
      [InlineData("MessageOutcome", "RestoreArchivedMessage_Click")]
      public void EachCard_SaysItsOutcomeInItsFooter_BesideItsButtons(string outcome, string button)
      {
         XElement page = XDocument.Parse(Read("BackupView.xaml")).Root;
         XElement notice = page.Descendants(Scaffold + "InlineNotice").SingleOrDefault(e => (string)e.Attribute(X + "Name") == outcome);
         Assert.True(notice != null, "BackupView.xaml has no notice named " + outcome + ".");
         Assert.Equal("Collapsed", (string)notice.Attribute("Visibility"));

         XElement footer = notice.Ancestors(Scaffold + "Card.Footer").FirstOrDefault();
         Assert.True(footer != null, outcome + " is not in a card's footer, by the buttons that cause it.");
         Assert.Contains(footer.Descendants(), e => (string)e.Attribute("Click") == button);
      }

      [Fact]
      public void NoOutcomeIsSaid_InTheNoticeAboveTheFirstCard()
      {
         string code = Read("BackupView.xaml.cs");
         Assert.DoesNotContain("ShowNotice_(", code, StringComparison.Ordinal);
         Assert.DoesNotContain("Notice.Text =", code.Replace("EncryptionNotice.Text =", "", StringComparison.Ordinal), StringComparison.Ordinal);
      }

      /// <summary>Every job started raises a toast, as Save settings and Save schedule did.</summary>
      [Theory]
      [InlineData("private void StartBackup_Click(object sender, RoutedEventArgs e)")]
      [InlineData("private void StartRestore_Click(object sender, RoutedEventArgs e)")]
      [InlineData("private void RestoreMailbox_Click(object sender, RoutedEventArgs e)")]
      [InlineData("private void RestoreArchivedMessage_Click(object sender, RoutedEventArgs e)")]
      public void EveryJobStarted_RaisesAToast(string member)
      {
         string body = WithheldRowTests.MemberText(Read("BackupView.xaml.cs"), member);
         Assert.True(body != null, "BackupView: " + member + " is gone.");
         Assert.Contains("Toast.Success(", body, StringComparison.Ordinal);
      }

      [Fact]
      public void AJobsOutcome_IsSaidInWords_NeverAsTheServersTokenOrJson()
      {
         string code = Read("BackupView.xaml.cs");
         string show = WithheldRowTests.MemberText(code, "private void ShowJobOutcome_(InlineNotice outcome, string json)");
         Assert.True(show != null, "BackupView.ShowJobOutcome_ is gone.");
         Assert.Contains("BackupRestoreReport.Job(json)", show, StringComparison.Ordinal);
         Assert.DoesNotContain("JsonDocument", code, StringComparison.Ordinal);
         Assert.DoesNotContain("MailboxRestoreOutcome", code, StringComparison.Ordinal);
      }

      [Fact]
      public void NoPassphrase_IsAWarning()
      {
         string show = WithheldRowTests.MemberText(Read("BackupView.xaml.cs"), "private void ShowEncryptionState_()");
         Assert.True(show != null, "BackupView.ShowEncryptionState_ is gone.");
         Assert.Contains("StatusLevel.Good : StatusLevel.Warning", show, StringComparison.Ordinal);
      }

      [Fact]
      public void TheBrowseButtons_BrowseOnlyTheServersOwnDisks()
      {
         string enter = WithheldRowTests.MemberText(Read("BackupView.xaml.cs"), "public void OnEnter()");
         Assert.True(enter != null, "BackupView.OnEnter is gone.");
         Assert.Contains("bool local = ServerSession.IsLocalSession;", enter, StringComparison.Ordinal);
         Assert.Contains("browse.IsEnabled = local;", enter, StringComparison.Ordinal);
         Assert.Contains("ToolTipService.SetShowOnDisabled(browse, true);", enter, StringComparison.Ordinal);
      }

      /// <summary>
      /// The archive list feeds the three restore cards, and the one message put back
      /// is the one chosen in a grid: nothing is typed out of a listing any more.
      /// </summary>
      [Fact]
      public void TheArchiveChosen_FillsTheRestoreCards_AndTheMessageIsChosenInAGrid()
      {
         XElement page = XDocument.Parse(Read("BackupView.xaml")).Root;
         Assert.Contains(page.Descendants(Wpf + "DataGrid"), g => (string)g.Attribute(X + "Name") == "ArchiveGrid");
         Assert.Contains(page.Descendants(Wpf + "DataGrid"), g => (string)g.Attribute(X + "Name") == "ArchivedMessageGrid");
         Assert.DoesNotContain(page.Descendants(), e => (string)e.Attribute(X + "Name") is "ArchivedMessagesBox" or "ArchivedMessageIdBox" or "MailboxRestoreOutcome");
         Assert.DoesNotContain("HMBackup*.xml", Read("BackupView.xaml"), StringComparison.Ordinal);

         string code = Read("BackupView.xaml.cs");
         string chosen = WithheldRowTests.MemberText(code, "private void ArchiveGrid_SelectionChanged(object sender, SelectionChangedEventArgs e)");
         Assert.True(chosen != null, "BackupView.ArchiveGrid_SelectionChanged is gone.");
         Assert.Contains("BackupFileBox.Text = archive.Path;", chosen, StringComparison.Ordinal);
         Assert.Contains("MailboxArchiveBox.Text = archive.Name;", chosen, StringComparison.Ordinal);

         Assert.Contains("SelectionGate.Bind(ArchivedMessageGrid, PutMessageBackButton);", code, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"RefreshScheduleStatus\(\)\s*\{[\s\S]*ShowArchives_\(destination\);"), code);
      }

      private static string Read(string file)
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
