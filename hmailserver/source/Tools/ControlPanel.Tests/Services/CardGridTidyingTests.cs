// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Card and grid tidying (T3.3 work item 11): the suppression list's two grids
   /// titled, linked and named in one vocabulary (review finding F318); the DKIM
   /// rotation's buttons on a surface of their own rather than the tab's last row
   /// over Save (F320); the TCP/IP ports and routes grids' fixed columns no wider
   /// than their words, with no header cut (F322); the Welcome page's intent rows
   /// that look pressable and say their blurb (F307); the setting profiles card's
   /// inset, empty state and note on its English (F308); and the domains' globe in
   /// the secondary text where it spent the brand blue on every row (F098). The
   /// WPF-free test project cannot draw them, so their source is held.
   /// </summary>
   public class CardGridTidyingTests
   {
      private static readonly XNamespace Xaml = "http://schemas.microsoft.com/winfx/2006/xaml";

      private static string View_(string file) => ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", file));

      private static XElement Named_(string file, string name)
         => XElement.Parse(View_(file)).Descendants().Single(e => (string)e.Attribute(Xaml + "Name") == name);

      [Fact]
      public void TheSuppressionList_TitlesBothCards_LinksTheGrids_AndNamesItsCategoriesOneWay()
      {
         string code = View_("SuppressionListView.cs");
         string build = WithheldRowTests.MemberText(code, "private void Build()");
         Assert.True(build != null, "SuppressionListView.Build is gone.");
         Assert.Contains("var listCard = new Card { Title = L(\"Suppressed addresses\"), Padding = new Thickness(DesignTokens.ListCardPadding), Content = host };", build, StringComparison.Ordinal);
         Assert.Contains("FillPanel.Filling(listCard, FillPanel.TitledListCardFloor, 2)", build, StringComparison.Ordinal);
         Assert.Contains("Title = L(\"Bounce counts\"),", build, StringComparison.Ordinal);
         // Each grid is named as its card is titled.
         Assert.Contains("AutomationProperties.SetName(entries_, L(\"Suppressed addresses\"));", build, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetName(counts_, L(\"Bounce counts\"));", build, StringComparison.Ordinal);
         Assert.Contains("entries_.SelectionChanged += (_, _) => SelectCountsOf_(entries_.SelectedItem as EntryView);", build, StringComparison.Ordinal);

         string select = WithheldRowTests.MemberText(code, "private void SelectCountsOf_(EntryView entry)");
         Assert.True(select != null, "SuppressionListView.SelectCountsOf_ is gone.");
         Assert.Contains("string.Equals(candidate.Address, entry.Address, StringComparison.OrdinalIgnoreCase)", select, StringComparison.Ordinal);
         Assert.Contains("counts_.ScrollIntoView(row);", select, StringComparison.Ordinal);
         // The counts' row is brought into view in the counts' grid and not the page,
         // which a short window scrolled down to the counts, the list's row just
         // clicked going off screen - unless the keyboard is in the counts.
         int bring = build.IndexOf("counts_.RequestBringIntoView += (_, e) =>", StringComparison.Ordinal);
         Assert.True(bring >= 0, "The counts' bring-into-view is passed on to the page again.");
         string handler = build.Substring(bring, build.IndexOf("};", bring, StringComparison.Ordinal) - bring);
         Assert.Contains("if (!counts_.IsKeyboardFocusWithin)", handler, StringComparison.Ordinal);
         Assert.Contains("e.Handled = true;", handler, StringComparison.Ordinal);

         // The counts' headers are the list's Reason words, and the two grids say
         // Reason and Status code where they said Why, Status, Soft and Policy.
         string columns = WithheldRowTests.MemberText(code, "private void BuildColumns_()");
         Assert.True(columns != null, "SuppressionListView.BuildColumns_ is gone.");
         foreach (string category in new[] { "hard", "soft", "mailbox_full", "policy", "other" })
            Assert.Contains("Number_(CategoryText_(\"" + category + "\")", columns, StringComparison.Ordinal);
         foreach (string header in new[] { "L(\"Reason\")", "L(\"Status code\")" })
            Assert.Contains(header, columns, StringComparison.Ordinal);
         // The last status code is the selected address's, in the strip under the counts.
         Assert.Contains("F(\"Last status code: {0}\", LastStatus)", code, StringComparison.Ordinal);
         foreach (string old in new[] { "L(\"Why\")", "L(\"Status\")", "L(\"Hard\")", "L(\"Soft\")", "L(\"Policy\")", "L(\"Last status\")" })
            Assert.DoesNotContain(old, code, StringComparison.Ordinal);

         // Under those names the counts can need more than a narrow window gives them,
         // so the grid scrolls sideways there, and its address stays in view as it does.
         int counts = code.IndexOf("private readonly DataGrid counts_ = new()", StringComparison.Ordinal);
         Assert.True(counts >= 0, "SuppressionListView.counts_ is gone.");
         string countsInit = code.Substring(counts, code.IndexOf("};", counts, StringComparison.Ordinal) - counts);
         Assert.Contains("FrozenColumnCount = 1", countsInit, StringComparison.Ordinal);
         Assert.StartsWith("counts_.Columns.Add(Trimmed_(L(\"Address\")", columns.Substring(columns.IndexOf("counts_.Columns.Add(", StringComparison.Ordinal)), StringComparison.Ordinal);

         // The Add dialog's hint says what cannot be added, and its expiry is a short box.
         Assert.Contains("L(\"Addresses on this server's own domains cannot be added - the list applies only to mail leaving the server.\")", code, StringComparison.Ordinal);
         Assert.Contains("MaxDecimalPlaces = 0, Width = 160, HorizontalAlignment = HorizontalAlignment.Left", code, StringComparison.Ordinal);
      }

      [Fact]
      public void TheDkimRotationsButtons_AreOnASurfaceOfTheirOwn_NotTheTabsLastRow()
      {
         string rotation = WithheldRowTests.MemberText(View_("DomainDialog.cs"), "private SettingsSection BuildRotation()");
         Assert.True(rotation != null, "DomainDialog.BuildRotation is gone.");
         Assert.Contains("now.Children.Add(rotStatus_);", rotation, StringComparison.Ordinal);
         Assert.Contains("now.Children.Add(buttons);", rotation, StringComparison.Ordinal);
         Assert.Contains("section.Children.Add(new Scaffold.Card", rotation, StringComparison.Ordinal);
         Assert.DoesNotContain("section.Children.Add(buttons);", rotation, StringComparison.Ordinal);
         // A section of the tab, with the gap under it that every section keeps.
         Assert.Contains("return new SettingsSection", rotation, StringComparison.Ordinal);
         Assert.Contains("Heading = L(\"Key rotation\"),", rotation, StringComparison.Ordinal);
      }

      [Fact]
      public void ThePortsGrid_GivesTheCertificateTheRoom_AndCallsItsAuthColumnAuth()
      {
         Assert.Equal("130", (string)Named_("TcpIpPortsView.xaml", "AddressColumn").Attribute("Width"));
         Assert.Equal("150", (string)Named_("TcpIpPortsView.xaml", "SecurityColumn").Attribute("Width"));
         Assert.Equal("130", (string)Named_("TcpIpPortsView.xaml", "ListeningColumn").Attribute("Width"));
         XElement auth = Named_("TcpIpPortsView.xaml", "AuthColumn");
         Assert.Equal("AUTH", (string)auth.Attribute("Header"));
         Assert.Equal("90", (string)auth.Attribute("Width"));

         XElement certificate = XElement.Parse(View_("TcpIpPortsView.xaml")).Descendants()
            .Single(e => e.Name.LocalName == "DataGridTextColumn" && (string)e.Attribute("Header") == "{loc:L 'Certificate'}");
         Assert.Equal("*", (string)certificate.Attribute("Width"));
         Assert.Equal("160", (string)certificate.Attribute("MinWidth"));

         // The words the header no longer says are its tool tip and help text.
         Assert.Contains("GridStyles.HeaderCaption(AuthColumn, L(\"SMTP authentication\"));", View_("TcpIpPortsView.xaml.cs"), StringComparison.Ordinal);
      }

      [Fact]
      public void TheRoutesGrid_CutsNoHeader_AndItsRowSaysTheRetriesAndTheSignIn()
      {
         XElement grid = XElement.Parse(View_("RoutesView.xaml")).Descendants().Single(e => (string)e.Attribute(Xaml + "Name") == "RouteGrid");
         XElement retries = grid.Descendants().Single(e => (string)e.Attribute("Header") == "{loc:L 'Retries'}");
         Assert.Equal("Auto", (string)retries.Attribute("Width"));
         Assert.Equal("80", (string)retries.Attribute("MinWidth"));
         XElement authentication = grid.Descendants().Single(e => (string)e.Attribute("Header") == "{loc:L 'Authentication'}");
         Assert.Equal("130", (string)authentication.Attribute("Width"));
         Assert.DoesNotContain("{loc:L 'Auth'}", View_("RoutesView.xaml"), StringComparison.Ordinal);

         string code = View_("RoutesView.xaml.cs");
         Assert.Contains("column.MinWidth = Math.Max(column.MinWidth, GridStyles.HeaderWidth(column.Header as string));", code, StringComparison.Ordinal);

         // A header's width as the window draws it, in its formatting mode and at its
         // DPI, as a cell's is: by ideal metrics a header of Latin letters came out a
         // pixel or two narrower than drawn at 150 %, and a column floored at it was cut.
         string width = WithheldRowTests.MemberText(ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Services", "GridStyles.cs")), "public static double HeaderWidth(string header)");
         Assert.True(width != null, "GridStyles.HeaderWidth is gone.");
         Assert.Contains("TextOptions.GetTextFormattingMode(window)", width, StringComparison.Ordinal);
         Assert.Contains("VisualTreeHelper.GetDpi(window).PixelsPerDip", width, StringComparison.Ordinal);
         Assert.Contains("Brushes.Black, null, mode, pixelsPerDip)", width, StringComparison.Ordinal);
         // And in whole device pixels, up: at 150 % a column floored at 147 was drawn
         // 146.67 wide and cut its header by a third of a pixel.
         Assert.Contains("return WholeDevicePixels(words.WidthIncludingTrailingWhitespace + padding.Left + padding.Right);", width, StringComparison.Ordinal);
         string styles = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Services", "GridStyles.cs"));
         string whole = WithheldRowTests.MemberText(styles, "public static double WholeDevicePixels(double width)");
         Assert.True(whole != null, "GridStyles.WholeDevicePixels is gone.");
         Assert.Contains("Math.Ceiling(width * pixelsPerDip - 1e-6) / pixelsPerDip", whole, StringComparison.Ordinal);
         string words = WithheldRowTests.MemberText(styles, "public static void FloorAtTheirWords(DataGrid grid, System.Collections.IEnumerable items, params DataGridColumn[] except)");
         Assert.True(words != null, "GridStyles.FloorAtTheirWords is gone.");
         Assert.Contains("floor = Math.Max(floor, CellWidth(value as string ?? value?.ToString()));", words, StringComparison.Ordinal);
         Assert.Contains("column.MinWidth = WholeDevicePixels(floor);", words, StringComparison.Ordinal);
         Assert.Contains("RowSpeech.Labelled(L(\"Retries\"),", code, StringComparison.Ordinal);
         Assert.Contains("RowSpeech.Labelled(L(\"Authentication\"), Auth)", code, StringComparison.Ordinal);
      }

      [Fact]
      public void TheWelcomeIntents_LookPressable_SayTheirBlurb_AndEndWithTheNotices()
      {
         string code = View_("WelcomeView.cs");
         string row = WithheldRowTests.MemberText(code, "private static Wpf.Ui.Controls.Button IntentRow(WelcomeIntent intent)");
         Assert.True(row != null, "WelcomeView.IntentRow is gone.");
         Assert.Contains("SymbolRegular.ChevronRight20", row, StringComparison.Ordinal);
         Assert.Contains("chevron.SetResourceReference(ForegroundProperty, \"TextFillColorTertiaryBrush\");", row, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetHelpText(btn, L(intent.Blurb));", row, StringComparison.Ordinal);

         // The two grids reach past their cells' right and bottom margins, so that the
         // right column ends where the notices above them do.
         Assert.Contains("Columns = 2, Margin = new Thickness(0, 0, -DesignTokens.Space.Md, -DesignTokens.Space.Md)", code, StringComparison.Ordinal);
         Assert.Contains("areas_.Margin = new Thickness(0, 0, -DesignTokens.Space.Md, -DesignTokens.Space.Md);", code, StringComparison.Ordinal);

         // An offer is at the Normal level: the walk's, and the sample domain's while there is none.
         int offer = code.IndexOf("private readonly InlineNotice tourOffer_ = new()", StringComparison.Ordinal);
         Assert.True(offer >= 0, "WelcomeView has no tourOffer_.");
         Assert.Contains("Level = StatusLevel.Normal,", code.Substring(offer, 200), StringComparison.Ordinal);
         string sample = WithheldRowTests.MemberText(code, "private void ShowSample_(string json)");
         Assert.True(sample != null, "WelcomeView.ShowSample_ is gone.");
         // The sample domain's offer is its card's description (t213a, F275), no notice at all.
         Assert.Contains("sampleNotice_.Hide();", sample, StringComparison.Ordinal);
         Assert.DoesNotContain("StatusLevel.Information", sample, StringComparison.Ordinal);
      }

      /// <summary>The brand blue marks the selection; a row's glyph is the secondary text, the domains' as the accounts'.</summary>
      [Fact]
      public void TheDomainsGlyph_IsTheSecondaryText_AsTheAccountsIs()
      {
         XElement page = XElement.Parse(View_("DomainsView.xaml"));
         XNamespace ui = "http://schemas.lepo.co/wpfui/2022/xaml";
         foreach (string symbol in new[] { "Globe24", "Person24" })
         {
            XElement glyph = page.Descendants(ui + "SymbolIcon").Single(e => (string)e.Attribute("Symbol") == symbol);
            Assert.Equal("{DynamicResource TextFillColorSecondaryBrush}", (string)glyph.Attribute("Foreground"));
         }
         Assert.DoesNotContain("AppBrandBrush", View_("DomainsView.xaml"), StringComparison.Ordinal);
      }

      [Fact]
      public void TheProfilesCard_TakesTheCardsInset_SaysWhyItsTableIsEmpty_AndThatItsWordsAreEnglish()
      {
         string code = View_("ProfilesView.cs");
         string build = WithheldRowTests.MemberText(code, "private void Build()");
         Assert.True(build != null, "ProfilesView.Build is gone.");
         Assert.Contains("var card = new Card { Content = body };", build, StringComparison.Ordinal);
         Assert.DoesNotContain("Padding = new Thickness(8)", build, StringComparison.Ordinal);
         Assert.Contains("EmptyStateUnderHeaders.Keep(empty_, rows_);", build, StringComparison.Ordinal);
         // Under the table and its steps, where above it the note took one of the five rows.
         Assert.Contains("bottom.Children.Add(englishNote_);", build, StringComparison.Ordinal);
         Assert.DoesNotContain("body.Children.Add(englishNote_);", build, StringComparison.Ordinal);
         Assert.True(build.IndexOf("bottom.Children.Add(steps_);", StringComparison.Ordinal) < build.IndexOf("bottom.Children.Add(englishNote_);", StringComparison.Ordinal),
            "The English words' note is above the steps it speaks of.");
         // Every column floored at its header, the two star columns among them, after
         // the columns are added: German cut "Verlangt eine Bestätigung" at 1280.
         int floor = build.IndexOf("foreach (DataGridColumn column in rows_.Columns)", StringComparison.Ordinal);
         Assert.True(floor >= 0, "ProfilesView.Build no longer floors its columns at their headers.");
         Assert.Contains("column.MinWidth = Math.Max(column.MinWidth, GridStyles.HeaderWidth(column.Header as string));", build.Substring(floor), StringComparison.Ordinal);
         Assert.True(floor > build.IndexOf("rows_.Columns.Add(note_);", StringComparison.Ordinal),
            "ProfilesView floors its columns before the last is added.");
         // Why keeps a reason's first words, where floored at its header alone it was
         // 61 pixels at 760; the grid then scrolls sideways and keeps its upright bar.
         Assert.Contains("why_.MinWidth = Math.Max(why_.MinWidth, 100);", build, StringComparison.Ordinal);
         Assert.Contains("ScrollViewer.SetVerticalScrollBarVisibility(rows_, ScrollBarVisibility.Visible);", build, StringComparison.Ordinal);
         // And the columns before the two are floored at their words once a profile is
         // shown, which a grid narrowed after it was laid out wider would otherwise cut.
         string shown = WithheldRowTests.MemberText(code, "private void ShowDiff_()");
         Assert.True(shown != null, "ProfilesView.ShowDiff_ is gone.");
         Assert.Contains("GridStyles.FloorAtTheirWords(rows_, rows, why_, note_);", shown, StringComparison.Ordinal);
         Assert.True(shown.IndexOf("rows_.ItemsSource = rows;", StringComparison.Ordinal) < shown.IndexOf("GridStyles.FloorAtTheirWords(", StringComparison.Ordinal),
            "ProfilesView floors its columns before the rows they are floored at are shown.");

         string nothing = WithheldRowTests.MemberText(code, "private void ShowNothingToCompare_(bool unread)");
         Assert.True(nothing != null, "ProfilesView.ShowNothingToCompare_ is gone.");
         Assert.Contains("L(\"Nothing to compare until the server can be read.\")", nothing, StringComparison.Ordinal);
         Assert.Contains("L(\"Choose a profile to compare it with this server.\")", nothing, StringComparison.Ordinal);

         string diff = WithheldRowTests.MemberText(code, "private void ShowDiff_()");
         Assert.True(diff != null, "ProfilesView.ShowDiff_ is gone.");
         Assert.Contains("ShowNothingToCompare_(listUnread_);", diff, StringComparison.Ordinal);
         Assert.Contains("ShowNothingToCompare_(true);", diff, StringComparison.Ordinal);
         // With steps, the note ends the steps' heading line in the caption's size, where
         // on a line of its own under the steps it still took one of the table's five rows
         // at 1280 by 800 in Japanese (the final check of Tier 2's renders); without
         // steps it is the caption under them.
         Assert.Contains("bool noteOnHeading = stepLines.Count > 0 && Loc.Translated;", diff, StringComparison.Ordinal);
         Assert.Contains("var note = new System.Windows.Documents.Run(englishNote_.Text);", diff, StringComparison.Ordinal);
         Assert.Contains("note.SetResourceReference(System.Windows.Documents.TextElement.FontSizeProperty, \"AppFontSizeCaption\");", diff, StringComparison.Ordinal);
         Assert.Contains("englishNote_.Visibility = Loc.Translated && !noteOnHeading ? Visibility.Visible : Visibility.Collapsed;", diff, StringComparison.Ordinal);
      }
   }
}
