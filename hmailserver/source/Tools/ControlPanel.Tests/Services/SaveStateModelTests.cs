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
   /// The settings pages keep one save-state model (review findings F120 and F218):
   /// no pill while the page shows what it read, Unsaved changes from the first edit,
   /// and what a save settled in until the next. The feature settings pages wore a
   /// grey Editing pill as they opened and never marked an edit, because nothing but
   /// the computed warnings listened to their editors. Scanned rather than rendered,
   /// as UnsavedChangesTests is: this assembly has no WPF. Each test fails against the
   /// tree before the change.
   /// </summary>
   public class SaveStateModelTests
   {
      [Fact]
      public void TheFeatureSettingsPill_FollowsEveryEditor()
      {
         string code = ReadControlPanelFile("Views/FeatureSettingsView.xaml.cs");

         string build = WithheldRowTests.MemberText(code, "private void BuildUi()");
         Assert.True(build != null, "FeatureSettingsView.BuildUi is gone.");
         Assert.Contains("setting.OnEditorChanged(EditorChanged_)", build, StringComparison.Ordinal);
         Assert.Contains("SetState_(SettingsPageState.Clean, null)", build, StringComparison.Ordinal);

         string changed = WithheldRowTests.MemberText(code, "private void EditorChanged_()");
         Assert.True(changed != null && changed.Contains("saveState_.Follow()", StringComparison.Ordinal),
            "An edit on a feature settings page does not reach its pill.");

         Assert.DoesNotContain("SettingsPageState.Editing", code, StringComparison.Ordinal);
      }

      /// <summary>
      /// A setting whose editor holds a value the page can call changed must say when
      /// it changes, or an edit to it leaves the pill saying nothing: the multi-line
      /// sending-limits editor had a value and no change event.
      /// </summary>
      [Fact]
      public void EverySettingWithAValue_SaysWhenItChanges()
      {
         string code = ReadControlPanelFile("Views/FeatureSettingsView.xaml.cs");

         var classes = Regex.Matches(code, @"private class (\w+) : Setting\b")
            .Select(match => match.Groups[1].Value)
            .ToList();
         Assert.NotEmpty(classes);

         var silent = classes
            .Where(name =>
            {
               string body = ClassBody_(code, "private class " + name + " : Setting");
               return body.Contains("public override string LiveValue", StringComparison.Ordinal)
                  && !body.Contains("public override void OnEditorChanged(Action handler)", StringComparison.Ordinal);
            })
            .ToList();

         Assert.True(silent.Count == 0, "These settings hold a value and never say it changed: " + string.Join(", ", silent));
      }

      [Fact]
      public void AFailedSave_IsACriticalNotice_AndNoPageWearsAPillAtRest()
      {
         string code = ReadControlPanelFile("Views/FeatureSettingsView.xaml.cs");
         string save = WithheldRowTests.MemberText(code, "private bool Save_()");
         Assert.True(save != null, "FeatureSettingsView.Save_ is gone.");
         Assert.Contains("saveState_.Refuse(F(\"Could not save: {0}\"", save, StringComparison.Ordinal);

         // The pill is collapsed in the markup, so a page that has not settled yet
         // shows none either.
         string markup = ReadControlPanelFile("Views/FeatureSettingsView.xaml");
         Assert.Matches(new Regex("<scaffold:StatusPill x:Name=\"StatePill\" Visibility=\"Collapsed\""), markup);
      }

      [Fact]
      public void TheServerSettingsPage_SettlesASaveByTheSameRule()
      {
         string code = ReadControlPanelFile("Views/ServerSettingsView.xaml.cs");
         string save = WithheldRowTests.MemberText(code, "private bool Save_()");
         Assert.True(save != null, "ServerSettingsView.Save_ is gone.");
         Assert.Contains("ServerSettingsPageStatus.AfterSave(failed, waiting, waitingForRestart)", save, StringComparison.Ordinal);
      }

      /// <summary>
      /// The two directory pages had Reload and Save changes in a footer under the
      /// scroller, the result beside them in grey caption text, and no pill; a failed
      /// save was a message box on one and grey text on the other (review finding
      /// F119). Both now put the actions and the pill in the page header and say where
      /// they stand by the model the other settings pages keep.
      /// </summary>
      [Theory]
      [InlineData("Views/LdapSettingsView.cs")]
      [InlineData("Views/DirectorySyncView.cs")]
      public void TheDirectoryPages_KeepTheirActionsAndStateInTheHeader(string file)
      {
         string code = ReadControlPanelFile(file);

         Assert.DoesNotContain("footerStatus_", code, StringComparison.Ordinal);
         Assert.DoesNotContain("BuildFooter_", code, StringComparison.Ordinal);
         Assert.Contains("Status = statePill_", code, StringComparison.Ordinal);
         Assert.Contains("Actions = BuildActions_()", code, StringComparison.Ordinal);
         Assert.Contains("new IniPageSaveState(statePill_, statusNotice_, () => HasUnsavedChanges)", code, StringComparison.Ordinal);

         string load = WithheldRowTests.MemberText(code, "private void Load_()");
         Assert.True(load != null && load.Contains("saveState_.Settle(", StringComparison.Ordinal), file + ".Load_ does not settle the pill.");

         string save = WithheldRowTests.MemberText(code, "private bool Save_()");
         Assert.True(save != null, file + ".Save_ is gone.");
         Assert.Contains("saveState_.Refuse(F(\"Could not save: {0}\"", save, StringComparison.Ordinal);
         Assert.Contains("saveState_.Settle(SettingsPageState.Saved", save, StringComparison.Ordinal);
         Assert.DoesNotContain("MessageBox.Show(F(\"Could not save", save, StringComparison.Ordinal);
      }

      [Fact]
      public void DirectorySynchronisation_SaysWhenItCouldNotWrite_AndWhenTheScheduleWaits()
      {
         string code = ReadControlPanelFile("Views/DirectorySyncView.cs");
         string save = WithheldRowTests.MemberText(code, "private bool Save_()");
         Assert.True(save != null, "DirectorySyncView.Save_ is gone.");

         // Each write is checked: the six ProfileApi.WriteString calls were not, so a
         // file that could be read and not written was reported saved.
         Assert.DoesNotContain("ProfileApi.WriteString(", save, StringComparison.Ordinal);
         string write = WithheldRowTests.MemberText(code, "private void Write_(string key, string value)");
         Assert.True(write != null && write.Contains("if (!ProfileApi.WriteString(", StringComparison.Ordinal), "A refused write is not noticed.");

         // A changed schedule waits for the service; nothing else here does.
         Assert.Contains("SettingsPageState.WaitsForRestart", save, StringComparison.Ordinal);
         Assert.Contains("scheduleChanged", save, StringComparison.Ordinal);
      }

      /// <summary>
      /// The IP ranges page's permissions card said a save by putting "(saved)" after
      /// the range's name in its title, and said nothing of a change (review finding
      /// F218). Its pill keeps the settings pages' model now.
      /// </summary>
      [Fact]
      public void TheIPRangesPermissionsCard_KeepsTheModel()
      {
         string code = ReadControlPanelFile("Views/IPRangesView.xaml.cs");
         Assert.DoesNotContain("PermCard.Title = F(\"Permissions - {0} (saved)\"", code, StringComparison.Ordinal);

         string state = WithheldRowTests.MemberText(code, "private void ShowPermsState_()");
         Assert.True(state != null && state.Contains("SettingsPageStatus.Follow(permsSettled_, HasUnsavedChanges)", StringComparison.Ordinal),
            "The permissions card's pill does not follow its boxes.");

         string save = WithheldRowTests.MemberText(code, "private void SavePerms_Click(object sender, RoutedEventArgs e)");
         Assert.True(save != null && save.Contains("permsSettled_ = SettingsPageState.Saved", StringComparison.Ordinal),
            "A saved range's card does not say Saved.");

         string markup = ReadControlPanelFile("Views/IPRangesView.xaml");
         Assert.Contains("x:Name=\"PermsStatePill\" Visibility=\"Collapsed\"", markup, StringComparison.Ordinal);
      }

      // ---- helpers -------------------------------------------------------------------------

      /// <summary>The text of a class from its declaration to its closing brace.</summary>
      internal static string ClassBody_(string code, string declaration)
      {
         int at = code.IndexOf(declaration, StringComparison.Ordinal);
         Assert.True(at >= 0, declaration + " is gone.");
         int brace = code.IndexOf('{', at);
         int depth = 0;
         for (int i = brace; i < code.Length; i++)
         {
            if (code[i] == '{')
               depth++;
            else if (code[i] == '}' && --depth == 0)
               return code.Substring(at, i + 1 - at);
         }

         return code.Substring(at);
      }

      internal static string ControlPanelDirectory()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel");
            if (File.Exists(Path.Join(candidate, "MainWindow.xaml.cs")))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("The ControlPanel folder could not be found from the test binaries.");
         return null;
      }

      internal static string ReadControlPanelFile(string relative)
         => File.ReadAllText(Path.Join(ControlPanelDirectory(), relative.Replace('/', Path.DirectorySeparatorChar)));
   }
}
