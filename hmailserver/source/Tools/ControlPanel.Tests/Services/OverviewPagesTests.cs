// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The transport encryption, spam filtering and virus scanning overviews show
   /// where a server stands at the right level and put its problems first. Each
   /// verdict was plain body text with no level; the Critical and Warning notes
   /// were the last card, several screens down, with no way to the page that
   /// fixes them; and nothing at the top said how many problems there were. The
   /// levels, words and pages are the pipelines' and are held by their own tests;
   /// this holds the three pages to drawing them.
   ///
   /// Scanned rather than rendered: this assembly has no WPF.
   /// </summary>
   public class OverviewPagesTests
   {
      [Theory]
      [InlineData("TlsOverviewView.cs", "TlsPosture", "TlsPostureNote")]
      [InlineData("SpamOverviewView.cs", "SpamPipeline", "SpamPipelineNote")]
      [InlineData("VirusOverviewView.cs", "VirusPipeline", "VirusPipelineNote")]
      public void TheVerdict_IsANoticeAtItsLevel_WithTheProblemsUnderIt_AndTheirCountByTheTitle(string file, string pipeline, string note)
      {
         string code = ReadView(file);

         // The verdict, at its level, with no action: where it has something to
         // put right a note says it, with the page.
         Assert.Contains("OverviewNotices.Notice(" + pipeline + ".VerdictLevel(config), " + pipeline + ".Verdict(config), null,", code, StringComparison.Ordinal);
         Assert.DoesNotContain("VerdictPage", code, StringComparison.Ordinal);

         // The problems under it, each with its page; the rest at the foot.
         Assert.Contains("List<" + note + "> problems = OverviewNotices.Problems(notes, n => n.Level);", code, StringComparison.Ordinal);
         Assert.Contains("content.Children.Add(OverviewNotices.Notice(problem.Level, problem.Text, problem.Page,", code, StringComparison.Ordinal);
         Assert.Contains("body_.Children.Add(NotesCard(worthKnowing));", code, StringComparison.Ordinal);
         Assert.DoesNotContain("body_.Children.Add(NotesCard(notes));", code, StringComparison.Ordinal);

         // Their count beside the title, and none while nothing could be read.
         Assert.Contains("header_.Status = OverviewNotices.ProblemsPill(problems.Count,", code, StringComparison.Ordinal);
         Assert.Contains("header_.Status = null;", code, StringComparison.Ordinal);

         // The verdict is no longer a paragraph of body text.
         Assert.DoesNotContain("Paragraph(" + pipeline + ".Verdict(config), Typography.Body)", code, StringComparison.Ordinal);
      }

      /// <summary>
      /// No overview says its verdict again, as a notice at its level, above the
      /// notes that say it: on transport encryption "POP3 110 has no encryption
      /// available..." stood above "POP3 on port 110 has no encryption
      /// available...", on virus scanning "Messages up to 10,240 KB (10 MB) are
      /// scanned by one scanner; anything larger is delivered without being
      /// scanned at all..." above the two Warnings that say it, and on spam
      /// filtering "Nothing at all happens on score: both thresholds are 0..."
      /// above "Both thresholds are 0, so no score can mark or refuse
      /// anything...", each with the same button and one notice more than the
      /// count beside the title. The pipelines hold NotesSayTheVerdict to a note
      /// at the verdict's level; this holds each page to drawing the verdict's
      /// notice only where it is false, and the spam and virus verdicts, which
      /// still say what becomes of every message, to the card's lead sentence
      /// where it is true.
      /// </summary>
      [Theory]
      [InlineData("TlsOverviewView.cs", "TlsPosture", "tls")]
      [InlineData("SpamOverviewView.cs", "SpamPipeline", "spam")]
      [InlineData("VirusOverviewView.cs", "VirusPipeline", "virus")]
      public void TheVerdict_IsNotSaidTwice(string file, string pipeline, string key)
      {
         string code = ReadView(file);
         string notice = "OverviewNotices.Notice(" + pipeline + ".VerdictLevel(config), " + pipeline + ".Verdict(config),";
         int guard = code.IndexOf("if (!" + pipeline + ".NotesSayTheVerdict(config))", StringComparison.Ordinal);
         int verdict = code.IndexOf(notice, StringComparison.Ordinal);
         Assert.True(guard >= 0, "The verdict is shown whatever the notes say.");
         Assert.True(verdict > guard && verdict - guard < 120, "The verdict's notice is not the one the guard keeps out.");
         Assert.Equal(verdict, code.LastIndexOf(notice, StringComparison.Ordinal));

         // The transport encryption verdict has no sentence where the notes say
         // it (TlsPosture.Verdict is null), so nothing leads the card then.
         if (pipeline == "TlsPosture")
         {
            Assert.DoesNotContain("OverviewNotices.Lead(", code, StringComparison.Ordinal);
            return;
         }

         int lead = code.IndexOf("content.Children.Add(OverviewNotices.Lead(" + pipeline + ".Verdict(config), \"" + key + "-overview-verdict\"));", StringComparison.Ordinal);
         int otherwise = code.IndexOf("else", verdict, StringComparison.Ordinal);
         Assert.True(otherwise > verdict && lead > otherwise && lead - otherwise < 80, "Where the notes say the verdict it is not the card's lead sentence.");
      }

      /// <summary>
      /// The lead sentence is body text, at no level and with no action: the
      /// notices at the Warning level and above are the problems counted beside
      /// the title.
      /// </summary>
      [Fact]
      public void TheLeadSentence_IsBodyText_AtNoLevel()
      {
         string helper = ReadView("OverviewNotices.cs");
         string lead = helper.Substring(helper.IndexOf("public static System.Windows.Controls.TextBlock Lead(string text, string automationId)", StringComparison.Ordinal));
         lead = lead.Substring(0, lead.IndexOf("return lead;", StringComparison.Ordinal));
         Assert.Contains("\"TextBody\"", lead, StringComparison.Ordinal);
         Assert.DoesNotContain("Level", lead, StringComparison.Ordinal);
         Assert.DoesNotContain("NoticeActions", lead, StringComparison.Ordinal);
      }

      [Fact]
      public void TheProblems_AreWarningAndAbove_WorstFirst()
      {
         string helper = ReadView("OverviewNotices.cs");
         Assert.Contains("notes.Where(n => level(n) >= StatusLevel.Warning).OrderByDescending(level)", helper, StringComparison.Ordinal);
         Assert.Contains("notes.Where(n => level(n) < StatusLevel.Warning)", helper, StringComparison.Ordinal);
         Assert.Contains("NoticeActions.OpenPage(page,", helper, StringComparison.Ordinal);
      }

      [Fact]
      public void ASpamChecksPill_IsTheStateThePipelineGivesIt()
      {
         string code = ReadView("SpamOverviewView.cs");
         Assert.Contains("(StatusLevel stateLevel, string state) = SpamPipeline.StateOf(config, check);", code, StringComparison.Ordinal);
         Assert.Contains("Level = stateLevel,", code, StringComparison.Ordinal);
      }

      private static string ReadView(string file)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", file);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/" + file + " could not be found from the test binaries.");
         return null;
      }
   }
}
