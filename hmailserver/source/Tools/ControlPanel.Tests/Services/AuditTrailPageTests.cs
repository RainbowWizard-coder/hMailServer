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
   /// Review finding F254: the audit trail offered Put back, its Primary, on every
   /// row, where on a domain's or an account's it answered with a notice; Newer stayed
   /// pressable on the first page and Older on the last, and they wrapped onto
   /// different rows at 760; the Interface, Object and Action columns showed the
   /// server's tokens - control_panel, role_assignment, password_reset - in every
   /// language; the Detail column got some 60 pixels; and the search matched the
   /// administrator alone. The words and the matching are AuditTrailReport's and are
   /// held here; the page's wiring is read from its source.
   /// </summary>
   public class AuditTrailPageTests
   {
      [Theory]
      [InlineData("COM", "Control Panel or script")]
      [InlineData("com", "Control Panel or script")]
      [InlineData("control_panel", "Control Panel")]
      [InlineData("REST", "REST API")]
      [InlineData("rest", "REST API")]
      [InlineData("Deck", "Control Deck")]
      [InlineData("Portal", "Self-service portal")]
      [InlineData("server", "Server")]
      [InlineData("Scheduler", "Scheduled task")]
      [InlineData("command line", "Command line")]
      [InlineData("service start", "Service start")]
      [InlineData("hmctl_batch", "Hmctl batch")]
      public void TheInterfaces_AreTheReadersWords(string token, string expected)
         => Assert.Equal(expected, AuditTrailReport.InterfaceWord(token));

      [Theory]
      [InlineData("setting", "Setting")]
      [InlineData("role_assignment", "Role assignment")]
      [InlineData("role assignment", "Role assignment")]
      [InlineData("ip range", "IP range")]
      [InlineData("tcp/ip port", "TCP/IP port")]
      [InlineData("alias", "Alias address")]
      [InlineData("rule", "Mail rule")]
      [InlineData("update", "Software update")]
      [InlineData("webhook subscription", "Webhook subscription")]
      public void TheObjects_AreTheReadersWords(string token, string expected)
         => Assert.Equal(expected, AuditTrailReport.ObjectWord(token));

      [Theory]
      [InlineData("created", "Created")]
      [InlineData("password_reset", "Password reset")]
      [InlineData("mailbox-key-forfeited", "Mailbox key forfeited")]
      [InlineData("restore started", "Restore started")]
      [InlineData("", "")]
      public void TheActions_AreTheReadersWords_AndAnUnknownOneIsHumanised(string token, string expected)
         => Assert.Equal(expected, AuditTrailReport.ActionWord(token));

      [Fact]
      public void ASettingsChange_IsSplitIntoItsTwoValues_WhereItSaysWhereTheOldOneEnds()
      {
         var row = new AuditRow { ObjectType = "setting", ObjectName = "AutoBanMinutes", Action = "updated", Detail = "AutoBanMinutes: 60 -> 120" };
         Assert.True(AuditTrailReport.TrySplitChange(row, out string from, out string to));
         Assert.Equal("60", from);
         Assert.Equal("120", to);

         // An empty side is a value as well.
         var emptied = new AuditRow { ObjectType = "setting", ObjectName = "SmtpRelayer", Action = "updated", Detail = "SmtpRelayer: relay.example.com -> " };
         Assert.True(AuditTrailReport.TrySplitChange(emptied, out from, out to));
         Assert.Equal("relay.example.com", from);
         Assert.Equal("", to);

         // A value holding the arrow does not say where the old value ends.
         var twice = new AuditRow { ObjectType = "setting", ObjectName = "Banner", Action = "updated", Detail = "Banner: a -> b -> c" };
         Assert.False(AuditTrailReport.TrySplitChange(twice, out _, out _));

         // Not a setting's change.
         var domain = new AuditRow { ObjectType = "domain", ObjectName = "example.net", Action = "created", Detail = "" };
         Assert.False(AuditTrailReport.TrySplitChange(domain, out _, out _));
      }

      [Fact]
      public void PutBack_IsOfferedOnASettingsChangeThatCarriesItsValues_AndOnNoOtherRow()
      {
         Assert.True(new AuditRow { ObjectType = "setting", ObjectName = "AutoBanMinutes", Action = "updated", Detail = "AutoBanMinutes: 60 -> 120" }.CanPutBack);
         Assert.True(new AuditRow { ObjectType = "setting", ObjectName = "AutoBanMinutes", Action = "deleted", Detail = "AutoBanMinutes: 60 -> (default)" }.CanPutBack);
         Assert.False(new AuditRow { ObjectType = "setting", ObjectName = "SmtpRelayerPassword", Action = "updated", Detail = "SmtpRelayerPassword: (changed)" }.CanPutBack);
         Assert.False(new AuditRow { ObjectType = "domain", ObjectName = "example.net", Action = "created" }.CanPutBack);
         Assert.False(new AuditRow { ObjectType = "account", ObjectName = "user01@example.com", Action = "password_reset" }.CanPutBack);
      }

      [Fact]
      public void TheDetail_DrawsTheServersArrowAsOne()
         => Assert.Equal("AutoBanMinutes: 60 → 120", AuditTrailReport.ShownDetail("AutoBanMinutes: 60 -> 120"));

      [Fact]
      public void TheSearch_LooksInEveryColumn_AsTheServerWroteItAndAsThePageShowsIt()
      {
         var row = new AuditRow
         {
            Actor = "helpdesk@example.com", Interface = "REST", Address = "10.1.0.42", ObjectType = "role_assignment",
            ObjectName = "audit@example.com", Action = "created", Detail = "auditor, every domain"
         };

         Assert.True(AuditTrailReport.Matches(row, ""));
         Assert.True(AuditTrailReport.Matches(row, "helpdesk"));
         Assert.True(AuditTrailReport.Matches(row, "audit@example.com"));
         Assert.True(AuditTrailReport.Matches(row, "Role assignment"));
         Assert.True(AuditTrailReport.Matches(row, "role_assignment"));
         Assert.True(AuditTrailReport.Matches(row, "REST API"));
         Assert.True(AuditTrailReport.Matches(row, "every domain"));
         Assert.True(AuditTrailReport.Matches(row, "10.1.0"));
         Assert.True(AuditTrailReport.Matches(row, "CREATED auditor"));
         Assert.False(AuditTrailReport.Matches(row, "auditor deleted"));
         Assert.False(AuditTrailReport.Matches(null, "x"));
      }

      [Fact]
      public void ThePage_OffersPutBackWhereItWorks_PagesUnderTheList_AndSearchesTheWholeTrail()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "AuditTrailView.cs"));

         // Put back: a secondary button, pressable on a row it can put back, saying why not on another.
         Assert.Contains("putBack_ = MakeButton(L(\"_Put back\"), Wpf.Ui.Controls.ControlAppearance.Secondary, (_, _) => PutBackSelected());", code, StringComparison.Ordinal);
         Assert.DoesNotContain("ControlAppearance.Primary", code, StringComparison.Ordinal);
         string gate = WithheldRowTests.MemberText(code, "private void GatePutBack_()");
         Assert.True(gate != null, "AuditTrailView.GatePutBack_ is gone.");
         Assert.Contains("rows_.SelectedItem is RowView { CanPutBack: true }", gate, StringComparison.Ordinal);
         Assert.Contains("AuditTrailReport.TrySplitChange(row.Row, out string from, out string to)", code, StringComparison.Ordinal);

         // The pages under the list, each step pressable only where there is a page that way.
         string pager = WithheldRowTests.MemberText(code, "private void ShowPager_()");
         Assert.True(pager != null, "AuditTrailView.ShowPager_ is gone.");
         Assert.Contains("newer_.IsEnabled = offset_ > 0;", pager, StringComparison.Ordinal);
         Assert.Contains("older_.IsEnabled = offset_ + PageSize < total_;", pager, StringComparison.Ordinal);
         Assert.Contains("parts.Children.Add(pager_);", code, StringComparison.Ordinal);
         Assert.True(code.IndexOf("parts.Children.Add(pager_);", StringComparison.Ordinal) > code.IndexOf("parts.Children.Add(FillPanel.Filling(card, FillPanel.ListCardFloor));", StringComparison.Ordinal),
            "The pager is not under the list.");

         // The server's tokens in the reader's words, and the detail its share of the width.
         Assert.Contains("Interface = AuditTrailReport.InterfaceWord(row.Interface),", code, StringComparison.Ordinal);
         Assert.Contains("Object = AuditTrailReport.ObjectWord(row.ObjectType),", code, StringComparison.Ordinal);
         Assert.Contains("Action = AuditTrailReport.ActionWord(row.Action),", code, StringComparison.Ordinal);
         Assert.Contains("new DataGridLength(2, DataGridLengthUnitType.Star)", code, StringComparison.Ordinal);
         Assert.Contains("detail.MinWidth = 200;", code, StringComparison.Ordinal);

         // The search reads the whole trail off the window's thread and looks in every column.
         Assert.Contains("read = await Task.Run(Walk_);", code, StringComparison.Ordinal);
         Assert.Contains("AuditTrailReport.Matches(row, query)", code, StringComparison.Ordinal);
         Assert.DoesNotContain("GetAuditTrail(actor", code, StringComparison.Ordinal);
      }

      [Fact]
      public void TheTimesSample_IsItsFormatInTheWidestDigit()
      {
         // A font whose 3 is widest and whose 1 is narrowest, as the window's are not one width.
         static double Measure(string text) => text.Length * (text[0] == '3' ? 9.0 : text[0] == '1' ? 5.0 : 7.0);

         Assert.Equal("3333-33-33 33:33:33", AuditTrailReport.WidestDigits("2026-10-10 00:00:00", Measure));
         // Whatever the culture puts between them is kept.
         Assert.Equal("3333-33-33 33.33.33", AuditTrailReport.WidestDigits("2026-10-10 10.10.10", Measure));
         Assert.Equal("", AuditTrailReport.WidestDigits(null, Measure));
      }

      [Fact]
      public void AColumnOfWords_IsFlooredAtItsWidestWord_NoNarrowerThanItsHeader_NoWiderThanTheCap()
      {
         Assert.Equal(132, AuditTrailReport.WordsFloor(60, new[] { 95.0, 131.2, 70.0 }, 180));
         Assert.Equal(180, AuditTrailReport.WordsFloor(60, new[] { 95.0, 260.0 }, 180));
         Assert.Equal(60, AuditTrailReport.WordsFloor(60, new[] { 40.0 }, 180));
         // No rows: the header's.
         Assert.Equal(60, AuditTrailReport.WordsFloor(60, Array.Empty<double>(), 180));
      }

      [Fact]
      public void TheColumns_AreFlooredAtTheirWordsOnThePage_AndTheTimeAtItsWidest()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "AuditTrailView.cs"));

         // The time measured in its widest digit, where "2026-10-10 00:00:00" - ones and
         // noughts - measured narrower than 23:56:48 and cut it.
         Assert.Contains("GridStyles.CellWidth(WidestTime_())", code, StringComparison.Ordinal);
         Assert.Contains("AuditTrailReport.WidestDigits(new DateTime(2026, 10, 10, 10, 10, 10).ToString(WhenFormat), text => GridStyles.CellWidth(text))", code, StringComparison.Ordinal);
         Assert.Contains(".ToLocalTime().ToString(WhenFormat)", code, StringComparison.Ordinal);
         Assert.DoesNotContain("\"2026-10-10 00:00:00\"", code, StringComparison.Ordinal);

         // Every column of words floored at its words on the page shown, where they
         // were floored at 90 and cut at 1280 in every language; Detail keeps its floor.
         foreach (string column in new[] { "Who", "Interface", "From", "Object", "Name", "Action" })
            Assert.Contains("rows_.Columns.Add(Shared_(L(\"" + column + "\"), nameof(RowView." + column + "), row => row." + column, code, StringComparison.Ordinal);
         Assert.DoesNotContain("Math.Max(90,", code, StringComparison.Ordinal);
         Assert.Contains("column.MinWidth = AuditTrailReport.WordsFloor(header, rows.Select(row => GridStyles.CellWidth(value(row))), SharedFloorCap);", code, StringComparison.Ordinal);
         Assert.DoesNotContain("rows_.ItemsSource = Present_(", code, StringComparison.Ordinal);
         string show = WithheldRowTests.MemberText(code, "private void Show_(IReadOnlyList<AuditRow> rows)");
         Assert.True(show != null, "AuditTrailView.Show_ is gone.");
         Assert.True(show.IndexOf("FloorShared_(shown);", StringComparison.Ordinal) < show.IndexOf("rows_.ItemsSource = shown;", StringComparison.Ordinal),
            "The columns are floored after the rows are in the grid.");
         Assert.Contains("detail.MinWidth = 200;", code, StringComparison.Ordinal);
      }

      [Fact]
      public void ThePagersSteps_HandTheKeyboardOn_BeforeOneIsDisabled()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "AuditTrailView.cs")).Replace("\r\n", "\n");

         // Older pressed from the keyboard onto the last page disabled the button that
         // held the keyboard, which dropped to the window.
         string pager = WithheldRowTests.MemberText(code, "private void ShowPager_()");
         Assert.True(pager != null, "AuditTrailView.ShowPager_ is gone.");
         int keep = pager.IndexOf("KeepPagerKeyboard_(pages && offset_ > 0, pages && offset_ + PageSize < total_);", StringComparison.Ordinal);
         Assert.True(keep >= 0 && keep < pager.IndexOf("pager_.Visibility =", StringComparison.Ordinal) && keep < pager.IndexOf("newer_.IsEnabled =", StringComparison.Ordinal),
            "The keyboard is not handed on before the pager is hidden or a step disabled.");

         // To the other step where it stays pressable, else to the list, else the search.
         string hand = WithheldRowTests.MemberText(code, "private void KeepPagerKeyboard_(bool newerStays, bool olderStays)");
         Assert.True(hand != null, "AuditTrailView.KeepPagerKeyboard_ is gone.");
         Assert.Contains("other.IsEnabled = true;", hand, StringComparison.Ordinal);
         Assert.Contains("ReferenceEquals(System.Windows.Input.Keyboard.Focus(other), other)", hand, StringComparison.Ordinal);
         Assert.Contains("GridKeys.FocusSelectedRow(rows_);", hand, StringComparison.Ordinal);
         Assert.Contains("toolbar_.FocusSearch();", hand, StringComparison.Ordinal);
      }

      [Fact]
      public void ARead_AskedForAgainWhileTheTrailIsRead_StartsWhenThatReadEnds()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "AuditTrailView.cs")).Replace("\r\n", "\n");
         string search = WithheldRowTests.MemberText(code, "private async void Search_()");
         Assert.True(search != null, "AuditTrailView.Search_ is gone.");

         // Refresh, and the page opened again, start a new generation and read again;
         // with a read running, Search_ leaves the trail to it.
         Assert.Contains("(_, _) => { generation_++; trail_ = null; Reload(); }", code, StringComparison.Ordinal);
         Assert.Contains("if (readingTrail_)\n               return;", search, StringComparison.Ordinal);

         // That read, ending in an older generation, reads again rather than return
         // and leave the list empty, with no rows, state or pager.
         Assert.Contains("if (generation != generation_)\n            {\n               Reload();\n               return;\n            }", search, StringComparison.Ordinal);
         Assert.True(search.IndexOf("readingTrail_ = false;", StringComparison.Ordinal) < search.IndexOf("if (generation != generation_)", StringComparison.Ordinal),
            "The read is not over when Search_ reads again, so the new read would find it running.");
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
