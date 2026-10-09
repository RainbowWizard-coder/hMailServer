// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
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
         Assert.Contains("store_.IsAvailable && denied_ == null && selectionBaseline_.Differs(SelectionValues_())", unsaved, StringComparison.Ordinal);

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

      [Fact]
      public void SaveAndPreview_PreviewsOnlyWhatItSaved()
      {
         string both = Member("private async Task SaveAndPreviewAsync_()");
         int save = both.IndexOf("if (!Save_())", StringComparison.Ordinal);
         int run = both.IndexOf("await RunAsync_(false);", StringComparison.Ordinal);
         Assert.True(save > 0 && run > save, "Save and preview no longer saves first and previews only after a save that worked.");
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
