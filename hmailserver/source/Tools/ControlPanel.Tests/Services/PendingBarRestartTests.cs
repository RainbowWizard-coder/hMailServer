// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The window's pending bar had one button, Apply now, which takes only the
   /// settings a reinitialise takes: the ones waiting for a service restart had
   /// nothing to press, though the feature settings pages had a restart of their
   /// own, and the bar never named the settings it counted (review finding F010).
   /// The restart is now one shared flow (Services/ServiceRestart), which the bar
   /// offers for the service on this machine and asks about first. Scanned rather
   /// than rendered: this assembly has no WPF.
   /// </summary>
   public class PendingBarRestartTests
   {
      [Fact]
      public void TheBar_OffersTheRestart_ForTheServiceOnThisMachine_AndNamesTheSettings()
      {
         string markup = SaveStateModelTests.ReadControlPanelFile("MainWindow.xaml");
         Assert.Contains("x:Name=\"RestartServiceButton\"", markup, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.AutomationId=\"restart-service-pending\"", markup, StringComparison.Ordinal);
         Assert.Contains("Click=\"RestartPending_Click\"", markup, StringComparison.Ordinal);

         string shell = SaveStateModelTests.ReadControlPanelFile("MainWindow.xaml.cs");
         string refresh = WithheldRowTests.MemberText(shell, "private void RefreshPendingBar_(bool fresh)");
         Assert.True(refresh != null, "RefreshPendingBar_ is gone.");
         Assert.Contains("waitingForRestart > 0 && ServiceRestart.Available", refresh, StringComparison.Ordinal);
         Assert.Contains("SettingApplies.PendingDetail(pending, IniFeatureStore.AppliesFor)", refresh, StringComparison.Ordinal);
         Assert.Contains("PendingNotice.ToolTip = detail", refresh, StringComparison.Ordinal);
         Assert.Contains("SetHelpText(PendingNotice, detail", refresh, StringComparison.Ordinal);
      }

      [Fact]
      public void TheBarsRestart_AsksBeforeItStopsTheService()
      {
         string shell = SaveStateModelTests.ReadControlPanelFile("MainWindow.xaml.cs");
         string click = WithheldRowTests.MemberText(shell, "private async void RestartPending_Click(object sender, RoutedEventArgs e)");
         Assert.True(click != null, "RestartPending_Click is gone.");

         int asks = click.IndexOf("MessageBox.Confirm(SettingApplies.RestartQuestion(", StringComparison.Ordinal);
         int restarts = click.IndexOf("ServiceRestart.RunAsync()", StringComparison.Ordinal);
         Assert.True(asks >= 0 && restarts > asks, "The bar restarts the service without asking first.");
         Assert.Contains("cancelByDefault: true", click, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("Views/FeatureSettingsView.xaml.cs")]
      [InlineData("Views/DirectorySyncView.cs")]
      public void EveryRestart_GoesThroughTheSharedFlow(string file)
      {
         string code = SaveStateModelTests.ReadControlPanelFile(file);
         Assert.Contains("ServiceRestart.RunAsync()", code, StringComparison.Ordinal);
         Assert.DoesNotContain("new ServiceController(", code, StringComparison.Ordinal);
         Assert.DoesNotContain("TryRestartService", code, StringComparison.Ordinal);
      }

      [Fact]
      public void DirectorySynchronisation_OffersTheRestart_WhenTheScheduleWaitsForIt()
      {
         string code = SaveStateModelTests.ReadControlPanelFile("Views/DirectorySyncView.cs");
         string save = WithheldRowTests.MemberText(code, "private bool Save_()");
         Assert.True(save != null && save.Contains("action: RestartAction_()", StringComparison.Ordinal),
            "A schedule that waits for a restart is not offered one.");

         string action = WithheldRowTests.MemberText(code, "private Wpf.Ui.Controls.Button RestartAction_()");
         Assert.True(action != null && action.Contains("if (!ServiceRestart.Available)", StringComparison.Ordinal),
            "The synchronisation page offers to restart a service it cannot reach.");

         string restart = WithheldRowTests.MemberText(code, "private async Task RestartForSchedule_()");
         Assert.True(restart != null, "RestartForSchedule_ is gone.");
         int asks = restart.IndexOf("MessageBox.Confirm(", StringComparison.Ordinal);
         int restarts = restart.IndexOf("ServiceRestart.RunAsync()", StringComparison.Ordinal);
         Assert.True(asks >= 0 && restarts > asks, "The synchronisation page restarts the service without asking first.");
      }
   }
}
