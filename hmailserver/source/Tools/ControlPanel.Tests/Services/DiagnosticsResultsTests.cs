// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F252: the Diagnostics and MX query results were raw text in a
   /// read-only box that looked like an input - "[ OK ]" and "[FAIL]" lines with no
   /// summary, "Diagnostics failed" and "Query failed" as plain text in the same box -
   /// Run stayed enabled while running, so a second click ran the checks twice, Copy
   /// was enabled with nothing to copy, and the message-store consistency card said a
   /// green "No problems found" of a scan thirteen days old, or of a check switched
   /// off. The age of a scan is MessageStoreConsistencyReport's, held here as a
   /// function; the pages' wiring is read from their source, as QuarantinePageTests
   /// reads its page.
   /// </summary>
   public class DiagnosticsResultsTests
   {
      private static readonly DateTime Now = new(2026, 10, 8, 23, 0, 0);

      [Fact]
      public void AScansAge_IsReadFromItsStamp_AndOnlyARecentOneIsFresh()
      {
         Assert.Equal(TimeSpan.FromMinutes(30), MessageStoreConsistencyReport.AgeOf("2026-10-08 22:30:00", Now));
         Assert.Equal(TimeSpan.FromDays(13) + TimeSpan.FromMinutes(21) + TimeSpan.FromSeconds(20), MessageStoreConsistencyReport.AgeOf("2026-09-25 22:38:40", Now));
         Assert.Null(MessageStoreConsistencyReport.AgeOf(null, Now));
         Assert.Null(MessageStoreConsistencyReport.AgeOf("25/09/2026 22:38", Now));

         Assert.True(MessageStoreConsistencyReport.IsFresh(TimeSpan.FromMinutes(70)));
         Assert.False(MessageStoreConsistencyReport.IsFresh(TimeSpan.FromHours(2)));
         Assert.False(MessageStoreConsistencyReport.IsFresh(TimeSpan.FromDays(13)));
         Assert.False(MessageStoreConsistencyReport.IsFresh(null));
         Assert.False(MessageStoreConsistencyReport.IsFresh(TimeSpan.FromHours(-3)));
      }

      [Fact]
      public void TheConsistencyCard_IsGoodOnlyForARecentScanOfACheckThatIsOn()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "UtilityViews.cs"));
         string read = WithheldRowTests.MemberText(code, "private static ConsistencyResult ReadConsistencyReport()");
         Assert.True(read != null, "DiagnosticsView.ReadConsistencyReport is gone.");

         Assert.Contains("bool fresh = enabled && Services.MessageStoreConsistencyReport.IsFresh(age);", read, StringComparison.Ordinal);
         Assert.Contains("Level = fresh ? Severity.Good : enabled ? Severity.Stale : Severity.Neutral,", read, StringComparison.Ordinal);
         Assert.Contains("That scan was {0}, although the check runs hourly", read, StringComparison.Ordinal);
         Assert.Contains("Severity.Stale => StatusLevel.Warning", code, StringComparison.Ordinal);

         // While the check is off, the page that turns it on.
         Assert.Contains("OfferSettings = !enabled", read, StringComparison.Ordinal);
         Assert.Contains("NavigateTo(\"hardening\")", code, StringComparison.Ordinal);
      }

      [Fact]
      public void TheDiagnostics_AreRowsUnderASummary_AndRunWaitsForTheRunInProgress()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "UtilityViews.cs"));
         int start = code.IndexOf("public class DiagnosticsView", StringComparison.Ordinal);
         Assert.True(start > 0, "DiagnosticsView is gone.");
         string view = code.Substring(start);

         Assert.DoesNotContain("\"[ OK ]   \"", view, StringComparison.Ordinal);
         Assert.DoesNotContain("private readonly TextBox output_", code, StringComparison.Ordinal);

         string run = WithheldRowTests.MemberText(view, "private async Task Run()");
         Assert.True(run != null, "DiagnosticsView.Run is gone.");
         Assert.True(run.IndexOf("run_.IsEnabled = false;", StringComparison.Ordinal) < run.IndexOf("await Task.Run", StringComparison.Ordinal),
            "Run is not disabled while the checks run, so a second click runs them twice.");
         Assert.Contains("loading_.Visibility = Visibility.Visible;", run, StringComparison.Ordinal);
         Assert.Contains("copy_.IsEnabled = copyText_.Length > 0;", run, StringComparison.Ordinal);
         Assert.Contains("ServerSession.DescribeComError(ex)", run, StringComparison.Ordinal);

         string show = WithheldRowTests.MemberText(view, "private void ShowResults_(List<CheckResult> checks)");
         Assert.True(show != null, "DiagnosticsView.ShowResults_ is gone.");
         // A check that did not pass is Critical in the summary as in its row's pill: one
         // level for one outcome, where the summary was a Warning over a red pill.
         Assert.Contains("Say_(LevelOf_(false), F(\"{0} of {1} checks passed. Failed: {2}.\"", show, StringComparison.Ordinal);
         string level = WithheldRowTests.MemberText(view, "private static StatusLevel LevelOf_(bool? passed)");
         Assert.True(level != null, "DiagnosticsView.LevelOf_ is gone.");
         Assert.Contains("passed == false ? StatusLevel.Critical", level, StringComparison.Ordinal);
         Assert.DoesNotContain("Say_(StatusLevel.Warning, F(\"{0} of {1} checks passed.", view, StringComparison.Ordinal);
         Assert.Contains("Say_(StatusLevel.Good,", show, StringComparison.Ordinal);
         Assert.Contains("rows_.Children.Add(CheckRow_(check));", show, StringComparison.Ordinal);

         string row = WithheldRowTests.MemberText(view, "private static FrameworkElement CheckRow_(CheckResult check)");
         Assert.True(row != null, "DiagnosticsView.CheckRow_ is gone.");
         Assert.Contains("new StatusPill", row, StringComparison.Ordinal);
         Assert.Contains("Level = LevelOf_(check.Passed),", row, StringComparison.Ordinal);
      }

      [Fact]
      public void TheMxQuery_SaysItsAnswerAtItsLevel_AndCopiesOnlyAnAnswer()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "UtilityViews.cs"));
         int start = code.IndexOf("public class MxQueryView", StringComparison.Ordinal);
         int end = code.IndexOf("public class SendoutView", StringComparison.Ordinal);
         Assert.True(start > 0 && end > start, "MxQueryView is gone.");
         string view = code.Substring(start, end - start);

         Assert.Contains("new() { Content = L(\"_Copy results\"), Margin = new Thickness(8, 0, 0, 0), IsEnabled = false }", view, StringComparison.Ordinal);
         string run = WithheldRowTests.MemberText(view, "private async Task RunQuery()");
         Assert.True(run != null, "MxQueryView.RunQuery is gone.");
         Assert.Contains("Say_(StatusLevel.Warning, F(\"The server found no mail servers for {0}", run, StringComparison.Ordinal);
         Assert.Contains("Say_(StatusLevel.Good,", run, StringComparison.Ordinal);
         Assert.Contains("Say_(StatusLevel.Critical, F(\"Query failed: {0}\"", run, StringComparison.Ordinal);
         Assert.True(run.IndexOf("run_.IsEnabled = false;", StringComparison.Ordinal) < run.IndexOf("await Task.Run", StringComparison.Ordinal),
            "Query is not disabled while a query runs.");
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
