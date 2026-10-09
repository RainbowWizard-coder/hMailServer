// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F264: the spam filtering overview's card titles and phase
   /// headings were all one size and weight, with no heading semantics for the
   /// cards; each row ended in an unlabelled "2", "per entry" or "-"; every link read
   /// "Settings…" and the lists card's "Open…" sat some 600 pixels from its label;
   /// with an external filtering engine attached (FilterHook*) the list of checks
   /// left it out, so "the highest total reachable is 12" was false; and the
   /// subtitle's grammar was broken. The checks and their words are SpamPipeline's;
   /// the page's structure is read from its source.
   /// </summary>
   public class SpamOverviewStructureTests
   {
      private static SpamPipelineConfig Config()
         => new()
         {
            MarkThreshold = 5,
            DeleteThreshold = 20,
            EvaluateDmarc = true,
            DmarcFailureScore = 5,
            CheckHeloHost = true,
            HeloHostScore = 0,
            SpamAssassinHost = "127.0.0.1",
            FilterHookRejectScore = 100
         };

      [Fact]
      public void TheExternalEngine_IsTheEleventhCheck_AndLeavesTheTotalOpenEnded()
      {
         SpamPipelineConfig config = Config();
         SpamCheck off = SpamPipeline.Checks(config).Single(c => c.Key == "filterhook");
         Assert.Equal(11, off.Order);
         Assert.Equal(SpamCheckPhase.AfterTheBody, off.Phase);
         Assert.False(off.Enabled);
         Assert.Equal("adds nothing", off.ScoreText);
         Assert.Equal(5, SpamPipeline.HighestReachableScore(config));

         config.FilterHookUrl = "http://127.0.0.1:11333/checkv2";
         SpamCheck on = SpamPipeline.Checks(config).Single(c => c.Key == "filterhook");
         Assert.True(on.Enabled);
         Assert.Null(on.Score);
         Assert.Equal("the engine's score", on.ScoreText);
         Assert.Contains("http://127.0.0.1:11333/checkv2", on.Detail, StringComparison.Ordinal);
         Assert.Contains("an answer of reject adds 100", on.Detail, StringComparison.Ordinal);

         Assert.Null(SpamPipeline.HighestReachableScore(config));
         Assert.Contains("the external engine's", SpamPipeline.CeilingSentence(config), StringComparison.Ordinal);

         // The size limit skips the engine with the other checks that need the body.
         config.MaxScanKilobytes = 1024;
         Assert.Contains(SpamPipeline.Notes(config), n => n.Text.Contains("SpamAssassin and the external filtering engine", StringComparison.Ordinal));
      }

      [Fact]
      public void AScore_IsWrittenAsWhatItAdds()
      {
         SpamPipelineConfig config = Config();
         var checks = SpamPipeline.Checks(config).ToDictionary(c => c.Key);
         Assert.Equal("+5", checks["dmarc"].ScoreText);
         Assert.Equal("adds nothing", checks["helo"].ScoreText);
         Assert.Equal("adds nothing", checks["blockedsenders"].ScoreText);

         config.BlockedSenderCount = 3;
         Assert.Equal("the entry's score", SpamPipeline.Checks(config).Single(c => c.Key == "blockedsenders").ScoreText);
         Assert.DoesNotContain(SpamPipeline.Checks(config), c => c.ScoreText == "-" || c.ScoreText.StartsWith("per ", StringComparison.Ordinal));
      }

      [Fact]
      public void ThePage_HasHeadingsAtTwoLevels_AHeaderRow_AndLinksThatNameTheirPage()
      {
         string view = ReadControlPanelFile(Path.Join("Views", "SpamOverviewView.cs"));

         string section = WithheldRowTests.MemberText(view, "private static Card Section(string title, out StackPanel content)");
         Assert.True(section != null, "SpamOverviewView.Section is gone.");
         Assert.Contains("AutomationHeadingLevel.Level2", section, StringComparison.Ordinal);

         // The phase lines are overlines, a level below the cards.
         Assert.Contains("phaseHeading.SetResourceReference(StyleProperty, \"TextCaption\");", view, StringComparison.Ordinal);
         Assert.Contains("SetHeadingLevel(phaseHeading, System.Windows.Automation.AutomationHeadingLevel.Level3)", view, StringComparison.Ordinal);

         // Check, State, Adds over the columns.
         Assert.Contains("AddColumnHeader(grid, 1, L(\"Spam check\")", view, StringComparison.Ordinal);
         Assert.Contains("AddColumnHeader(grid, 3, L(\"Adds\")", view, StringComparison.Ordinal);

         // No link says Settings… or Open…; each names its page.
         Assert.DoesNotContain("L(\"Settings…\")", view, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Open…\")", view, StringComparison.Ordinal);
         Assert.Contains("F(\"{0} →\"", view, StringComparison.Ordinal);

         // The engine is read, and the subtitle reads.
         Assert.Contains("ini.Read(\"FilterHookUrl\", \"\")", view, StringComparison.Ordinal);
         Assert.Contains("Each spam check in the order the server runs it, what it adds to the score, and what the total does to a message.", view, StringComparison.Ordinal);
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
