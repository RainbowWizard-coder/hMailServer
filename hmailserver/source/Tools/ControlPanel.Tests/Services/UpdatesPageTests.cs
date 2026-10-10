// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Updates have a page of their own (review finding F272; the owner's decision
   /// of 9 October 2026 on the low-risk moves). Check, Download and Install were on
   /// Server status, the policy that drives them - the daily check, the channel,
   /// the download, the unattended window, the signature checks - a card on API
   /// &amp; monitoring, About showed the version but not whether a newer one was
   /// waiting, and nothing was called Updates or Upgrade, so Ctrl+K "update" found
   /// only setting rows ranked last. The Updates page under Maintenance holds the
   /// three steps above the policy; Server status, the Dashboard and About say in
   /// one line when a release is waiting, with the way there.
   /// </summary>
   [Collection("Loc")]
   public class UpdatesPageTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      [Fact]
      public void TheUpdatesPageIsUnderMaintenanceBesideBackup()
      {
         NavNode updates = NavigationMap.Find("updates");
         Assert.NotNull(updates);
         Assert.Equal("Updates", updates.Title);
         Assert.Equal("Maintenance", updates.Parent.Title);

         List<string> maintenance = updates.Parent.Children.Select(child => child.Key).ToList();
         Assert.Equal(maintenance.IndexOf("backup") + 1, maintenance.IndexOf("updates"));
         Assert.Equal("administer_server", updates.Needs);
         Assert.Equal("UpdateFeed.md", HelpMap.For("updates").Doc);
         Assert.DoesNotContain("UpdateFeed.md", HelpMap.ReferenceOnly);
      }

      /// <summary>Every word an administrator types for it finds it, first among the pages.</summary>
      [Theory]
      [InlineData("update")]
      [InlineData("updates")]
      [InlineData("upgrade")]
      [InlineData("new version")]
      [InlineData("live update")]
      [InlineData("patch")]
      [InlineData("release channel")]
      [InlineData("automatic updates")]
      public void TheWordsForItFindIt(string query)
      {
         List<PaletteRow> pages = PaletteSearch.Query(query, null, null).Where(r => r.Kind == PaletteRowKind.Page).ToList();
         Assert.True(pages.Count > 0, "'" + query + "' finds no page.");
         Assert.Equal("updates", pages[0].Page);
      }

      [Theory]
      [InlineData("install a new version")]
      [InlineData("turn on automatic updates")]
      [InlineData("is there a new version")]
      public void TheTasksForItOpenIt(string phrase)
      {
         Assert.Contains(IntentIndex.Entries, intent => intent.Phrase == phrase && intent.Page == "updates");
      }

      /// <summary>The policy's settings are found on the Updates page, and none on API &amp; monitoring.</summary>
      [Fact]
      public void TheUpdatePolicyIsFiledOnTheUpdatesPage()
      {
         List<SettingEntry> policy = SettingsSearchIndex.Entries.Where(e => e.Key.StartsWith("Update", StringComparison.Ordinal)).ToList();
         Assert.True(policy.Count >= 14, "Only " + policy.Count + " Update* settings are indexed.");
         Assert.All(policy, entry => Assert.Equal("updates", entry.Page));

         string feature = ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml.cs"));
         string integration = Section_(feature, "Integration");
         Assert.DoesNotContain("UpdateCheckEnabled", integration, StringComparison.Ordinal);
         Assert.Contains("new ElsewhereSetting(\"updates\",", integration, StringComparison.Ordinal);

         string updates = Section_(feature, "Updates");
         Assert.Contains("Header.PageKey = \"updates\";", updates, StringComparison.Ordinal);
         Assert.Contains("Key = \"UpdateCheckEnabled\"", updates, StringComparison.Ordinal);
         Assert.Contains("Title = L(\"How this server updates\")", updates, StringComparison.Ordinal);
         Assert.DoesNotContain("the Status page", updates, StringComparison.Ordinal);
      }

      /// <summary>
      /// The three steps are on the Updates page's own card and nowhere else, and
      /// the Control Panel that opens again after an administrator's install goes
      /// there to say how it ended.
      /// </summary>
      [Fact]
      public void TheThreeStepsAreOnTheUpdatesPage()
      {
         string page = ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml"));
         int card = page.IndexOf("x:Name=\"UpdateCard\"", StringComparison.Ordinal);
         Assert.True(card > 0, "The Updates page has no card of its own.");
         foreach (string button in new[] { "CheckUpdateButton", "DownloadUpdateButton", "InstallUpdateButton", "UpdateValue", "InstalledVersionValue" })
            Assert.True(page.IndexOf("x:Name=\"" + button + "\"", StringComparison.Ordinal) > card, button + " is not on the Updates page's card.");

         // Under the page's notice and its file caption, as on every page of settings
         // (ControlPanelDesign.md: the file a page edits in a caption under the header, a
         // save that wrote nothing in a Critical notice under the header), and above the
         // policy it drives: the card stood above both, and a lost link put the page's
         // Critical notice under the card's.
         int notice = page.IndexOf("x:Name=\"StatusNotice\"", StringComparison.Ordinal);
         int path = page.IndexOf("x:Name=\"PathText\"", StringComparison.Ordinal);
         int cards = page.IndexOf("x:Name=\"CardsPanel\"", StringComparison.Ordinal);
         Assert.True(notice > 0 && path > notice && card > path && cards > card,
            "The Updates page's order is not the page's notice, its file caption, the card and the policy.");

         string status = ReadControlPanelFile(Path.Join("Views", "StatusView.xaml"));
         foreach (string gone in new[] { "CheckUpdateButton", "DownloadUpdateButton", "InstallUpdateButton", "UpdateValue" })
            Assert.DoesNotContain("x:Name=\"" + gone + "\"", status, StringComparison.Ordinal);

         string steps = ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.Updates.cs"));
         Assert.Contains("private async void RunUpdateAction_(string action)", steps, StringComparison.Ordinal);
         Assert.Contains("private bool TryAdministratorInstall_()", steps, StringComparison.Ordinal);
         Assert.Contains("public void ShowInstallOutcome(AdministratorUpdate.Outcome outcome)", steps, StringComparison.Ordinal);
         Assert.DoesNotContain("RunUpdateAction_", ReadControlPanelFile(Path.Join("Views", "StatusView.xaml.cs")), StringComparison.Ordinal);

         string shell = ReadControlPanelFile("MainWindow.xaml.cs");
         Assert.Contains("pageFactories_[\"updates\"] = () => new FeatureSettingsView(FeatureSettingsView.Section.Updates);", shell, StringComparison.Ordinal);
         Assert.Contains("NavigateTo(\"updates\");", shell, StringComparison.Ordinal);
         Assert.Contains("is FeatureSettingsView updates)", shell, StringComparison.Ordinal);
         Assert.DoesNotContain("is StatusView status", shell, StringComparison.Ordinal);
      }

      /// <summary>
      /// A read of the update status that failed holds all three steps while the card
      /// says so, as Server status held them before they moved: with the link lost the
      /// card said "Could not read the update status" and still offered Check for
      /// updates, which answered "The update step could not be run". A read that worked
      /// gives Check back.
      /// </summary>
      [Fact]
      public void AFailedReadHoldsEveryStep()
      {
         string steps = ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.Updates.cs"));
         int show = steps.IndexOf("ReadFailure.Show(UpdateReadNotice,", StringComparison.Ordinal);
         Assert.True(show > 0, "The Updates page's card says no failed read.");
         string call = steps.Substring(show, steps.IndexOf(");", show, StringComparison.Ordinal) - show);
         foreach (string button in new[] { "CheckUpdateButton", "DownloadUpdateButton", "InstallUpdateButton" })
            Assert.Contains(button, call, StringComparison.Ordinal);
         // Check comes back with a read that worked, unless an install is under way
         // (ShowUpdateSteps_, review finding F246).
         Assert.Contains("CheckUpdateButton.IsEnabled = shownStep_ != UpdateStep.None;", steps, StringComparison.Ordinal);
      }

      /// <summary>
      /// With the link lost the page says so under its header, Connection lost and why,
      /// and the card says it no second time: a lost link showed two Critical notices of
      /// one cause, the card's and then the page's. The steps are held all the same.
      /// </summary>
      [Fact]
      public void ALostLinkIsSaidOnceOnThePage()
      {
         string steps = ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.Updates.cs"));
         int lost = steps.IndexOf("if (saveState_.State == SettingsPageState.NotConnected)", StringComparison.Ordinal);
         int card = steps.IndexOf("ReadFailure.Show(UpdateReadNotice,", StringComparison.Ordinal);
         Assert.True(lost > 0 && card > lost, "The Updates page's card says a lost link the page has already said.");
         string held = steps.Substring(lost, card - lost);
         Assert.Contains("ReadFailure.Clear(UpdateReadNotice);", held, StringComparison.Ordinal);
         Assert.Contains("ReadFailure.Show(null, null, null, null, readError, null, CheckUpdateButton, DownloadUpdateButton, InstallUpdateButton);", held, StringComparison.Ordinal);
      }

      /// <summary>Server status, the Dashboard and About say a waiting release in one line.</summary>
      [Theory]
      [InlineData("StatusView.xaml.cs", "UpdateNotice.ShowIn(UpdateNoticeHost, \"status-update-available\");")]
      [InlineData("DashboardView.xaml.cs", "UpdateNotice.ShowIn(UpdateNoticeHost, \"dashboard-update-available\");")]
      [InlineData("AboutView.cs", "UpdateNotice.ShowIn(updateNotice_, \"about-update-available\");")]
      public void AWaitingReleaseIsSaidWhereTheVersionIs(string view, string call)
      {
         Assert.Contains(call, ReadControlPanelFile(Path.Join("Views", view)), StringComparison.Ordinal);

         string notice = ReadControlPanelFile(Path.Join("Views", "UpdateNotice.cs"));
         Assert.Contains("NoticeActions.OpenPage(\"updates\", automationId)", notice, StringComparison.Ordinal);
         Assert.Contains("return UpdateStatusText.WaitingOrFailed(state, (string)status.AvailableVersion, error, ApplyOutcome_(status), SeenOutcome_());", notice, StringComparison.Ordinal);
         Assert.Contains("Level = level,", notice, StringComparison.Ordinal);
         Assert.Contains("mayUse(updates.Needs)", notice, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData(2, "hMailServer 6.3.7 is available.")]
      [InlineData(3, "hMailServer 6.3.7 is downloaded and verified, ready to install.")]
      [InlineData(7, "hMailServer 6.3.7 is downloaded and verified, ready to install.")]
      [InlineData(0, null)]
      [InlineData(1, null)]
      [InlineData(4, null)]
      [InlineData(5, null)]
      [InlineData(6, null)]
      public void TheLineSaysAReleaseThatIsWaitingAndNothingElse(int state, string expected)
      {
         Assert.Equal(expected, UpdateStatusText.Waiting(state, "6.3.7"));
         Assert.Null(UpdateStatusText.Waiting(state, ""));
      }

      /// <summary>
      /// The line also says what Server status's Update status row said before the steps
      /// moved: an update step that failed, and the last apply's outcome - an unattended
      /// apply that was rolled back showed only on the Updates page, under Maintenance,
      /// and the health page said nothing. Each is a sentence of its own, at Warning, an
      /// apply that went as asked at Information.
      /// </summary>
      [Fact]
      public void TheLineSaysAFailedStepAndTheLastApply()
      {
         Assert.Equal((StatusLevel.Warning, "The last update step failed: gitlab.com did not answer."),
            UpdateStatusText.WaitingOrFailed(5, "6.3.7", "gitlab.com did not answer.", "", null));

         const string rolledBack = "rolled-back 6.3.7: the service did not start, so 6.3.6 was put back";
         Assert.Equal((StatusLevel.Warning, "Last update: rolled-back 6.3.7: the service did not start, so 6.3.6 was put back. hMailServer 6.3.7 is available."),
            UpdateStatusText.WaitingOrFailed(2, "6.3.7", "", rolledBack, null));
         foreach (string word in new[] { "failed", "rollback-failed", "no-rollback", "upgraded-incomplete", "refused" })
            Assert.Equal(StatusLevel.Warning, UpdateStatusText.WaitingOrFailed(1, "", "", word + " 6.3.7: why", null).Level);

         Assert.Equal((StatusLevel.Information, "Last update: ok 6.3.7: installed by the service."),
            UpdateStatusText.WaitingOrFailed(1, "", "", "ok 6.3.7: installed by the service", null));
         Assert.Equal(StatusLevel.Information, UpdateStatusText.WaitingOrFailed(2, "6.3.7", "", "cancelled 6.3.7: by the administrator", null).Level);

         // Once the Updates page has shown the outcome it is not said again; the waiting release still is.
         Assert.Equal((StatusLevel.Information, "hMailServer 6.3.7 is available."), UpdateStatusText.WaitingOrFailed(2, "6.3.7", "", rolledBack, rolledBack));
         Assert.Equal((StatusLevel.Normal, null), UpdateStatusText.WaitingOrFailed(1, "", "", rolledBack, rolledBack));
         Assert.Equal((StatusLevel.Normal, null), UpdateStatusText.WaitingOrFailed(1, "", "", "", null));
         Assert.Equal((StatusLevel.Information, "hMailServer 6.3.7 is downloaded and verified, ready to install."), UpdateStatusText.WaitingOrFailed(3, "6.3.7", "", null, null));

         // The Updates page marks the outcome its row has shown.
         string steps = ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.Updates.cs"));
         Assert.Contains("UpdateNotice.MarkSeen(applyOutcome);", steps, StringComparison.Ordinal);
      }

      /// <summary>The update status points at the policy on its own page, not at API &amp; monitoring.</summary>
      [Fact]
      public void NotCheckedYetPointsAtThePolicyBelow()
      {
         Assert.Equal("Not checked yet (Check for updates, or turn on the daily check below)", UpdateStatusText.For(0, "", "", "", ""));
      }

      private static string Section_(string feature, string section)
      {
         Match match = Regex.Match(feature, "case Section\\." + section + ":(?<body>[\\s\\S]*?)(?:case Section\\.|\\n      \\})");
         Assert.True(match.Success, "FeatureSettingsView has no " + section + " section.");
         return match.Groups["body"].Value;
      }

      private static string ReadControlPanelFile(string relativePath)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relativePath);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relativePath + " could not be found from the test binaries.");
         return null;
      }
   }
}
