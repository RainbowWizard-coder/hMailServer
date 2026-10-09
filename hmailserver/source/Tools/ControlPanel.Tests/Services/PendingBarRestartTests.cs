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

      /// <summary>
      /// The bar's two buttons are met in the order they are drawn. Each was docked
      /// right on its own, so Apply now, declared first, was drawn rightmost and
      /// Restart the service… to its left, while Tab and UI Automation, which follow
      /// declaration order, reached Apply now first: right to left.
      /// </summary>
      [Fact]
      public void TheBarsButtons_AreReached_InTheOrderTheyAreDrawn()
      {
         string markup = SaveStateModelTests.ReadControlPanelFile("MainWindow.xaml");
         int bar = markup.IndexOf("x:Name=\"PendingBar\"", StringComparison.Ordinal);
         Assert.True(bar >= 0, "The pending bar is gone.");
         int end = markup.IndexOf("</Border>", bar, StringComparison.Ordinal);
         string body = markup.Substring(bar, end - bar);

         int panel = body.IndexOf("<StackPanel DockPanel.Dock=\"Right\" Orientation=\"Horizontal\">", StringComparison.Ordinal);
         int restart = body.IndexOf("x:Name=\"RestartServiceButton\"", StringComparison.Ordinal);
         int apply = body.IndexOf("x:Name=\"ApplyPendingButton\"", StringComparison.Ordinal);
         int panelEnd = body.IndexOf("</StackPanel>", StringComparison.Ordinal);
         int notice = body.IndexOf("x:Name=\"PendingNotice\"", StringComparison.Ordinal);

         Assert.True(panel >= 0, "The bar's buttons are not in one horizontal panel docked right.");
         Assert.True(panel < restart && restart < apply && apply < panelEnd,
            "The bar's buttons are not declared in the panel in the order they are drawn: Restart the service…, then Apply now.");
         Assert.True(panelEnd < notice, "The notice is not the bar's fill, after its buttons.");

         // Neither button is docked on its own, which is what reversed them.
         string buttons = body.Substring(restart, panelEnd - restart);
         Assert.DoesNotContain("DockPanel.Dock", buttons, StringComparison.Ordinal);
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
