// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Setting profiles page names a profile by its title and keeps its acts
   /// where they apply (review finding F278). "The small-office profile is
   /// applied." said the profile's key - in German and Japanese as well - where the
   /// list, the checklist and the other confirmations said "Small office", and the
   /// Apply and Put back questions and the outcome lines used the key too; the
   /// 430-pixel list and five buttons wrapped onto two rows at 1280 and three at
   /// 760, with "Keep its settings, forget the way back" beside "Keep the shipped
   /// settings"; Refresh was in the toolbar where the sibling pages have it in the
   /// header; the list's items were sentences; and the column "Asks for a
   /// confirmation" held notes that were not confirmations. Scanned rather than
   /// rendered: this assembly has no WPF. Each test fails against the page before
   /// the change.
   /// </summary>
   public class ProfilesPageFlowTests
   {
      [Fact]
      public void TheAppliedNotice_TheQuestionsAndTheOutcomes_NameTheProfileByItsTitle()
      {
         string source = ReadView();
         Assert.Contains("private string TitleOf_(string name)", source, StringComparison.Ordinal);

         string applied = WithheldRowTests.MemberText(source, "private void ShowApplied_(JsonElement root)");
         Assert.Contains("TitleOf_(snapshotProfile_)", applied, StringComparison.Ordinal);
         Assert.Contains("string name = TitleOf_(Text_(applied, \"profile\"));", applied, StringComparison.Ordinal);

         string apply = WithheldRowTests.MemberText(source, "private void ApplySelected()");
         Assert.DoesNotContain("choice.Name, changes_", apply, StringComparison.Ordinal);
         Assert.Contains("choice.Title, changes_", apply, StringComparison.Ordinal);
         Assert.Contains("for a restart: {5}.\", choice.Title,", apply, StringComparison.Ordinal);

         string revert = WithheldRowTests.MemberText(source, "private void RevertApplied()");
         Assert.Contains("string name = TitleOf_(snapshotProfile_);", revert, StringComparison.Ordinal);
         Assert.DoesNotContain("profile replaced? A setting changed since is left as it is, and a filter the profile turned on may be turned off again.\", snapshotProfile_)", revert, StringComparison.Ordinal);
      }

      [Fact]
      public void RefreshIsInTheHeader_AndTheActsOnWhatIsAppliedAreTheNoticesOwn()
      {
         string source = ReadView();
         string build = WithheldRowTests.MemberText(source, "private void Build()");
         Assert.True(build != null, "ProfilesView.Build is gone.");
         Assert.Contains("Actions = MakeButton(L(\"_Refresh\")", build, StringComparison.Ordinal);
         Assert.Contains("applied_.Content = appliedActs;", build, StringComparison.Ordinal);
         foreach (string act in new[] { "appliedActs.Children.Add(revert_);", "appliedActs.Children.Add(discard_);", "appliedActs.Children.Add(decline_);" })
            Assert.Contains(act, build, StringComparison.Ordinal);

         // Beside the list, the list and its one act.
         Assert.DoesNotContain("actions.Children.Add(revert_);", build, StringComparison.Ordinal);
         Assert.DoesNotContain("actions.Children.Add(discard_);", build, StringComparison.Ordinal);
         Assert.DoesNotContain("actions.Children.Add(decline_);", build, StringComparison.Ordinal);
         Assert.DoesNotContain("actions.Children.Add(MakeButton(L(\"_Refresh\")", build, StringComparison.Ordinal);
      }

      [Fact]
      public void TheTwoKeeps_AreNeverSideBySide_AndTheDiscardSaysWhatItDiscards()
      {
         string source = ReadView();
         Assert.DoesNotContain("forget the way back", source, StringComparison.Ordinal);
         Assert.Contains("L(\"_Discard the previous settings\")", source, StringComparison.Ordinal);

         // Each act is shown while it can be taken: discarding with a way back held,
         // keeping the shipped settings with none.
         string applied = WithheldRowTests.MemberText(source, "private void ShowApplied_(JsonElement root)");
         Assert.Contains("discard_.Visibility = snapshotHeld_ ? Visibility.Visible : Visibility.Collapsed;", applied, StringComparison.Ordinal);
         Assert.Contains("decline_.Visibility = !snapshotHeld_ &&", applied, StringComparison.Ordinal);
      }

      [Fact]
      public void TheListShowsTitles_AndTheColumnSaysNotes()
      {
         string source = ReadView();
         Assert.DoesNotContain("settings that would change on this server: {1} of {2}.", source, StringComparison.Ordinal);
         Assert.Contains("F(\"Would change here: {0} of {1}\", Changes, Entries)", source, StringComparison.Ordinal);
         Assert.Contains("profiles_.ItemTemplate = ChoiceTemplate_();", source, StringComparison.Ordinal);
         Assert.Contains("Column_(L(\"Notes\"), nameof(RowView.Note)", source, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Asks for a confirmation\")", source, StringComparison.Ordinal);
      }

      private static string ReadView()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "ProfilesView.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/ProfilesView.cs could not be found from the test binaries.");
         return null;
      }
   }
}
