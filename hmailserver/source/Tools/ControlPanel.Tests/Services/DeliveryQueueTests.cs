// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F248: the Delivery queue's grid let several rows be chosen while
   /// View source, Deliver now and Remove read the first alone - Remove asked about one
   /// message and removed one - its purpose promised "why each is waiting" and no column
   /// said it, the server's locked flag (a message being delivered now) read and dropped;
   /// nothing led from a row to its trace; and at 760 pixels the fixed date columns cut
   /// From and Recipients to a word. Deliver now and Remove act on every row chosen, with
   /// Deliver all now beside them; a State column says why each waits; Trace message opens
   /// the message's events on Message trace; and From and Recipients have floors.
   /// </summary>
   public class DeliveryQueueTests
   {
      private static readonly DateTime Now = new(2026, 10, 10, 14, 0, 0);

      [Fact]
      public void TheState_SaysWhyAMessageWaits()
      {
         Assert.Equal("Delivering now", QueueWords.State(true, "2026-10-10 14:12:00", Now, sameClock: true));
         Assert.Equal("Next try in 12 min", QueueWords.State(false, "2026-10-10 14:12:30", Now, sameClock: true));
         Assert.Equal("Next try in 1 h 5 min", QueueWords.State(false, "2026-10-10 15:05:00", Now, sameClock: true));
         // A next try passed, or the server's never - a new message - is due.
         Assert.Equal("Due for delivery", QueueWords.State(false, "2026-10-10 13:58:00", Now, sameClock: true));
         Assert.Equal("Due for delivery", QueueWords.State(false, "1901-01-01 00:00:00", Now, sameClock: true));
         Assert.Equal("Due for delivery", QueueWords.State(false, "1901-01-01", Now, sameClock: false));
         Assert.Equal("Due for delivery", QueueWords.State(false, "", Now, sameClock: true));
      }

      /// <summary>
      /// Across two computers' clocks the wait is not worked out: the State says the next
      /// try itself, in the reader's date and time, marked as the server's - the Next try
      /// column it replaces said it as the database's text. A time in none of the server's
      /// formats is not taken for "due": it is said as the server wrote it.
      /// </summary>
      [Fact]
      public void TheState_SaysTheServersTime_WhereTheClocksMayDiffer()
      {
         CultureInfo before = CultureInfo.CurrentCulture;
         try
         {
            CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("en-GB");
            Assert.Equal("Next try at 10/10/2026 14:12 (server time)", QueueWords.State(false, "2026-10-10 14:12:00", Now, sameClock: false));
            Assert.Equal("Next try at 10/10/2026 14:12 (server time)", QueueWords.StateTip(false, "2026-10-10 14:12:00"));
            Assert.Equal("Next try at 10 Oct 14:12 (server time)", QueueWords.State(false, "10 Oct 14:12", Now, sameClock: true));
         }
         finally
         {
            CultureInfo.CurrentCulture = before;
         }

         // No time to give for a message being delivered, or one never tried.
         Assert.Null(QueueWords.StateTip(true, "2026-10-10 14:12:00"));
         Assert.Null(QueueWords.StateTip(false, "1901-01-01 00:00:00"));
      }

      [Fact]
      public void TheStateColumn_SortsTheDeliveredFirst_ThenTheDue_ThenTheSoonest()
      {
         var rows = new List<(string Name, DateTime Order)>
         {
            ("later", QueueWords.Order(false, "2026-10-10 15:00:00")),
            ("unreadable", QueueWords.Order(false, "soon")),
            ("due", QueueWords.Order(false, "1901-01-01 00:00:00")),
            ("sooner", QueueWords.Order(false, "2026-10-10 14:10:00")),
            ("delivering", QueueWords.Order(true, "2026-10-10 14:10:00"))
         };

         rows.Sort((a, b) => a.Order.CompareTo(b.Order));
         Assert.Equal(new[] { "delivering", "due", "sooner", "later", "unreadable" }, rows.ConvertAll(r => r.Name).ToArray());
      }

      [Fact]
      public void TheServersNever_IsToldApartFromATimeThatCannotBeRead()
      {
         Assert.True(TimeUntilText.IsNever("1901-01-01"));
         Assert.True(TimeUntilText.IsNever("1901-01-01 00:00:00"));
         Assert.True(TimeUntilText.IsNever(" "));
         Assert.False(TimeUntilText.IsNever("2026-10-10 14:12:00"));
         Assert.False(TimeUntilText.IsNever("soon"));
      }

      /// <summary>
      /// Trace message on a message whose id is past 32 bits - the server's ids are
      /// 64-bit and outgrow 32 on a long-lived installation, the trace keeps 32 - opened
      /// Message trace and left it as it was, saying nothing; on such a server Trace
      /// message never worked for any message. The trace page says why and what to
      /// search for instead, and takes the last search's events away.
      /// </summary>
      [Theory]
      [InlineData(1L, true)]
      [InlineData(2147483647L, true)]
      [InlineData(2147483648L, false)]
      [InlineData(5000000000L, false)]
      [InlineData(0L, false)]
      [InlineData(-1L, false)]
      public void TraceMessage_FollowsTheIdsTheTraceKeeps(long id, bool follows)
         => Assert.Equal(follows, QueueWords.CanFollow(id));

      [Fact]
      public void TraceMessage_SaysWhyAMessageCannotBeFollowed()
      {
         Assert.Equal("Message 5000000000 cannot be followed here: the trace records queue ids only up to 2147483647. Search for its sender or a recipient instead.",
            QueueWords.CannotFollow(5000000000L));

         Match follow = Regex.Match(Read("MessageTraceView.cs"), @"public void FollowQueuedMessage\(long queueId\)\s*\{(.*?)\n      \}", RegexOptions.Singleline);
         Assert.True(follow.Success, "MessageTraceView.FollowQueuedMessage is gone.");
         string body = follow.Groups[1].Value;
         Assert.Contains("if (QueueWords.CanFollow(queueId))", body, StringComparison.Ordinal);
         int cleared = body.IndexOf("list_.ItemsSource = null;", StringComparison.Ordinal);
         int said = body.IndexOf("Status_(StatusLevel.Information, QueueWords.CannotFollow(queueId));", StringComparison.Ordinal);
         Assert.True(cleared > 0 && said > cleared, "A message the trace cannot follow is not said, or the last search's events stay under it.");
         Assert.Contains("count_.Visibility = Visibility.Collapsed;", body, StringComparison.Ordinal);
         Assert.Contains("Remember_(() => FollowQueuedMessage(queueId));", body, StringComparison.Ordinal);
      }

      [Fact]
      public void WhatTheButtonsDid_NamesOneMessage_AndCountsMore()
      {
         Assert.Equal("Message 1042 will be tried again now.", QueueWords.Retried(new List<string> { "1042" }));
         Assert.Equal("3 messages will be tried again now.", QueueWords.Retried(new List<string> { "1042", "1043", "1044" }));
         Assert.Equal("Message 1042 removed.", QueueWords.Removed(new List<string> { "1042" }));
         Assert.Equal("5 messages removed.", QueueWords.Removed(new List<string> { "1", "2", "3", "4", "5" }));
         Assert.Equal("Remove 5 messages from the queue? They are deleted without a bounce, so their senders are not told.", Confirmations.RemoveQueuedMessages(5));
      }

      /// <summary>A refusal part of the way through says what was removed, then the one refused and why.</summary>
      [Fact]
      public void ARefusedRemoval_SaysWhatWasRemoved_ThenWhichWasRefused()
      {
         Assert.Equal("Could not remove message 1042: it is held for the sandbox.",
            QueueWords.RemoveRefused(new List<string>(), "1042", "it is held for the sandbox."));
         Assert.Equal("2 messages removed. Could not remove message 1044: it is held for the sandbox.",
            QueueWords.RemoveRefused(new List<string> { "1042", "1043" }, "1044", "it is held for the sandbox."));

         string code = Read("QueueView.xaml.cs");
         Assert.Contains("QueueWords.RemoveRefused(removed, trying, ServerSession.DescribeComError(ex))", code, StringComparison.Ordinal);
      }

      [Fact]
      public void DeliverNowAndRemove_ActOnEveryRowChosen()
      {
         string xaml = Read("QueueView.xaml");
         string code = Read("QueueView.xaml.cs");
         Assert.Contains("SelectionMode=\"Extended\"", xaml, StringComparison.Ordinal);
         Assert.Contains("private void Retry_Click(object sender, RoutedEventArgs e) => Retry_(Chosen_());", code, StringComparison.Ordinal);

         Match remove = Regex.Match(code, @"private void Remove_Click\(object sender, RoutedEventArgs e\)\s*\{(.*?)\n      \}", RegexOptions.Singleline);
         Assert.True(remove.Success, "Remove_Click is gone.");
         Assert.Contains("List<QueueRow> rows = Chosen_();", remove.Groups[1].Value, StringComparison.Ordinal);
         Assert.Contains("foreach (QueueRow row in rows)", remove.Groups[1].Value, StringComparison.Ordinal);
         Assert.Contains("Confirmations.RemoveQueuedMessages(rows.Count)", remove.Groups[1].Value, StringComparison.Ordinal);
         Assert.DoesNotContain("QueueGrid.SelectedItem is not QueueRow row)\n", remove.Groups[1].Value.Replace("\r\n", "\n"), StringComparison.Ordinal);

         // One message's verbs wait for exactly one row.
         Assert.Contains("bool one = QueueGrid.SelectedItems.Count == 1;", code, StringComparison.Ordinal);
         Assert.Contains("TraceButton.IsEnabled = one;", code, StringComparison.Ordinal);
         Assert.Contains("x:Name=\"RetryAllButton\"", xaml, StringComparison.Ordinal);
      }

      [Fact]
      public void TheQueue_SaysWhyEachWaits_AndLeadsToTheTrace()
      {
         string xaml = Read("QueueView.xaml");
         string code = Read("QueueView.xaml.cs");
         Assert.Matches(new Regex(@"DataGridTemplateColumn Header=""\{loc:L 'State'\}"" Width=""1\.2\*"" MinWidth=""150"" SortMemberPath=""StateOrder"""), xaml);
         Assert.Contains("<TextBlock Text=\"{Binding State}\" ToolTip=\"{Binding StateTip}\"", xaml, StringComparison.Ordinal);
         // The State holds what the Next try column said; with both, eight columns cut
         // the state and Tries at 1280 pixels in German.
         Assert.DoesNotContain("Header=\"{loc:L 'Next try'}\"", xaml, StringComparison.Ordinal);
         Assert.Contains("Header=\"{loc:L 'Tries'}\" Binding=\"{Binding Tries}\" Width=\"Auto\"", xaml, StringComparison.Ordinal);
         Assert.Contains("State = QueueWords.State(locked, columns[4], now, sameClock),", code, StringComparison.Ordinal);
         Assert.Contains("bool locked = columns[6] == \"1\";", code, StringComparison.Ordinal);
         Assert.Contains("trace.FollowQueuedMessage(id);", code, StringComparison.Ordinal);
         Assert.Contains("public void FollowQueuedMessage(long queueId)", Read("MessageTraceView.cs"), StringComparison.Ordinal);
      }

      /// <summary>
      /// Each row has its own Trace message, a 24-pixel icon named for its row (the
      /// owner's decision 15) by the page as it is loaded and given a row - not through a
      /// property of the row, which the page's search reads: "trace" matched every row.
      /// </summary>
      [Fact]
      public void EachRowsTraceIcon_IsNamedForItsRow_AndTheSearchDoesNotReadIt()
      {
         string xaml = Read("QueueView.xaml");
         string code = Read("QueueView.xaml.cs");
         Assert.Contains("Loaded=\"TraceRow_Named\" DataContextChanged=\"TraceRow_DataContextChanged\"", xaml, StringComparison.Ordinal);
         Assert.Contains("Click=\"TraceRow_Click\"", xaml, StringComparison.Ordinal);
         Assert.Contains("internal static string TraceName(string id) => F(\"Trace message {0}\", id);", code, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetName(button, name);", code, StringComparison.Ordinal);
         Assert.DoesNotContain("{Binding TraceName}", xaml, StringComparison.Ordinal);

         Match row = Regex.Match(code, @"public class QueueRow\s*\{(.*?)\n      \}", RegexOptions.Singleline);
         Assert.True(row.Success, "QueueRow is gone.");
         Assert.DoesNotContain("Trace", row.Groups[1].Value, StringComparison.Ordinal);
         Assert.Equal("Trace message 1042", Loc.F("Trace message {0}", "1042"));
      }

      [Fact]
      public void FromAndRecipients_HaveFloors_AndTheIdIsNarrow()
      {
         string xaml = Read("QueueView.xaml");
         Assert.Contains("Header=\"{loc:L 'From'}\" Binding=\"{Binding From}\" Width=\"*\" MinWidth=\"160\"", xaml, StringComparison.Ordinal);
         Assert.Contains("Header=\"{loc:L 'Recipients'}\" Binding=\"{Binding Recipients}\" Width=\"*\" MinWidth=\"160\"", xaml, StringComparison.Ordinal);
         // Left-aligned, as review finding F248 asks and the comment says: the fold's
         // 660fe6262 gave it the number styles to pass the fixed-width rule, which
         // right-aligned it (the final check of Tier 2, g3 defect 9); Auto over 56 passes it.
         Assert.Contains("<DataGridTextColumn Header=\"ID\" Binding=\"{Binding Id}\" Width=\"Auto\" MinWidth=\"56\" />", xaml, StringComparison.Ordinal);
         string id = xaml.Substring(xaml.IndexOf("<DataGridTextColumn Header=\"ID\"", StringComparison.Ordinal));
         id = id.Substring(0, id.IndexOf("/>", StringComparison.Ordinal));
         Assert.DoesNotContain("GridNumberText", id, StringComparison.Ordinal);
         Assert.DoesNotContain("GridNumberHeader", id, StringComparison.Ordinal);
      }

      private static string Read(string file) => FormLineAssert.ReadControlPanelFile(Path.Join("Views", file));
   }
}
