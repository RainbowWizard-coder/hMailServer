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
   /// The suppression list page in a 1280 by 800 window: the address column took
   /// two stars and about 670 pixels, Source read "Refuse" and "An ad", and Since
   /// and Expires were off the card, so when an entry lapses could not be seen;
   /// the bounce counts' address was cut to "gone@old-provider.exampl" with no
   /// ellipsis while its date columns had room to spare; and the domain box sized
   /// itself to the longest domain name, so in the narrowest window it filled the
   /// toolbar, its own chevron cut off, and Add…, Remove and Refresh had no width
   /// left and were not drawn. Every test here fails against that page: the three
   /// actions are in the page header, the domain box is captioned and capped, and
   /// every column keeps a width, the names trimmed with an ellipsis.
   /// </summary>
   public class SuppressionListPageTests
   {
      /// <summary>
      /// The list page pattern (review finding F288): Remove and then Add…, the
      /// Primary, in the toolbar over the list, the count and Refresh in the
      /// header. The actions were put in the header because beside the domain box
      /// they had no width in the narrowest window; the toolbar puts them on a row
      /// of their own there now, so that they are drawn whatever the width.
      /// </summary>
      [Fact]
      public void TheActions_AreInTheToolbar_AndRefreshAndTheCountInTheHeader()
      {
         string build = Build();
         Assert.Contains("toolbar_.Actions = actions;", build, StringComparison.Ordinal);
         Assert.Contains("Status = count_,", build, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"Actions = MakeButton\(L\(""_Refresh""\)"), build);

         int remove = build.IndexOf("L(\"Re_move\")", StringComparison.Ordinal);
         int add = build.IndexOf("L(\"_Add…\")", StringComparison.Ordinal);
         int header = build.IndexOf("root.Children.Add(new PageHeader", StringComparison.Ordinal);
         Assert.True(remove >= 0 && add > remove && header > add, "Remove, then Add…, the Primary, are not the toolbar's actions in that order.");

         string toolbar = ReadControlPanelFile(Path.Join("Views", "Scaffold", "Toolbar.cs"));
         Assert.Contains("Services.ToolbarRows.ActionsUnder(", toolbar, StringComparison.Ordinal);
      }

      [Fact]
      public void TheDomainBox_IsCaptionedWithAnAltKey_AndCapped()
      {
         string code = Source();
         Assert.Contains("new Label { Content = L(\"Sending _domain\"), Target = domainChoice_", code, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.SetName(domainChoice_, MnemonicText.Strip(L(\"Sending _domain\")))", code, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"domainChoice_ = new\(\) \{ MinWidth = 220, MaxWidth = 320 \}"), code);

         // A name wider than the box ends in an ellipsis, whole in its tool tip.
         Assert.Contains("domainChoice_.ItemTemplate = TrimmedItem_();", code, StringComparison.Ordinal);
         string template = WithheldRowTests.MemberText(code, "private static DataTemplate TrimmedItem_()");
         Assert.True(template != null, "TrimmedItem_ is gone.");
         Assert.Contains("TextTrimming.CharacterEllipsis", template, StringComparison.Ordinal);
         Assert.Contains("SetBinding(ToolTipProperty", template, StringComparison.Ordinal);
      }

      /// <summary>
      /// The list's columns: the address and the source share the width over
      /// floors, trimmed; Why is as wide as its words over a floor of the widest,
      /// trimmed too ("Harte Unzustellbarkeit" in German); Status, Since and
      /// Expires are as wide as theirs over floors of their widest. At a fixed
      /// 140 and 150 pixels, 150 per cent text cut Why to "Added by h…" and the
      /// dates mid-glyph (review finding F317).
      /// </summary>
      [Fact]
      public void EveryColumnOfTheList_KeepsAWidth_AndTheNamesAreTrimmed()
      {
         string columns = WithheldRowTests.MemberText(Source(), "private void BuildColumns_()");
         Assert.True(columns != null, "BuildColumns_ is gone.");

         Assert.Contains("Trimmed_(L(\"Address\"), nameof(EntryView.Address), new DataGridLength(1, DataGridLengthUnitType.Star), 200)", columns, StringComparison.Ordinal);
         Assert.Contains("Trimmed_(L(\"Reason\"), nameof(EntryView.Why), DataGridLength.Auto, WidestCategory_())", columns, StringComparison.Ordinal);
         Assert.Contains("Fitted_(L(\"Status code\"), nameof(EntryView.Status), 84)", columns, StringComparison.Ordinal);
         Assert.Contains("Trimmed_(L(\"Source\"), nameof(EntryView.Source), new DataGridLength(1, DataGridLengthUnitType.Star), 160)", columns, StringComparison.Ordinal);
         Assert.Contains("sinceColumn_ = Dated_(L(\"Since (server time)\"), nameof(EntryView.Since), nameof(EntryView.SinceAt));", columns, StringComparison.Ordinal);
         Assert.Contains("expiresColumn_ = Dated_(L(\"Expires (server time)\"), nameof(EntryView.Expires), nameof(EntryView.ExpiresAt));", columns, StringComparison.Ordinal);

         // No column of the list, or of the counts, sizes itself to its cells with no
         // floor, which is what let the address take the width the dates needed, and
         // none is a fixed number of pixels, which larger text cuts short.
         Assert.DoesNotContain("new DataGridLength(2, DataGridLengthUnitType.Star)", columns, StringComparison.Ordinal);
         Assert.DoesNotMatch(@"new DataGridLength\(\d+\)", Source());
         string widest = WithheldRowTests.MemberText(Source(), "private static double WidestCategory_()");
         Assert.True(widest != null && widest.Contains("GridStyles.CellWidth(CategoryText_(category))", StringComparison.Ordinal), "Why's floor is not its widest word.");

         // A column of short codes is its floor or its header's width, whichever
         // is more; the floor holds "ステータス" on its own (84: five 12-pixel glyphs
         // in semibold and the header's 20 pixels of padding), since the header is
         // not always counted once the grid scrolls sideways, and at 64 the
         // smallest window read "ステー…".
         string fitted = WithheldRowTests.MemberText(Source(), "private static DataGridTextColumn Fitted_(string header, string path, double minWidth)");
         Assert.True(fitted != null && fitted.Contains("Width = DataGridLength.Auto, MinWidth = Floor_(header, minWidth)", StringComparison.Ordinal), "A fitted column has no floor.");
      }

      [Fact]
      public void TheCountsColumns_KeepAWidth_AndTheAddressIsTrimmed()
      {
         string code = Source();
         string columns = WithheldRowTests.MemberText(code, "private void BuildColumns_()");
         Assert.Contains("Trimmed_(L(\"Address\"), nameof(CountView.Address), new DataGridLength(1, DataGridLengthUnitType.Star), 180)", columns, StringComparison.Ordinal);
         Assert.Contains("lastBounceColumn_ = Dated_(L(\"Last bounce (server time)\"), nameof(CountView.LastBounce), nameof(CountView.LastBounceAt));", columns, StringComparison.Ordinal);
         Assert.Contains("lastDeliveredColumn_ = Dated_(L(\"Last delivered (server time)\"), nameof(CountView.LastDelivered), nameof(CountView.LastDeliveredAt));", columns, StringComparison.Ordinal);
         Assert.Contains("Fitted_(L(\"Listed\"), nameof(CountView.Listed), GridStyles.UpToDevicePixels(GridStyles.CellWidth(L(\"Listed\"))))", columns, StringComparison.Ordinal);

         // The six counts: 64 pixels, or the header's width where that is more, so
         // that "Mailbox full" is not cut to "Mailb…".
         string number = WithheldRowTests.MemberText(code, "private static DataGridTextColumn Number_(string header, string path)");
         Assert.True(number != null && number.Contains("Fitted_(header, path, 64)", StringComparison.Ordinal), "A count column has no floor of 64 pixels.");

         // A column of dates is floored at its header and, once the rows are read,
         // at its widest date, so a narrow window scrolls the grid sideways rather
         // than squeezing the dates.
         string dated = WithheldRowTests.MemberText(code, "private static DataGridTextColumn Dated_(string header, string path, string sortPath)");
         Assert.True(dated != null && dated.Contains("Width = DataGridLength.Auto, MinWidth = Floor_(header, 0)", StringComparison.Ordinal), "A column of dates has no floor.");
         string floor = WithheldRowTests.MemberText(code, "private static void FloorAtCells_<T>(DataGridTextColumn column, IEnumerable<T> rows, Func<T, string> cell)");
         Assert.True(floor != null && floor.Contains("floor = Math.Max(floor, GridStyles.CellWidth(cell(row)));", StringComparison.Ordinal), "A column of dates is not floored at its widest date.");
         string reload = WithheldRowTests.MemberText(code, "private void Reload()");
         foreach (string column in new[] { "sinceColumn_, entries, e => e.Since", "expiresColumn_, entries, e => e.Expires", "lastBounceColumn_, countRows, c => c.LastBounce", "lastDeliveredColumn_, countRows, c => c.LastDelivered" })
            Assert.Contains("FloorAtCells_(" + column + ");", reload, StringComparison.Ordinal);
      }

      /// <summary>
      /// No column of either grid is narrower than its own header, measured in the
      /// header's font: an Auto column's header is not always counted once the grid
      /// is narrower than its columns and scrolls sideways, and a fixed column's is
      /// cut wherever a translation is wider than the column, so in the smallest
      /// window the counts read "メール…" for メールボックス満杯 (Mailbox full) over
      /// their 64-pixel floor and "最後のステ…" for 最後のステータス (Last status)
      /// in its 90-pixel column.
      /// </summary>
      [Fact]
      public void NoColumn_IsNarrowerThanItsHeader()
      {
         string code = Source();
         Assert.Contains("private static double Floor_(string header, double floor) => Math.Max(floor, GridStyles.HeaderWidth(header));", code, StringComparison.Ordinal);
         foreach (string member in new[]
         {
            "private static DataGridTextColumn Dated_(string header, string path, string sortPath)",
            "private static DataGridTextColumn Trimmed_(string header, string path, DataGridLength width, double minWidth)",
            "private static DataGridTextColumn Fitted_(string header, string path, double minWidth)",
         })
         {
            string text = WithheldRowTests.MemberText(code, member);
            Assert.True(text != null && text.Contains("Floor_(header, ", StringComparison.Ordinal), member + " does not floor its column at its header's width.");
         }
      }

      /// <summary>
      /// A header's width is its words in the header's font - the application's
      /// DataGridColumnHeader style's size and weight, 12 pixels semibold, in the
      /// window's family - and the style's padding. GridStyles is WPF and is not
      /// compiled into the tests, so the measure is held by its text.
      /// </summary>
      [Fact]
      public void AHeadersWidth_IsItsWordsInTheHeaderFont_AndThePadding()
      {
         string width = WithheldRowTests.MemberText(ReadControlPanelFile(Path.Join("Services", "GridStyles.cs")), "public static double HeaderWidth(string header)");
         Assert.True(width != null, "GridStyles.HeaderWidth is gone, so no column can be floored at its header.");
         Assert.Contains("TryFindResource(typeof(System.Windows.Controls.Primitives.DataGridColumnHeader))", width, StringComparison.Ordinal);
         Assert.Contains("Setting(style, Control.FontSizeProperty, 12.0)", width, StringComparison.Ordinal);
         Assert.Contains("Setting(style, Control.FontWeightProperty, FontWeights.SemiBold)", width, StringComparison.Ordinal);
         Assert.Contains("Setting(style, Control.PaddingProperty, new Thickness(10, 8, 10, 8))", width, StringComparison.Ordinal);
         Assert.Contains("new FormattedText(header, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,", width, StringComparison.Ordinal);
         Assert.Contains("WholeDevicePixels(words.WidthIncludingTrailingWhitespace + padding.Left + padding.Right)", width, StringComparison.Ordinal);
      }

      private static string Build() => WithheldRowTests.MemberText(Source(), "private void Build()");

      private static string Source() => ReadControlPanelFile(Path.Join("Views", "SuppressionListView.cs"));

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
