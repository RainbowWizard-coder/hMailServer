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

         // The verdict, at its level, with its page.
         Assert.Contains("OverviewNotices.Notice(" + pipeline + ".VerdictLevel(config), " + pipeline + ".Verdict(config),", code, StringComparison.Ordinal);
         Assert.Contains(pipeline + ".VerdictPage(config)", code, StringComparison.Ordinal);

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
      /// The transport encryption verdict is not said again above the notes that
      /// say it listener by listener: "POP3 110 has no encryption available..."
      /// and "POP3 on port 110 has no encryption available..." were two Critical
      /// notices, one more than the count beside the title.
      /// </summary>
      [Fact]
      public void TheTransportEncryptionVerdict_IsNotSaidTwice()
      {
         string code = ReadView("TlsOverviewView.cs");
         int guard = code.IndexOf("if (!TlsPosture.NotesSayTheVerdict(config))", StringComparison.Ordinal);
         int verdict = code.IndexOf("OverviewNotices.Notice(TlsPosture.VerdictLevel(config), TlsPosture.Verdict(config),", StringComparison.Ordinal);
         Assert.True(guard >= 0, "The verdict is shown whatever the notes say.");
         Assert.True(verdict > guard && verdict - guard < 120, "The verdict's notice is not the one the guard keeps out.");
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
