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
   /// Review finding F253: the Message trace page showed the same Information notice
   /// whatever the setting - that the trace records nothing until it is switched on -
   /// above a blank grid of headers; "Follow this message" replaced the address's
   /// results with no way back, and Enter and a double-click did nothing; and the
   /// Status column was the bare reply code, right-aligned like a count. The words
   /// are MessageTraceStatus's and are held here; the page's wiring is read from its
   /// source.
   /// </summary>
   public class MessageTraceStatusTests
   {
      [Theory]
      [InlineData(250, StatusLevel.Good, "OK 250")]
      [InlineData(221, StatusLevel.Good, "OK 221")]
      [InlineData(451, StatusLevel.Warning, "Deferred 451")]
      [InlineData(421, StatusLevel.Warning, "Deferred 421")]
      [InlineData(550, StatusLevel.Critical, "Refused 550")]
      [InlineData(554, StatusLevel.Critical, "Refused 554")]
      [InlineData(0, StatusLevel.Normal, "")]
      [InlineData(999, StatusLevel.Normal, "999")]
      public void AReplyCode_IsItsClassesWordAndLevel(int code, StatusLevel level, string text)
      {
         Assert.Equal(level, MessageTraceStatus.LevelOf(code));
         Assert.Equal(text, MessageTraceStatus.TextOf(code));
      }

      [Fact]
      public void TheRetention_IsSaidInDays_AndZeroKeepsEveryEvent()
      {
         Assert.Equal("Events are kept 30 days.", MessageTraceStatus.KeptFor(30));
         Assert.Equal("Events are kept 1 day.", MessageTraceStatus.KeptFor(1));
         Assert.Equal("Events are kept with no time limit.", MessageTraceStatus.KeptFor(0));
         Assert.Equal("Events are kept with no time limit.", MessageTraceStatus.KeptFor(-1));
      }

      [Fact]
      public void ThePage_SaysWhetherTracingIsOn_AndKeepsTheResultsAMessageIsFollowedFrom()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "MessageTraceView.cs"));

         // Read as the page opens: off is a Warning with the page that turns it on,
         // on is how long events are kept; the old notice that said both is gone.
         Assert.Contains("public class MessageTraceView : UserControl, IPageLifecycle, IRefreshablePage", code, StringComparison.Ordinal);
         string enter = WithheldRowTests.MemberText(code, "public void OnEnter()");
         Assert.True(enter != null, "MessageTraceView.OnEnter is gone.");
         Assert.Contains("ShowTracingState_();", enter, StringComparison.Ordinal);
         Assert.Contains("iniStore_.Read(\"MessageTraceEnabled\", \"0\")", code, StringComparison.Ordinal);
         Assert.Contains("L(\"Message tracing is off - nothing is being recorded.\")", code, StringComparison.Ordinal);
         Assert.Contains("tracingOff_.Content = OpenLogging_();", code, StringComparison.Ordinal);
         Assert.Contains("MessageTraceStatus.KeptFor(RetentionDays_())", code, StringComparison.Ordinal);
         Assert.DoesNotContain("The trace records nothing at all until message tracing is switched on", code, StringComparison.Ordinal);

         // Before the first search, the empty state says what to do.
         Assert.Contains("private readonly EmptyState empty_ = new() { Icon = Wpf.Ui.Controls.SymbolRegular.DocumentSearch24 };", code, StringComparison.Ordinal);
         Assert.Contains("empty_.Text = L(\"Enter an address and press Search to see every event of the mail it sent or received.\");", code, StringComparison.Ordinal);

         // A followed message keeps the results it came from, for Back to results.
         string follow = WithheldRowTests.MemberText(code, "private void Follow_(int queueId)");
         Assert.True(follow != null, "MessageTraceView.Follow_ is gone.");
         Assert.Contains("resultRows_ = results;", follow, StringComparison.Ordinal);
         Assert.Contains("ShowBack_();", follow, StringComparison.Ordinal);
         string back = WithheldRowTests.MemberText(code, "private void BackToResults_()");
         Assert.True(back != null, "MessageTraceView.BackToResults_ is gone.");
         Assert.Contains("list_.ItemsSource = resultRows_;", back, StringComparison.Ordinal);
         Assert.Contains("list_.SelectedIndex = resultSelected_;", back, StringComparison.Ordinal);
         Assert.Contains("Remember_(() => Search_(address));", back, StringComparison.Ordinal);
         Assert.Contains("F(\"_Back to results for {0}\", resultsFor_.Replace(\"_\", \"__\"))", code, StringComparison.Ordinal);

         // Enter, F2 and a double-click follow, as the button does.
         Assert.Contains("GridKeys.Wire(list_, follow, null, null);", code, StringComparison.Ordinal);

         // The status is a pill of its word and level, sorted by the code.
         Assert.Contains("pill.SetBinding(StatusPill.LevelProperty, new System.Windows.Data.Binding(nameof(TraceRow.StatusLevel)));", code, StringComparison.Ordinal);
         Assert.Contains("SortMemberPath = nameof(TraceRow.StatusCode),", code, StringComparison.Ordinal);
         Assert.DoesNotContain("Binding = new System.Windows.Data.Binding(nameof(TraceRow.StatusCode))", code, StringComparison.Ordinal);

         // Ctrl+C copies the status's words: a template column copies nothing of its own,
         // and the copy had an empty status where the text column it replaced had the code.
         Assert.Contains("ClipboardContentBinding = new System.Windows.Data.Binding(nameof(TraceRow.StatusText)),", code, StringComparison.Ordinal);
      }

      /// <summary>
      /// The final check of Tier 2: the Status column was Auto with no floor, and the
      /// delivery queue's Trace message, which fills the grid before the page's first
      /// layout, left it at its header's width - every pill cut to its mark and a dot,
      /// "■ .", in every language, until a search drew them again. The column is floored
      /// at its header and at the widest class's pill, measured as the pill draws it.
      /// </summary>
      [Fact]
      public void TheStatusColumn_IsFlooredAtItsWidestPill()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "MessageTraceView.cs"));
         Assert.Contains("MinWidth = StatusFloor_()", code, StringComparison.Ordinal);
         string floor = WithheldRowTests.MemberText(code, "private static double StatusFloor_()");
         Assert.True(floor != null, "MessageTraceView.StatusFloor_ is gone.");
         Assert.Contains("GridStyles.HeaderWidth(L(\"Status\"))", floor, StringComparison.Ordinal);
         Assert.Contains("new[] { 250, 451, 550 }", floor, StringComparison.Ordinal);
         Assert.Contains("GridStyles.PillCellWidth(MessageTraceStatus.TextOf(code))", floor, StringComparison.Ordinal);

         // The pill's own room is the template's: padding 8 and 10, an 8-pixel mark, a 6-pixel gap.
         string styles = ReadControlPanelFile(Path.Join("Services", "GridStyles.cs"));
         Assert.Contains("private const double PillChrome = 8 + 10 + 8 + 6;", styles, StringComparison.Ordinal);
         string scaffold = ReadControlPanelFile(Path.Join("Views", "Scaffold", "Scaffold.xaml"));
         Assert.Contains("Padding=\"8,3,10,4\" SnapsToDevicePixels=\"True\"", scaffold, StringComparison.Ordinal);
         Assert.Contains("<Path x:Name=\"PART_Mark\" Width=\"8\" Height=\"8\" Stretch=\"Fill\"", scaffold, StringComparison.Ordinal);
         Assert.Contains("Margin=\"0,0,6,0\" VerticalAlignment=\"Center\" />", scaffold, StringComparison.Ordinal);
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
