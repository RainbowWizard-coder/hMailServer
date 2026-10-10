// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Discovery page (review finding F291): the lines' card is padded as every
   /// titled card is, says what it lacks where it has no lines to show, the counts
   /// are right-aligned, a ring says the cases are being read, and both grids keep
   /// three rows in the smallest window. The page reads the REST API, which this
   /// assembly does not run, so its lines are read here.
   /// </summary>
   public class DiscoveryPageTests
   {
      [Theory]
      [InlineData("Searches", "CaseRow.Searches")]
      [InlineData("Live holds", "CaseRow.LiveHolds")]
      [InlineData("Exports", "CaseRow.Exports")]
      public void TheCounts_AreRightAligned(string header, string path)
      {
         string page = Page();
         Assert.Contains("cases_.Columns.Add(Count_(L(\"" + header + "\"), nameof(" + path + ")));", page, StringComparison.Ordinal);

         string count = WithheldRowTests.MemberText(page, "private static DataGridTextColumn Count_(string header, string path)");
         Assert.True(count != null, "DiscoveryView.Count_ is gone.");
         Assert.Contains("GridStyles.Number(column);", count, StringComparison.Ordinal);
      }

      /// <summary>The case's name and a line's detail share the width over a floor, trimmed: with none, the smallest window gave the name nothing.</summary>
      [Fact]
      public void TheNameAndTheDetail_HaveAFloor()
      {
         string page = Page();
         Assert.Contains("cases_.Columns.Add(Shared_(L(\"Name\"), nameof(CaseRow.Name), 160));", page, StringComparison.Ordinal);
         Assert.Contains("lines_.Columns.Add(Shared_(L(\"Detail\"), nameof(LineRow.Detail), 140));", page, StringComparison.Ordinal);

         string shared = WithheldRowTests.MemberText(page, "private static DataGridTextColumn Shared_(string header, string path, double floor)");
         Assert.True(shared != null, "DiscoveryView.Shared_ is gone.");
         Assert.Contains("GridStyles.Trimmed(", shared, StringComparison.Ordinal);
         Assert.Contains("column.MinWidth = Math.Max(floor, GridStyles.HeaderWidth(header));", shared, StringComparison.Ordinal);
      }

      /// <summary>The lines' card takes the Card's own padding, AppCardPadding, so that its title stands where every titled card's does.</summary>
      [Fact]
      public void TheLinesCard_IsPaddedAsEveryTitledCardIs()
      {
         string layout = Layout();
         int at = layout.IndexOf("linesCard_ = new Card", StringComparison.Ordinal);
         Assert.True(at >= 0, "DiscoveryView builds no lines card.");
         string card = layout.Substring(at, layout.IndexOf("};", at, StringComparison.Ordinal) - at);
         Assert.Contains("Title = L(\"The case's lines on the audit trail\")", card, StringComparison.Ordinal);
         Assert.DoesNotContain("Padding", card, StringComparison.Ordinal);
      }

      /// <summary>Both grids are three rows on a FillPanel of their own, and their cards are floored at what they hold.</summary>
      [Fact]
      public void BothGrids_KeepThreeRows()
      {
         string layout = Layout();
         Assert.Contains("caseRows.Children.Add(FillPanel.Filling(cases_, FillPanel.ListRowsFloor));", layout, StringComparison.Ordinal);
         Assert.Contains("lineRows.Children.Add(FillPanel.Filling(lines_, FillPanel.ListRowsFloor));", layout, StringComparison.Ordinal);
         Assert.Contains("parts.Children.Add(FillPanel.Filling(casesCard, 0));", layout, StringComparison.Ordinal);
         Assert.Contains("parts.Children.Add(FillPanel.Filling(linesCard_, 0));", layout, StringComparison.Ordinal);
         Assert.DoesNotContain("TitledListCardFloor", layout, StringComparison.Ordinal);
         Assert.Contains("EmptyStateUnderHeaders.Keep(noLines_, lines_);", layout, StringComparison.Ordinal);
      }

      /// <summary>Where no case is selected, or the one selected has no lines, the lines' card says so where its rows would be.</summary>
      [Fact]
      public void TheLines_SayWhatIsMissing()
      {
         string show = WithheldRowTests.MemberText(Page(), "private void ShowLines_()");
         Assert.True(show != null, "DiscoveryView.ShowLines_ is gone.");
         Assert.Contains("noLines_.Text = L(\"Select a case above to see its lines on the audit trail.\");", show, StringComparison.Ordinal);
         Assert.Contains("noLines_.Text = L(\"The audit trail holds no line for this case.\");", show, StringComparison.Ordinal);
         Assert.Contains("noLines_.Visibility = lines.Count > 0 ? Visibility.Collapsed : Visibility.Visible;", show, StringComparison.Ordinal);

         string present = WithheldRowTests.MemberText(Page(), "private void Present_(IReadOnlyList<DiscoveryCase> cases, List<AuditRow> audit, string auditNote)");
         Assert.True(present != null, "DiscoveryView.Present_ is gone.");
         Assert.Contains("linesCard_.Visibility = loaded_.Count > 0 ? Visibility.Visible : Visibility.Collapsed;", present, StringComparison.Ordinal);
      }

      /// <summary>The ring is shown from the first request and taken away whatever the read came to.</summary>
      [Fact]
      public void ARead_ShowsTheRing_UntilItEnds()
      {
         string page = Page();
         Assert.Contains("private readonly LoadingState reading_ = new() { Text = L(\"Reading the cases…\"), Visibility = Visibility.Collapsed };", page, StringComparison.Ordinal);

         string reload = WithheldRowTests.MemberText(page, "private async void Reload_()");
         Assert.True(reload != null, "DiscoveryView.Reload_ is gone.");
         int shown = reload.IndexOf("Reading_(true);", StringComparison.Ordinal);
         int request = reload.IndexOf("await Get_(", StringComparison.Ordinal);
         int hidden = reload.LastIndexOf("Reading_(false);", StringComparison.Ordinal);
         int final = reload.IndexOf("finally", StringComparison.Ordinal);
         Assert.True(shown >= 0 && request > shown, "Reload_ asks the REST API before it shows the ring.");
         Assert.True(final >= 0 && hidden > final, "Reload_ does not take the ring away in its finally.");
      }

      private static string Page() => ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "DiscoveryView.cs"));

      private static string Layout()
      {
         string layout = WithheldRowTests.MemberText(Page(), "private void Build_()");
         Assert.True(layout != null, "DiscoveryView.Build_ is gone.");
         return layout;
      }
   }
}
