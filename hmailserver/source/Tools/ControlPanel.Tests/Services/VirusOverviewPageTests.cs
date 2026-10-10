// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F265: the virus scanning overview read everything on the UI
   /// thread, the walk of up to a thousand accounts for their fetch accounts
   /// among it, so a large installation's window stopped answering with nothing
   /// on screen to say why; its "On, but cannot run" pill was held to 170 pixels
   /// and cut in German, French and Russian with no tool tip; and the sandbox was
   /// not mentioned. The page's wiring is read from its source; what the verdict
   /// and the sandbox's card say is VirusPipeline's (VirusPipelineTests).
   /// </summary>
   public class VirusOverviewPageTests
   {
      [Fact]
      public void TheFetchAccounts_AreCountedOffTheUiThread_AfterThePageIsDrawn()
      {
         string page = ReadControlPanelFile(Path.Join("Views", "VirusOverviewView.cs"));

         // The quick reads draw the page; the walk is not among them.
         string read = WithheldRowTests.MemberText(page, "private VirusPipelineConfig ReadConfig()");
         Assert.True(read != null, "VirusOverviewView.ReadConfig is gone.");
         Assert.DoesNotContain("CountFetchAccounts", read.Replace("(CountFetchAccounts_)", ""), StringComparison.Ordinal);

         string reload = WithheldRowTests.MemberText(page, "private void Reload()");
         Assert.True(reload != null, "VirusOverviewView.Reload is gone.");
         int stop = reload.IndexOf("StopCounting_();", StringComparison.Ordinal);
         int draw = reload.IndexOf("Draw_(config);", StringComparison.Ordinal);
         int walk = reload.IndexOf("CountFetchAccountsThenDraw_(config, generation_);", StringComparison.Ordinal);
         Assert.True(stop >= 0 && draw > stop && walk > draw, "Reload does not stop the last walk, draw, then walk.");

         // The walk runs on the pool and touches nothing of the page's; a walk of a
         // read the page has since replaced, or left, changes nothing.
         string then = WithheldRowTests.MemberText(page, "private async void CountFetchAccountsThenDraw_(VirusPipelineConfig config, int generation)");
         Assert.True(then != null, "VirusOverviewView.CountFetchAccountsThenDraw_ is gone.");
         Assert.Contains("await Task.Run(() => CountFetchAccounts_(cancel.Token));", then, StringComparison.Ordinal);
         Assert.Contains("if (generation != generation_ || cancel.IsCancellationRequested)", then, StringComparison.Ordinal);
         Assert.Contains("private static FetchAccountCount CountFetchAccounts_(CancellationToken cancel)", page, StringComparison.Ordinal);
         Assert.Contains("public void OnLeave() => StopCounting_();", page, StringComparison.Ordinal);

         // The wait is said where the count goes, and no "No problems found" is
         // claimed of notes still to come.
         Assert.Contains("new LoadingState { Text = L(\"Counting external accounts…\")", page, StringComparison.Ordinal);
         Assert.Contains("!counting_ && failedReads_ == 0 && VirusPipeline.VerdictLevel(config) != StatusLevel.Information", page, StringComparison.Ordinal);

         // The keyboard stays where it was when the page is drawn again.
         string redraw = WithheldRowTests.MemberText(page, "private void Draw_(VirusPipelineConfig config)");
         Assert.True(redraw != null, "VirusOverviewView.Draw_ is gone.");
         Assert.Contains("(string id, int nth) = FocusedPlace_();", redraw, StringComparison.Ordinal);
         Assert.Contains("Refocus_(id, nth)", redraw, StringComparison.Ordinal);
      }

      [Fact]
      public void TheScannersState_KeepsItsWholeWord_AndThePillSaysACutOneInATip()
      {
         string page = ReadControlPanelFile(Path.Join("Views", "VirusOverviewView.cs"));
         Assert.DoesNotContain("MaxWidth = 170", page, StringComparison.Ordinal);

         // At its own word's width: the column is as wide as its longest word, which
         // an Off beside it stretched to.
         string row = WithheldRowTests.MemberText(page, "private void AddScannerRow(Grid grid, int row, VirusScannerEntry scanner)");
         Assert.True(row != null, "VirusOverviewView.AddScannerRow is gone.");
         Assert.Contains("HorizontalAlignment = HorizontalAlignment.Left", row, StringComparison.Ordinal);

         string pill = ReadControlPanelFile(Path.Join("Views", "Scaffold", "StatusPill.cs"));
         string tip = WithheldRowTests.MemberText(pill, "private void TipWhenTrimmed_()");
         Assert.True(tip != null, "StatusPill.TipWhenTrimmed_ is gone.");
         Assert.Contains("NaturalWidth_(label, text) > label.ActualWidth + 1", tip, StringComparison.Ordinal);

         // A tool tip the page gave the pill is its own, and stays.
         Assert.Contains("if (ToolTip == null || ReferenceEquals(ToolTip, ownTip_))", tip, StringComparison.Ordinal);
         Assert.Contains("if (ReferenceEquals(ToolTip, ownTip_))", tip, StringComparison.Ordinal);

         // Asked as the word's room changes, and once a new word is laid out.
         Assert.Contains("label_.SizeChanged += Label_SizeChanged;", pill, StringComparison.Ordinal);
         Assert.Contains("pill.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(pill.TipWhenTrimmed_));", pill, StringComparison.Ordinal);
      }

      [Fact]
      public void TheSandbox_HasACardAfterAttachmentBlocking()
      {
         string page = ReadControlPanelFile(Path.Join("Views", "VirusOverviewView.cs"));

         int blocking = page.IndexOf("body_.Children.Add(AttachmentsCard(config));", StringComparison.Ordinal);
         int sandbox = page.IndexOf("body_.Children.Add(SandboxCard(config));", StringComparison.Ordinal);
         Assert.True(blocking >= 0 && sandbox > blocking, "The sandbox's card does not follow attachment blocking.");

         string card = WithheldRowTests.MemberText(page, "private static Card SandboxCard(VirusPipelineConfig config)");
         Assert.True(card != null, "VirusOverviewView.SandboxCard is gone.");
         Assert.Contains("(StatusLevel level, string state, string sentence) = VirusPipeline.Sandbox(config);", card, StringComparison.Ordinal);
         Assert.Contains("PageLink(\"antivirus\"", card, StringComparison.Ordinal);

         // On the tab that holds the sandbox's settings, where the page opens on its first.
         Assert.Contains("L(\"Sandbox\")), N(\"Sandbox\"));", card, StringComparison.Ordinal);
         Assert.Contains("NavigateToTab(page, tab)", page, StringComparison.Ordinal);

         // Read with the rest, from the server's settings and the [Settings] store.
         Assert.Contains("(bool)settings.SandboxEnabled", page, StringComparison.Ordinal);
         Assert.Contains("(int)settings.SandboxTimeoutAction == 1", page, StringComparison.Ordinal);
         Assert.Contains("ReadBool(\"QuarantineEnabled\", false)", page, StringComparison.Ordinal);
      }

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
