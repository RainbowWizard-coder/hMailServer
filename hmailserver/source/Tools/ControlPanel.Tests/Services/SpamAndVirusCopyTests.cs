// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The spam and virus pages name each thing once (review finding F312): a
   /// scanner was "ClamAV (network daemon)" on its settings tab and "ClamAV (clamd,
   /// over TCP)" on the overview, the custom scanner "Custom", "Custom scanner" and
   /// "Custom scanner (external program)"; its result field was a "Return value"
   /// beside the overview's exit code; "Notify receiver" sat over a notice "to every
   /// recipient"; the tabs listed the scanners in another order than the server runs
   /// them; greylisting's MX switch, its bypass list's "Matched against" and "HELO
   /// host" said what the overview said in other words; the virus overview's
   /// subtitle "condemned" a message and titled a card "Attachment blocking, which is
   /// separate"; and every settings button's failure said "Test failed".
   /// </summary>
   public class SpamAndVirusCopyTests
   {
      private static readonly string Settings = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
      private static readonly string Pipeline = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Services", "VirusPipeline.cs"));

      [Fact]
      public void EachScannerHasOneName_OnItsTabAndOnTheOverview()
      {
         Assert.Contains("\"clamwin\", 1, L(\"ClamWin\"),", Pipeline, StringComparison.Ordinal);
         Assert.Contains("\"custom\", 2, L(\"Custom scanner\"),", Pipeline, StringComparison.Ordinal);
         Assert.Contains("\"clamav\", 3, L(\"ClamAV (clamd)\"),", Pipeline, StringComparison.Ordinal);
         Assert.Contains("var clamwin = Card(L(\"ClamWin\"));", Settings, StringComparison.Ordinal);
         Assert.Contains("var custom = Card(L(\"Custom scanner\"),", Settings, StringComparison.Ordinal);
         Assert.Contains("var clamav = Card(L(\"ClamAV (clamd)\"));", Settings, StringComparison.Ordinal);
         Assert.Contains("Tab(L(\"Custom scanner\")).Cards.Add(custom);", Settings, StringComparison.Ordinal);
         Assert.Contains("Label = L(\"Exit code that means infected\")", Settings, StringComparison.Ordinal);
         Assert.Contains("Label = L(\"Notify the recipients\")", Settings, StringComparison.Ordinal);
      }

      [Fact]
      public void TheScannersTabsAreInTheOrderTheServerRunsThem()
      {
         int at = Settings.IndexOf("private void BuildAntiVirus()", StringComparison.Ordinal);
         string page = Settings.Substring(at, Settings.IndexOf("private void BuildTls()", at, StringComparison.Ordinal) - at);
         int general = page.IndexOf("Tab(L(\"General\"));", StringComparison.Ordinal);
         int clamwin = page.IndexOf("Tab(L(\"ClamWin\"));", StringComparison.Ordinal);
         int custom = page.IndexOf("Tab(L(\"Custom scanner\"));", StringComparison.Ordinal);
         int clamav = page.IndexOf("Tab(L(\"ClamAV\"));", StringComparison.Ordinal);
         Assert.True(general >= 0 && general < clamwin && clamwin < custom && custom < clamav, "The antivirus tabs are not asked for in the server's order.");
         Assert.True(clamav < page.IndexOf(".Cards.Add(", StringComparison.Ordinal), "A card is added before the tabs are in order.");
         // ... which is VirusPipeline's: ClamWin 1, the custom scanner 2, ClamAV 3.
         Assert.True(Pipeline.IndexOf("\"clamwin\", 1,", StringComparison.Ordinal) < Pipeline.IndexOf("\"custom\", 2,", StringComparison.Ordinal));
         Assert.True(Pipeline.IndexOf("\"custom\", 2,", StringComparison.Ordinal) < Pipeline.IndexOf("\"clamav\", 3,", StringComparison.Ordinal));
      }

      [Fact]
      public void GreylistingAndTheOverviewsSayItInOneWay()
      {
         Assert.Contains("Label = L(\"Skip greylisting when the connecting address is an A or MX host of the sender's domain\")", Settings, StringComparison.Ordinal);
         string specs = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "CollectionSpecs.cs"));
         Assert.Contains("Label = L(\"Matched on\")", specs, StringComparison.Ordinal);
         Assert.Contains("(2, L(\"The HELO/EHLO name\"))", specs, StringComparison.Ordinal);
         Assert.Contains("HELO name is one of the {0} entries on the greylisting bypass list", ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Services", "SpamPipeline.cs")), StringComparison.Ordinal);

         string virus = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "VirusOverviewView.cs"));
         Assert.Contains("what happens to a message found to be infected.", virus, StringComparison.Ordinal);
         Assert.DoesNotContain("one of them condemns", virus, StringComparison.Ordinal);
         Assert.Contains("Section(L(\"Attachment blocking\"),", virus, StringComparison.Ordinal);
      }

      [Fact]
      public void AButtonsFailureNamesTheButton()
      {
         Assert.Contains("F(\"{0} did not work: {1}\", MnemonicText.Strip(ButtonText), ServerSession.DescribeComError(ex))", Settings, StringComparison.Ordinal);
         Assert.DoesNotContain("\"Test failed: {0}\"", Settings, StringComparison.Ordinal);
      }
   }
}
