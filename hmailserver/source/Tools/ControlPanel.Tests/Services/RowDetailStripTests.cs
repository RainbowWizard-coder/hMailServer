// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The selected row's fact that only a tool tip held is on the page, under its
   /// list (review finding F286): why a message was held, what the receiving
   /// server answered for a suppressed address, an audit row's whole detail, and
   /// whether a traced event can be followed. A keyboard user never saw them and a
   /// screen reader user heard them, if at all, as the row's help text.
   /// </summary>
   public class RowDetailStripTests
   {
      [Fact]
      public void TheStrip_ShowsTheSelectedRowsFact_AndCollapsesWithoutOne()
      {
         string strip = RowSpeechTests.Read(Path.Join("Views", "RowDetailStrip.cs"));

         Assert.Contains("list.SelectionChanged += (_, _) => Show();", strip, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"if \(string\.IsNullOrWhiteSpace\(words\)\)\s*\{\s*card\.Visibility = Visibility\.Collapsed;"), strip);
         Assert.Contains("body.SetResourceReference(FrameworkElement.StyleProperty, \"TextBody\");", strip, StringComparison.Ordinal);
         Assert.Contains("card.Title = row == null ? null : caption?.Invoke(row);", strip, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("QuarantineView.cs", "RowDetailStrip.For(list_, _ => L(\"Why it was held\"), row => row is QuarantineRow { IsMessage: true } held ? held.Message.Reason : null)")]
      [InlineData("SuppressionListView.cs", "row => ((EntryView)row).ByHand ? L(\"Note\") : L(\"What the receiving server answered\"),")]
      [InlineData("AuditTrailView.cs", "RowDetailStrip.For(rows_, _ => L(\"Detail\"), row => ((RowView)row).Strip)")]
      [InlineData("MessageTraceView.cs", "RowDetailStrip.For(list_, null, row => ((TraceRow)row).FollowHint)")]
      public void EachPage_PutsTheFactUnderItsList(string file, string strip)
      {
         Assert.Contains(strip, RowSpeechTests.Read(Path.Join("Views", file)), StringComparison.Ordinal);
      }

      /// <summary>
      /// The strip is a part of the page's scrolling FillPanel after the list's
      /// card, which keeps a floor of three rows, so that the strip never takes
      /// the list's height: on the audit trail, in a grid's last row, it left the
      /// list no height at all in the narrowest window.
      /// </summary>
      [Theory]
      [InlineData("AuditTrailView.cs", "parts.Children.Add(RowDetailStrip.For(rows_")]
      [InlineData("MessageTraceView.cs", "parts.Children.Add(RowDetailStrip.For(list_")]
      [InlineData("QuarantineView.cs", "parts.Children.Add(RowDetailStrip.For(list_")]
      [InlineData("SuppressionListView.cs", "parts.Children.Add(RowDetailStrip.For(entries_")]
      public void TheStrip_FollowsTheListsFilledCard(string file, string strip)
      {
         string page = RowSpeechTests.Read(Path.Join("Views", file));
         int card = page.IndexOf("FillPanel.Filling(", StringComparison.Ordinal);
         int at = page.IndexOf(strip, StringComparison.Ordinal);
         Assert.True(card >= 0 && at > card, file + " puts its detail strip somewhere other than after the list's filling card.");
         Assert.Contains("FillPanel.ScrollingPage(out FillPanel parts)", page, StringComparison.Ordinal);
      }

      /// <summary>The rows keep their tool tips, which their automation peers give as the rows' help text.</summary>
      [Theory]
      [InlineData("QuarantineView.cs", "new Setter(ToolTipProperty, new Binding(nameof(QuarantineRow.HeldBecause)))")]
      [InlineData("AuditTrailView.cs", "new Setter(ToolTipProperty, new System.Windows.Data.Binding(nameof(RowView.Detail)))")]
      public void TheRowsKeepTheirToolTips(string file, string tip)
      {
         Assert.Contains(tip, RowSpeechTests.Read(Path.Join("Views", file)), StringComparison.Ordinal);
      }
   }
}
