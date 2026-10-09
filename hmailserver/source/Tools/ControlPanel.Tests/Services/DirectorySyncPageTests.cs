// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Directory synchronisation page draws a run at the level of its outcome.
   /// Its summary was Information whenever the run itself completed, so an apply
   /// in which changes failed was reported as Information with "N of M changes
   /// could not be carried out" as a grey caption under it; and its rows differed
   /// only by bold text. The levels and words are DirectorySyncReport's and are
   /// held by DirectorySyncReportTests; this holds the page to drawing them.
   ///
   /// Scanned rather than rendered: this assembly has no WPF.
   /// </summary>
   public class DirectorySyncPageTests
   {
      [Fact]
      public void TheSummary_IsAtTheOutcomesLevel_AndTheAttentionSentence_IsANoticeAtTheSame()
      {
         string code = Source();
         Assert.Contains("Summary_(report.SummaryLevel(applied), report.Summary);", code, StringComparison.Ordinal);
         Assert.Contains("private readonly InlineNotice attentionText_", code, StringComparison.Ordinal);
         Assert.Contains("attentionText_.Level = summaryText_.Level;", code, StringComparison.Ordinal);
         Assert.DoesNotContain("attentionText_.SetResourceReference(StyleProperty, \"TextCaption\")", code, StringComparison.Ordinal);
      }

      [Fact]
      public void EachRow_CarriesItsOutcomesMarkAndWord()
      {
         string row = WithheldRowTests.MemberText(Source(), "private static FrameworkElement Row_(DirectorySyncRow row, bool emphasise)");
         Assert.True(row != null, "DirectorySyncView.Row_ is gone.");
         Assert.Contains("new StatusPill", row, StringComparison.Ordinal);
         Assert.Contains("Level = row.Level,", row, StringComparison.Ordinal);
         Assert.Contains("Text = row.Word,", row, StringComparison.Ordinal);
      }

      internal static string Source()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "DirectorySyncView.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/DirectorySyncView.cs could not be found from the test binaries.");
         return null;
      }
   }
}
