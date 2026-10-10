// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F246: Server status said its state as body text - "Stopped" for a
   /// server paused from that page - and a paused engine raised no notice; the update
   /// flow was three equal buttons, two greyed with no reason given (a disabled button
   /// shows no tool tip), Check and Download ran on the UI thread behind a wait cursor
   /// with a tool tip that said the Control Panel waits, and the install window drew
   /// its steps as hollow circles in one colour whether done or failed. The state is a
   /// pill at its level with the paused notice and Resume under the header; the
   /// Updates page offers one step at a time as its Primary, off the UI thread with a
   /// busy line; and the install window's steps are status marks in their levels'
   /// colours.
   /// </summary>
   public class ServerStatusFlowTests
   {
      [Theory]
      [InlineData(3, "Running", StatusLevel.Good)]
      [InlineData(1, "Paused", StatusLevel.Warning)]
      [InlineData(5, "Draining", StatusLevel.Warning)]
      [InlineData(2, "Starting", StatusLevel.Information)]
      [InlineData(4, "Stopping", StatusLevel.Information)]
      [InlineData(0, "Unknown", StatusLevel.Normal)]
      public void TheState_IsAWordAtItsLevel(int state, string word, StatusLevel level)
      {
         Assert.Equal((word, level), ServerStateText.For(state));
         Assert.Equal(state == 1, ServerStateText.IsPaused(state));
      }

      [Theory]
      [InlineData(0, "", UpdateStep.Check)]
      [InlineData(1, "", UpdateStep.Check)]
      [InlineData(2, "6.3.7", UpdateStep.Download)]
      // A step that failed with a release known: a download retries it.
      [InlineData(5, "6.3.7", UpdateStep.Download)]
      [InlineData(5, "", UpdateStep.Check)]
      [InlineData(3, "6.3.7", UpdateStep.Install)]
      // Installing, being prepared, or going on in the administrator's own window.
      [InlineData(4, "6.3.7", UpdateStep.None)]
      [InlineData(6, "6.3.7", UpdateStep.None)]
      [InlineData(7, "6.3.7", UpdateStep.None)]
      public void TheUpdatesPage_OffersTheStepTheStateCallsFor(int state, string version, UpdateStep step)
         => Assert.Equal(step, UpdateSteps.Next(state, version));

      [Fact]
      public void ServerStatus_ShowsItsStateAsAPill_AndAPausedEngineUnderTheHeader()
      {
         string xaml = Read("StatusView.xaml");
         string code = Read("StatusView.xaml.cs");
         Assert.Contains("<scaffold:StatusPill Grid.Row=\"1\" Grid.Column=\"1\" x:Name=\"StatePill\"", xaml, StringComparison.Ordinal);
         Assert.DoesNotContain("x:Name=\"StateValue\"", xaml, StringComparison.Ordinal);
         Assert.True(xaml.IndexOf("x:Name=\"ReadNotice\"", StringComparison.Ordinal) < xaml.IndexOf("x:Name=\"PausedNotice\"", StringComparison.Ordinal),
            "The paused notice is not under the header's read notice.");

         Assert.Contains("(string word, StatusLevel level) = ServerStateText.For(state);", code, StringComparison.Ordinal);
         Assert.Contains("PausedNotice.Visibility = paused ? Visibility.Visible : Visibility.Collapsed;", code, StringComparison.Ordinal);
         Assert.Contains("resumeAction_.Click += PauseResume_Click;", code, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Stopped\")", code, StringComparison.Ordinal);
      }

      [Fact]
      public void TheUpdatesPage_OffersOneStepAtATime_OffTheUiThread()
      {
         string xaml = Read("FeatureSettingsView.xaml");
         string code = Read("FeatureSettingsView.Updates.cs");

         Match steps = Regex.Match(code, @"private void ShowUpdateSteps_\(UpdateStep step\)\s*\{(.*?)\n      \}", RegexOptions.Singleline);
         Assert.True(steps.Success, "ShowUpdateSteps_ is gone.");
         Assert.Contains("CheckUpdateButton.Content = checkIsTheStep ? L(\"Check for _updates\") : L(\"Check _again\");", steps.Groups[1].Value, StringComparison.Ordinal);
         Assert.Contains("DownloadUpdateButton.Visibility = step == UpdateStep.Download ? Visibility.Visible : Visibility.Collapsed;", steps.Groups[1].Value, StringComparison.Ordinal);
         Assert.Contains("InstallUpdateButton.Visibility = step == UpdateStep.Install ? Visibility.Visible : Visibility.Collapsed;", steps.Groups[1].Value, StringComparison.Ordinal);
         Assert.Contains("ShowUpdateSteps_(UpdateSteps.Next(updateState, availableVersion));", code, StringComparison.Ordinal);

         // Off the UI thread, with the card's busy line, not a wait cursor.
         Match run = Regex.Match(code, @"private async void RunUpdateAction_\(string action\)\s*\{(.*?)\n      \}", RegexOptions.Singleline);
         Assert.True(run.Success, "RunUpdateAction_ is not asynchronous.");
         Assert.Contains("await System.Threading.Tasks.Task.Run(() => RunUpdateStep_(application, action));", run.Groups[1].Value, StringComparison.Ordinal);
         Assert.DoesNotContain("Mouse.OverrideCursor", run.Groups[1].Value, StringComparison.Ordinal);
         Assert.Contains("x:Name=\"UpdateBusy\"", xaml, StringComparison.Ordinal);
         Assert.DoesNotContain("The Control Panel waits while it does", xaml, StringComparison.Ordinal);
      }

      [Theory]
      // The step offered can be pressed, and Check beside it; the steps not offered cannot.
      [InlineData(UpdateStep.Check, UpdateStep.Check, false, true)]
      [InlineData(UpdateStep.Download, UpdateStep.Check, false, false)]
      [InlineData(UpdateStep.Check, UpdateStep.Download, false, true)]
      [InlineData(UpdateStep.Download, UpdateStep.Download, false, true)]
      [InlineData(UpdateStep.Install, UpdateStep.Download, false, false)]
      [InlineData(UpdateStep.Install, UpdateStep.Install, false, true)]
      // Nothing while an install is under way, nor while a step runs, whatever the state calls for.
      [InlineData(UpdateStep.Check, UpdateStep.None, false, false)]
      [InlineData(UpdateStep.Check, UpdateStep.Download, true, false)]
      [InlineData(UpdateStep.Download, UpdateStep.Download, true, false)]
      [InlineData(UpdateStep.Install, UpdateStep.Install, true, false)]
      public void AnUpdateStep_CanBePressed_WhenOfferedAndNoStepRuns(UpdateStep button, UpdateStep offered, bool running, bool pressable)
         => Assert.Equal(pressable, UpdateSteps.CanPress(button, offered, running));

      /// <summary>
      /// The step runs off the UI thread, so the page's Reload - BuildUi, then
      /// EnterUpdates_, then ReloadUpdate_ - reads the card again while it is under way:
      /// it enabled Download and Install as the state called for while the busy line
      /// still said "Downloading 6.3.7...", and a press then returned without a word
      /// (the skeptic's review of F246). Every step's button follows CanPress with the
      /// running flag, which only SetUpdateBusy_ sets and clears.
      /// </summary>
      [Fact]
      public void TheUpdateSteps_StayHeld_WhileOneRuns_HoweverOftenTheCardIsRead()
      {
         string code = Read("FeatureSettingsView.Updates.cs");

         Match steps = Regex.Match(code, @"private void ShowUpdateSteps_\(UpdateStep step\)\s*\{(.*?)\n      \}", RegexOptions.Singleline);
         Assert.True(steps.Success, "ShowUpdateSteps_ is gone.");
         foreach (string button in new[] { "Check", "Download", "Install" })
            Assert.Contains(button + "UpdateButton.IsEnabled = UpdateSteps.CanPress(UpdateStep." + button + ", step, updateRunning_);", steps.Groups[1].Value, StringComparison.Ordinal);

         Match reload = Regex.Match(code, @"private void ReloadUpdate_\(\)\s*\{(.*?)\n      \}", RegexOptions.Singleline);
         Assert.True(reload.Success, "ReloadUpdate_ is gone.");
         Assert.Contains("CheckUpdateButton.IsEnabled = UpdateSteps.CanPress(UpdateStep.Check, shownStep_, updateRunning_);", reload.Groups[1].Value, StringComparison.Ordinal);
         Assert.DoesNotContain("CheckUpdateButton.IsEnabled = shownStep_ != UpdateStep.None;", reload.Groups[1].Value, StringComparison.Ordinal);

         // Set and cleared in one place, and the one guard of a press.
         Assert.Single(Regex.Matches(code, @"\bupdateRunning_ = "));
         Match busy = Regex.Match(code, @"private void SetUpdateBusy_\(string text\)\s*\{(.*?)\n      \}", RegexOptions.Singleline);
         Assert.True(busy.Success, "SetUpdateBusy_ is gone.");
         Assert.Contains("updateRunning_ = busy;", busy.Groups[1].Value, StringComparison.Ordinal);
         Assert.Contains("if (app == null || updateRunning_)", code, StringComparison.Ordinal);

         // Reload reaches the card through EnterUpdates_.
         string page = Read("FeatureSettingsView.xaml.cs");
         Match click = Regex.Match(page, @"private void Reload_Click\(object sender, RoutedEventArgs e\)\s*\{(.*?)\n      \}", RegexOptions.Singleline);
         Assert.True(click.Success, "Reload_Click is gone.");
         Assert.Contains("EnterUpdates_();", click.Groups[1].Value, StringComparison.Ordinal);
      }

      [Fact]
      public void TheInstallWindowsSteps_AreStatusMarksAtTheirLevels()
      {
         string code = Read("UpdateInstallWindow.cs");
         Assert.Contains("ShapeMarkVisuals.ApplyMark(mark, presentation.Shape, presentation.FillTextBrushKey);", code, StringComparison.Ordinal);
         Assert.Contains("StepState.Done => StatusLevel.Good,", code, StringComparison.Ordinal);
         Assert.Contains("StepState.Failed => StatusLevel.Critical,", code, StringComparison.Ordinal);
         Assert.DoesNotContain("SymbolRegular.Circle24", code, StringComparison.Ordinal);
      }

      private static string Read(string file) => FormLineAssert.ReadControlPanelFile(Path.Join("Views", file));
   }
}
