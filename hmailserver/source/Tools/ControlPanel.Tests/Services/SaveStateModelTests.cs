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

      /// <summary>
      /// A secret's editor says it changed by PasswordChanged. WPF-UI 4.3's PasswordBox
      /// is a TextBox, but a Password set in code - Generate's - rewrites the mask
      /// without raising TextChanged, so with TextChanged a generated metrics token or
      /// SRS secret left the pill saying nothing over an unsaved change, and the
      /// warnings that ask whether a secret is set unrefreshed (the review's re-check of
      /// F218 and F239, on server1: one PasswordChanged and no TextChanged per Generate).
      /// </summary>
      [Fact]
      public void ASecretsEditor_SaysItChanged_WhenGenerateFillsIt()
      {
         string code = ReadControlPanelFile("Views/FeatureSettingsView.xaml.cs");
         string secret = ClassBody_(code, "private class SecretSetting : Setting");

         Assert.Contains("generate.Click += (s, e) => box_.Password = PasswordGenerator.Generate(32);", secret, StringComparison.Ordinal);

         string watch = WithheldRowTests.MemberText(secret, "public override void OnEditorChanged(Action handler)");
         Assert.True(watch != null, "SecretSetting.OnEditorChanged is gone.");
         Assert.Contains("box_.PasswordChanged += (s, e) => handler();", watch, StringComparison.Ordinal);
         Assert.DoesNotContain("box_.TextChanged", secret, StringComparison.Ordinal);
      }

      /// <summary>
      /// No password box on any page is watched by TextChanged, alone or in a list of
      /// boxes wired together: the edit a page makes in code is missed by it, as
      /// Generate's was on the feature settings pages. Each name is looked for in the
      /// innermost class that declares it, since the settings classes of one page each
      /// call their own editor box_.
      /// </summary>
      [Fact]
      public void NoPasswordBox_IsWatchedByTextChanged()
      {
         string views = Path.Join(ControlPanelDirectory(), "Views");
         var found = new System.Collections.Generic.List<string>();
         int declarations = 0;

         foreach (string file in Directory.GetFiles(views, "*.cs", SearchOption.AllDirectories))
         {
            string code = File.ReadAllText(file);
            var declared = Regex.Matches(code, @"\bPasswordField\s+(\w+)\s*[=;:)]")
               .Concat(Regex.Matches(code, @"\b(\w+)\s*=\s*new\s+(?:[\w.]+\.)?PasswordField\b"))
               .ToList();

            foreach (Match declaration in declared)
            {
               declarations++;
               string name = declaration.Groups[1].Value;
               string scope = InnermostClassAt_(code, declaration.Index);

               bool direct = Regex.IsMatch(scope, @"\b" + Regex.Escape(name) + @"\.TextChanged\s*\+=");
               bool listed = Regex.Matches(scope, @"foreach\s*\([^)]*?\bin\s+new\s+[\w.]*\[\]\s*\{(?<items>[^}]*)\}\s*\)\s*\{?\s*\w+\.TextChanged\s*\+=")
                  .Any(list => Regex.IsMatch(list.Groups["items"].Value, @"\b" + Regex.Escape(name) + @"\b"));
               if (direct || listed)
                  found.Add(Path.GetFileName(file) + ": " + name);
            }
         }

         Assert.True(declarations >= 8, "The password boxes were not found: " + declarations);
         Assert.True(found.Count == 0, "These password boxes are watched by TextChanged, which a Password set in code does not raise: " + string.Join(", ", found.Distinct()));
      }

      /// <summary>The text of the innermost class whose body holds <paramref name="index"/>.</summary>
      private static string InnermostClassAt_(string code, int index)
      {
         string innermost = code;
         foreach (Match declaration in Regex.Matches(code, @"\bclass\s+\w+[^{;]*\{"))
         {
            int open = declaration.Index + declaration.Length - 1;
            if (open > index)
               break;

            int depth = 0;
            int close = code.Length - 1;
            for (int i = open; i < code.Length; i++)
            {
               if (code[i] == '{')
                  depth++;
               else if (code[i] == '}' && --depth == 0)
               {
                  close = i;
                  break;
               }
            }

            if (close > index && close + 1 - open < innermost.Length)
               innermost = code.Substring(open, close + 1 - open);
         }

         return innermost;
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

      /// <summary>
      /// The feature settings pages settle a save in the state SettingsPageStatus.AfterSave
      /// gives, the rule SettingsPageStatusTests holds. Save_ chose the state with an
      /// if-chain of its own beside that rule, so the tested rule never ran and a change
      /// to the chain passed every test.
      /// </summary>
      [Fact]
      public void TheFeatureSettingsSave_SettlesTheStateTheTestedRuleGives()
      {
         string save = WithheldRowTests.MemberText(ReadControlPanelFile("Views/FeatureSettingsView.xaml.cs"), "private bool Save_()");
         Assert.True(save != null, "FeatureSettingsView.Save_ is gone.");

         int rule = save.IndexOf("SettingsPageState state = SettingsPageStatus.AfterSave(waiting?.Count, waitingForRestart);", StringComparison.Ordinal);
         Assert.True(rule >= 0, "FeatureSettingsView.Save_ does not take its state from SettingsPageStatus.AfterSave.");

         string settles = save.Substring(rule);
         Assert.DoesNotContain("SetState_(SettingsPageState.", settles, StringComparison.Ordinal);
         Assert.Equal(3, Regex.Matches(settles, @"SetState_\(state, ").Count);
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

         // Every refusal, not only a failed write, is the Critical notice: Directory
         // authentication refused an invalid port, an invalid timeout and a new service
         // password typed with its removal ticked in message boxes, while the
         // Authentication page refuses the same conflict in its notice.
         Assert.DoesNotContain("MessageBox.Show(", save, StringComparison.Ordinal);
      }

      [Fact]
      public void DirectoryAuthentication_RefusesASave_InTheNotice()
      {
         string save = WithheldRowTests.MemberText(ReadControlPanelFile("Views/LdapSettingsView.cs"), "private bool Save_()");
         Assert.True(save != null, "LdapSettingsView.Save_ is gone.");

         int port = save.IndexOf("saveState_.Refuse(portError);", StringComparison.Ordinal);
         int timeout = save.IndexOf("saveState_.Refuse(timeoutError);", StringComparison.Ordinal);
         int conflict = save.IndexOf("saveState_.Refuse(L(\"A new service account password has been typed AND", StringComparison.Ordinal);
         int writes = save.IndexOf("IniWrite_(", StringComparison.Ordinal);
         Assert.True(port >= 0 && timeout > port && conflict > timeout && writes > conflict,
            "Directory authentication does not refuse an invalid port, an invalid timeout and a conflicting password in its notice before it writes.");

         // The notice is brought into view: the save the question before leaving a page
         // makes can be refused with the page scrolled down to the edit.
         string shared = ReadControlPanelFile("Views/IniPageSaveState.cs");
         string refuse = WithheldRowTests.MemberText(shared, "public void Refuse(string sentence)");
         Assert.True(refuse != null && refuse.Contains("notice_.BringIntoView();", StringComparison.Ordinal),
            "A refused save's notice is not brought into view.");
      }

      /// <summary>
      /// The two directory pages scroll as the feature settings pages of their group
      /// do: the page padding is the scroller's Padding, as FeatureSettingsView.xaml
      /// has it, with nothing more inside. It was the scroller's Margin, with twelve
      /// pixels of the page's own inside it, which kept the scroll bar some
      /// twenty-four pixels in from the window's edge and ended Save changes and the
      /// cards twelve pixels further left than on every other page of the group.
      /// </summary>
      [Theory]
      [InlineData("Views/LdapSettingsView.cs", "public LdapSettingsView()")]
      [InlineData("Views/DirectorySyncView.cs", "public DirectorySyncView()")]
      public void TheDirectoryPages_ScrollAsTheFeatureSettingsPagesDo(string file, string constructor)
      {
         string markup = ReadControlPanelFile("Views/FeatureSettingsView.xaml");
         Assert.Contains("<ScrollViewer VerticalScrollBarVisibility=\"Auto\" Padding=\"{StaticResource AppPagePadding}\">", markup, StringComparison.Ordinal);

         string built = WithheldRowTests.MemberText(ReadControlPanelFile(file), constructor);
         Assert.True(built != null, file + ": " + constructor + " is gone.");
         Assert.Contains("scroll.SetResourceReference(PaddingProperty, \"AppPagePadding\")", built, StringComparison.Ordinal);
         Assert.DoesNotContain("scroll.SetResourceReference(MarginProperty", built, StringComparison.Ordinal);
         Assert.Contains("var page = new StackPanel();", built, StringComparison.Ordinal);
      }

      /// <summary>
      /// Directory synchronisation says why it cannot edit as Directory authentication
      /// does (review findings F120 and F119): in the notice under the header, with the
      /// line that names the file collapsed. Its Unavailable pill said nothing, and the
      /// reason went into that line, styled TextCaptionTertiary, the design document's
      /// quietest text. A file that is here and could not be read has its own Critical
      /// notice, so the notice under the header adds nothing to it.
      /// </summary>
      [Fact]
      public void DirectorySynchronisation_SaysWhyItCannotEdit_InTheNotice()
      {
         string load = WithheldRowTests.MemberText(ReadControlPanelFile("Views/DirectorySyncView.cs"), "private void Load_()");
         Assert.True(load != null, "DirectorySyncView.Load_ is gone.");

         Assert.Matches(new Regex(@"pathText_\.Text = editable \? F\(""Editing \{0\}"", store_\.IniPath\) : null;"), load);
         Assert.Contains("pathText_.Visibility = editable ? Visibility.Visible : Visibility.Collapsed;", load, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"saveState_\.Settle\(SettingsPageState\.Unavailable,\s*L\(""hMailServer\.INI cannot be reached from this computer, so these selections cannot be edited here\."), load);
         Assert.Matches(new Regex(@"else if \(denied_ != null\)\s*saveState_\.Settle\(SettingsPageState\.Unavailable, null\);"), load);
         Assert.DoesNotContain("these selections are not shown", load, StringComparison.Ordinal);

         // Directory authentication, which this page now matches.
         string ldap = WithheldRowTests.MemberText(ReadControlPanelFile("Views/LdapSettingsView.cs"), "private void Load_()");
         Assert.True(ldap != null && ldap.Contains("saveState_.Settle(SettingsPageState.Unavailable, L(\"hMailServer.INI was not found on this machine.", StringComparison.Ordinal),
            "Directory authentication no longer says why it cannot edit in its notice.");
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

         // A changed schedule waits for the service; nothing else here does. Measured
         // from the schedule the server started with, not from the last save, which
         // called a second save after a schedule change applied, and from the file as
         // the server reads it, Enabled with the minutes (DirectorySyncScheduleTests).
         Assert.Contains("SettingsPageState.WaitsForRestart", save, StringComparison.Ordinal);
         int written = save.IndexOf("Write_(\"SyncScheduleMinutes\"", StringComparison.Ordinal);
         int saved = save.IndexOf("DirectorySyncSchedule.Shared.SavedFile(store_.IniPath)", StringComparison.Ordinal);
         Assert.True(written >= 0 && saved > written, "The schedule's wait is not measured from the file as written.");
         Assert.DoesNotContain("savedSchedule_", code, StringComparison.Ordinal);
         Assert.DoesNotContain("Shared.Saved(", code, StringComparison.Ordinal);

         // And the page says so as it opens, while the schedule still waits.
         string load = WithheldRowTests.MemberText(code, "private void Load_()");
         Assert.True(load != null, "DirectorySyncView.Load_ is gone.");
         int read = load.IndexOf("DirectorySyncSchedule.Shared.ReadFile(store_.IniPath);", StringComparison.Ordinal);
         int waits = load.IndexOf("if (DirectorySyncSchedule.Shared.Waits)", StringComparison.Ordinal);
         Assert.True(read >= 0 && waits > read, "Directory synchronisation does not say, as it opens, that a saved schedule waits.");
         Assert.Contains("SettingsPageState.WaitsForRestart", load.Substring(waits), StringComparison.Ordinal);
      }

      /// <summary>
      /// The record reads the file with the server's own call and asks the server when
      /// it started: [LDAP] Enabled is half of the schedule the server registers, and the
      /// minutes are what GetPrivateProfileInt makes of them, clamped.
      /// </summary>
      [Fact]
      public void TheScheduleRecord_ReadsTheFileAsTheServerDoes()
      {
         string session = ReadControlPanelFile("Services/DirectorySyncSchedule.Session.cs");
         Assert.Contains("ProfileApi.ReadInt(Section, \"Enabled\", 0, iniPath) != 0", session, StringComparison.Ordinal);
         Assert.Contains("ProfileApi.ReadInt(Section, \"SyncScheduleMinutes\", 0, iniPath)", session, StringComparison.Ordinal);

         string started = WithheldRowTests.MemberText(session, "public static string ServerStarted()");
         Assert.True(started != null && started.Contains("status.StartTime", StringComparison.Ordinal),
            "The record no longer asks the server when it started.");
      }

      /// <summary>
      /// Directory authentication writes [LDAP] Enabled, half of what the server
      /// registers for the synchronisation schedule. Switched on after a start with it
      /// off, a schedule in the file runs nothing until the service restarts, and the
      /// page said "No service restart is needed". It tells the record what it read and
      /// what it saved, and says Restart to apply, with the restart, when the schedule
      /// waits.
      /// </summary>
      [Fact]
      public void DirectoryAuthentication_SaysWhenItsSaveLeavesTheScheduleWaiting()
      {
         string code = ReadControlPanelFile("Views/LdapSettingsView.cs");

         string load = WithheldRowTests.MemberText(code, "private void Load_()");
         Assert.True(load != null && load.Contains("DirectorySyncSchedule.Shared.ReadFile(store_.IniPath);", StringComparison.Ordinal),
            "Directory authentication does not tell the schedule record what the file held before its save.");

         string save = WithheldRowTests.MemberText(code, "private bool Save_()");
         Assert.True(save != null, "LdapSettingsView.Save_ is gone.");
         int enabled = save.IndexOf("IniWrite_(\"Enabled\"", StringComparison.Ordinal);
         int saved = save.IndexOf("if (DirectorySyncSchedule.Shared.SavedFile(store_.IniPath))", StringComparison.Ordinal);
         Assert.True(enabled >= 0 && saved > enabled, "Directory authentication does not ask, after its save, whether the schedule waits.");
         Assert.Matches(new Regex(@"saveState_\.Settle\(SettingsPageState\.WaitsForRestart,\s*F\(""Saved \{0\} - the server picks this up within two seconds, but the directory synchronisation schedule takes effect when the service restarts\."", time\),\s*action: RestartAction_\(\)\);"), save.Substring(saved));

         string action = WithheldRowTests.MemberText(code, "private Wpf.Ui.Controls.Button RestartAction_()");
         Assert.True(action != null && action.Contains("saveState_.RestartAction(\"ldap-restart-service\"", StringComparison.Ordinal),
            "Directory authentication's restart is not the shared one.");
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
