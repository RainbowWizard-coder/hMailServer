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
      [Fact]
      public void TheActions_AreInThePageHeader_NotTheToolbar()
      {
         string build = Build();
         Assert.Contains("Actions = actions", build, StringComparison.Ordinal);
         Assert.DoesNotContain("toolbar_.Actions", build, StringComparison.Ordinal);

         // Add…, Remove and Refresh, built before the header that holds them.
         int header = build.IndexOf("root.Children.Add(new PageHeader", StringComparison.Ordinal);
         foreach (string caption in new[] { "L(\"_Add…\")", "L(\"_Remove\")", "L(\"Re_fresh\")" })
         {
            int at = build.IndexOf(caption, StringComparison.Ordinal);
            Assert.True(at >= 0 && at < header, caption + " is not built for the page header.");
         }
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
      /// floors, trimmed; Why is 140 pixels, trimmed too ("Harte
      /// Unzustellbarkeit" in German); Status, Since and Expires keep theirs.
      /// </summary>
      [Fact]
      public void EveryColumnOfTheList_KeepsAWidth_AndTheNamesAreTrimmed()
      {
         string columns = WithheldRowTests.MemberText(Source(), "private void BuildColumns_()");
         Assert.True(columns != null, "BuildColumns_ is gone.");

         Assert.Contains("Trimmed_(L(\"Address\"), nameof(EntryView.Address), new DataGridLength(1, DataGridLengthUnitType.Star), 200)", columns, StringComparison.Ordinal);
         Assert.Contains("Trimmed_(L(\"Why\"), nameof(EntryView.Why), new DataGridLength(140), 140)", columns, StringComparison.Ordinal);
         Assert.Contains("Fitted_(L(\"Status\"), nameof(EntryView.Status), 84)", columns, StringComparison.Ordinal);
         Assert.Contains("Trimmed_(L(\"Source\"), nameof(EntryView.Source), new DataGridLength(1, DataGridLengthUnitType.Star), 160)", columns, StringComparison.Ordinal);
         Assert.Contains("Text_(L(\"Since\"), nameof(EntryView.Since), new DataGridLength(150))", columns, StringComparison.Ordinal);
         Assert.Contains("Text_(L(\"Expires\"), nameof(EntryView.Expires), new DataGridLength(150))", columns, StringComparison.Ordinal);

         // No column of the list, or of the counts, is left to size itself to its
         // cells with no floor, which is what let the address take the width the
         // dates needed.
         Assert.DoesNotContain("DataGridLength.Auto)", columns, StringComparison.Ordinal);
         Assert.DoesNotContain("new DataGridLength(2, DataGridLengthUnitType.Star)", columns, StringComparison.Ordinal);

         // A column of short codes is its floor or its header's width, whichever
         // is more; the floor holds "ステータス" on its own (84: five 12-pixel glyphs
         // in semibold and the header's 20 pixels of padding), since the header is
         // not always counted once the grid scrolls sideways, and at 64 the
         // smallest window read "ステー…".
         string fitted = WithheldRowTests.MemberText(Source(), "private static DataGridTextColumn Fitted_(string header, string path, double minWidth)");
         Assert.True(fitted != null && fitted.Contains("Width = DataGridLength.Auto, MinWidth = minWidth", StringComparison.Ordinal), "A fitted column has no floor.");
      }

      [Fact]
      public void TheCountsColumns_KeepAWidth_AndTheAddressIsTrimmed()
      {
         string code = Source();
         string columns = WithheldRowTests.MemberText(code, "private void BuildColumns_()");
         Assert.Contains("Trimmed_(L(\"Address\"), nameof(CountView.Address), new DataGridLength(1, DataGridLengthUnitType.Star), 180)", columns, StringComparison.Ordinal);
         Assert.Contains("Text_(L(\"Last bounce\"), nameof(CountView.LastBounce), new DataGridLength(150))", columns, StringComparison.Ordinal);
         Assert.Contains("Text_(L(\"Last delivered\"), nameof(CountView.LastDelivered), new DataGridLength(150))", columns, StringComparison.Ordinal);
         Assert.Contains("Text_(L(\"Listed\"), nameof(CountView.Listed), new DataGridLength(70))", columns, StringComparison.Ordinal);

         // The six counts: 64 pixels, or the header's width where that is more, so
         // that "Mailbox full" is not cut to "Mailb…".
         string number = WithheldRowTests.MemberText(code, "private static DataGridTextColumn Number_(string header, string path)");
         Assert.True(number != null && number.Contains("Fitted_(header, path, 64)", StringComparison.Ordinal), "A count column has no floor of 64 pixels.");

         // A fixed column's width is its floor too, so a narrow window scrolls
         // the grid sideways rather than squeezing the dates.
         string text = WithheldRowTests.MemberText(code, "private static DataGridTextColumn Text_(string header, string path, DataGridLength width)");
         Assert.True(text != null && text.Contains("MinWidth = width.IsAbsolute ? width.Value : 0", StringComparison.Ordinal), "A fixed column can be squeezed below its width.");
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
