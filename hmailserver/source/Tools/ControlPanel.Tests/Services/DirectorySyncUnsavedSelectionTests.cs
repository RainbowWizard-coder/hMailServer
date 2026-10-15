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
   /// Directory synchronisation's Preview and Apply read the saved [LDAP] section, and
   /// ran while the selection editors right above them held changes not saved, so a
   /// preview of what had just been typed was a preview of the old selection (review
   /// finding F121). These hold that both refuse while the selection is unsaved, that
   /// the run card says so with Save and preview, and that the schedule, which a run
   /// does not read, does not count.
   /// </summary>
   public class DirectorySyncUnsavedSelectionTests
   {
      private static string Source() => File.ReadAllText(ControlPanelFile_(Path.Join("Views", "DirectorySyncView.cs")));

      private static string Member(string signature)
      {
         string body = WithheldRowTests.MemberText(Source(), signature);
         Assert.True(body != null, "DirectorySyncView: " + signature + " is gone.");
         return body;
      }

      [Fact]
      public void TheSelection_IsTheFiveEditorsARunReads_NotTheSchedule()
      {
         string values = Member("private IEnumerable<string> SelectionValues_()");
         foreach (string editor in new[] { "filter_.Text", "mailAttribute_.Text", "usernameAttribute_.Text", "displayNameAttribute_.Text", "maxUsers_.Text" })
            Assert.Contains(editor, values, StringComparison.Ordinal);
         Assert.DoesNotContain("scheduleMinutes_", values, StringComparison.Ordinal);

         string unsaved = Member("private bool SelectionUnsaved_");
         Assert.Contains("ServersFile_ && denied_ == null && selectionBaseline_.Differs(SelectionValues_())", unsaved, StringComparison.Ordinal);

         // Taken where the page's own record is: after a read and after a save.
         Assert.Contains("selectionBaseline_.Take(SelectionValues_());", Member("private void Load_()"), StringComparison.Ordinal);
         Assert.Contains("selectionBaseline_.Take(SelectionValues_());", Member("private bool Save_()"), StringComparison.Ordinal);
      }

      [Fact]
      public void WhileItIsUnsaved_PreviewAndApplyAreDisabled_AndTheCardSaysSoWithSaveAndPreview()
      {
         string follow = Member("private void FollowSelection_()");
         foreach (string step in new[]
         {
            "unsavedSelection_.Visibility = unsaved ? Visibility.Visible : Visibility.Collapsed;",
            "saveAndPreviewButton_.Visibility = unsaved ? Visibility.Visible : Visibility.Collapsed;",
            "previewButton_.IsEnabled = !unsaved && !running_;",
            "applyButton_.IsEnabled = !unsaved && !running_ && previewedOptions_ != null && previewedOptions_ == CurrentOptions_();"
         })
            Assert.True(follow.Contains(step, StringComparison.Ordinal), "FollowSelection_ no longer does " + step);
      }

      /// <summary>
      /// And previews it once the server has read it (found re-checking 729eeb5b1): the
      /// server reads the [LDAP] section again only two seconds after it last looked,
      /// which every directory sign-in does, so a preview started milliseconds after the
      /// save ran on the selection before it, while the page recorded the preview as one
      /// of the options it reads from the file and enabled Apply. A preview waits, the
      /// card busy, until two and a half seconds after the last save of either directory
      /// page, and reads what it is of after the wait.
      /// </summary>
      [Fact]
      public void SaveAndPreview_PreviewsOnlyWhatItSaved()
      {
         string both = Member("private async Task SaveAndPreviewAsync_()");
         int save = both.IndexOf("if (!Save_())", StringComparison.Ordinal);
         int run = both.IndexOf("await RunAsync_(false);", StringComparison.Ordinal);
         Assert.True(save > 0 && run > save, "Save and preview no longer saves first and previews only after a save that worked.");

         string written = Member("private bool Save_()");
         int flush = written.IndexOf("ProfileApi.Flush(store_.IniPath);", StringComparison.Ordinal);
         int noted = written.IndexOf("LdapReread.Saved();", StringComparison.Ordinal);
         int refused = written.IndexOf("catch (Exception ex)", StringComparison.Ordinal);
         Assert.True(flush > 0 && noted > flush && refused > noted, "Directory synchronisation's save no longer notes a save that worked, after the flush.");
         Assert.Contains("LdapReread.Saved();", File.ReadAllText(ControlPanelFile_(Path.Join("Views", "LdapSettingsView.cs"))), StringComparison.Ordinal);

         string runs = Member("private async Task RunAsync_(bool apply)");
         int busy = runs.IndexOf("running_ = true;", StringComparison.Ordinal);
         int waits = runs.IndexOf("while ((wait = LdapReread.Wait()) > TimeSpan.Zero)", StringComparison.Ordinal);
         int delay = runs.IndexOf("await Task.Delay(wait);", StringComparison.Ordinal);
         int reads = runs.IndexOf("string options = CurrentOptions_();", StringComparison.Ordinal);
         int calls = runs.IndexOf("await Task.Run(() => Invoke_(domain, apply, disableMissing));", StringComparison.Ordinal);
         Assert.True(busy >= 0 && waits > busy && delay > waits && reads > delay && calls > reads,
            "A preview no longer waits, the card busy, for the server to read the last save before it reads its options and runs.");
         Assert.Contains("if (!apply)", runs[busy..waits], StringComparison.Ordinal);
      }

      [Theory]
      [InlineData(null, 10_000L, 0)]        // nothing saved
      [InlineData(10_000L, 10_000L, 2500)]  // saved this instant
      [InlineData(10_000L, 10_400L, 2100)]
      [InlineData(10_000L, 12_499L, 1)]
      [InlineData(10_000L, 12_500L, 0)]     // the server has read it
      [InlineData(10_000L, 60_000L, 0)]
      [InlineData(10_000L, 9_000L, 2500)]   // never more than the window
      public void APreview_WaitsForTheServerToReadTheLastSave(long? savedAt, long now, int milliseconds)
         => Assert.Equal(TimeSpan.FromMilliseconds(milliseconds), LdapReread.Wait(savedAt, now));

      [Fact]
      public void TheWait_IsTheServersRecheckWindow_AndAMargin()
      {
         // LdapSettings.cpp: static const time_t kSettingsRecheckSeconds = 2;
         Assert.Equal(TimeSpan.FromMilliseconds(2500), LdapReread.AfterSave);
      }

      [Fact]
      public void ARun_LeavesTheButtonsAsTheSelectionStands_AndApplyRefusesAnUnsavedOne()
      {
         string run = Member("private async Task RunAsync_(bool apply)");
         Assert.Contains("running_ = true;", run, StringComparison.Ordinal);
         string end = run[run.LastIndexOf("finally", StringComparison.Ordinal)..];
         Assert.Contains("FollowSelection_();", end, StringComparison.Ordinal);
         Assert.DoesNotContain("previewButton_.IsEnabled = true;", run, StringComparison.Ordinal);

         string apply = Member("private async Task ApplyAsync_()");
         int refuses = apply.IndexOf("if (SelectionUnsaved_)", StringComparison.Ordinal);
         int asks = apply.IndexOf("MessageBox.Confirm(", StringComparison.Ordinal);
         Assert.True(refuses > 0 && asks > refuses, "Apply no longer refuses an unsaved selection before it asks.");
      }

      private static string ControlPanelFile_(string relative)
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return candidate;
         }

         throw new FileNotFoundException("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
      }
   }
}
