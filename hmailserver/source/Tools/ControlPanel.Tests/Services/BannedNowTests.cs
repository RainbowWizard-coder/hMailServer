// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;
using System.IO;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F231: the Auto-ban page listed no ban, "Clear logon-failure
   /// list" said outright that it lifted none, the IP ranges page drew the bans as
   /// permanent ranges with no Expires column or filter, and lifting one took four
   /// steps across the two pages. The Auto-ban page opens on Banned now - each ban's
   /// address, why and when it ends, with Lift ban - and the IP ranges page has an
   /// Expires column, a Relay column with the open relay marked, and Show auto-bans
   /// only. The words are AutoBanList's, TimeUntilText's and IpRangeCells'; the
   /// pages' wiring is read from their source.
   /// </summary>
   public class BannedNowTests
   {
      private static readonly DateTime Now = new(2026, 10, 10, 14, 0, 0);

      [Theory]
      [InlineData("Auto-ban: jane@example.com", true, true)]
      [InlineData("Auto-ban: spam trap nobody@example.com", true, true)]
      // An administrator's own expiring range is not one of the server's bans, and a
      // range named like a ban that does not expire is not one the server lifts.
      [InlineData("Branch office until Friday", true, false)]
      [InlineData("Auto-ban: jane@example.com", false, false)]
      [InlineData("auto-ban: jane@example.com", true, false)]
      [InlineData(null, true, false)]
      public void ABan_IsAnExpiringRangeNamedWithTheServersPrefix(string name, bool expires, bool ban)
         => Assert.Equal(ban, AutoBanList.IsBan(name, expires));

      [Theory]
      [InlineData("Auto-ban: jane@example.com", "Failed sign-ins as jane@example.com")]
      // The server's suffixes that make a second ban's name unique are a counter, not a cause.
      [InlineData("Auto-ban: jane@example.com (2)", "Failed sign-ins as jane@example.com")]
      [InlineData("Auto-ban: jane@example.com (10)", "Failed sign-ins as jane@example.com")]
      [InlineData("Auto-ban: jane@example.com (Zab12CD34e)", "Failed sign-ins as jane@example.com")]
      [InlineData("Auto-ban: jane@example.com {0F8FAD5B-D9CB-469F-A165-70867728950E}", "Failed sign-ins as jane@example.com")]
      [InlineData("Auto-ban: spam trap nobody@example.com", "Mail to the spam trap nobody@example.com")]
      // A second address that mails the trap while the first is banned: the server's counter is not the trap's.
      [InlineData("Auto-ban: spam trap nobody@example.com (1)", "Mail to the spam trap nobody@example.com")]
      [InlineData("Auto-ban: spam trap nobody@example.com (Zab12CD34e)", "Mail to the spam trap nobody@example.com")]
      [InlineData("Auto-ban: spam trap nobody@example.com {0F8FAD5B-D9CB-469F-A165-70867728950E}", "Mail to the spam trap nobody@example.com")]
      [InlineData("Auto-ban: multi-recipient null sender 198.51.100.7", "Mail from an empty sender to several recipients")]
      [InlineData("Auto-ban: AUTH on a delivery port 198.51.100.7", "A sign-in attempt on a port that takes no password")]
      public void TheCause_IsSaidForEachOfTheFourWriters(string name, string cause)
         => Assert.Equal(cause, AutoBanList.Cause(name));

      [Fact]
      public void TheAddress_IsOneAddress_OrTheRunAWidenedBanCovers()
      {
         Assert.Equal("198.51.100.77", AutoBanList.Address("198.51.100.77", "198.51.100.77"));
         Assert.Equal("203.0.113.10 - 203.0.113.14", AutoBanList.Address("203.0.113.10", "203.0.113.14"));
      }

      [Fact]
      public void TheEnd_IsAWait_WhereTheClocksAreOne()
      {
         Assert.Equal("23 min from now", AutoBanList.Ends(Now.AddMinutes(23).AddSeconds(30), Now, sameClock: true));
         Assert.Equal("1 h 5 min from now", AutoBanList.Ends(Now.AddMinutes(65), Now, sameClock: true));
         Assert.Equal("2 d 3 h from now", AutoBanList.Ends(Now.AddHours(51), Now, sameClock: true));

         // The expiry sweep runs once a minute: a ban whose time has passed is
         // lifted at the next one, not "-3 min from now".
         Assert.Equal("within a minute", AutoBanList.Ends(Now.AddMinutes(-3), Now, sameClock: true));
         Assert.Equal("within a minute", AutoBanList.Ends(Now.AddSeconds(20), Now, sameClock: true));
      }

      [Fact]
      public void TheEnd_IsTheServersTime_AcrossTwoComputersClocks()
      {
         CultureInfo before = CultureInfo.CurrentCulture;
         try
         {
            CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("en-GB");
            Assert.Equal("10/10/2026 14:23 (server time)", AutoBanList.Ends(Now.AddMinutes(23), Now, sameClock: false));
         }
         finally
         {
            CultureInfo.CurrentCulture = before;
         }
      }

      [Fact]
      public void TheServersTextTimes_AreRead_AndItsNeverIsNone()
      {
         Assert.Equal(new DateTime(2026, 10, 10, 14, 23, 5), TimeUntilText.Parse("2026-10-10 14:23:05"));
         Assert.Null(TimeUntilText.Parse("1901-01-01 00:00:00"));
         Assert.Null(TimeUntilText.Parse(""));
         Assert.Null(TimeUntilText.Parse("soon"));
         Assert.Equal("less than a minute", TimeUntilText.Span(TimeSpan.FromSeconds(-5)));
         Assert.Equal("12 min", TimeUntilText.Span(TimeSpan.FromMinutes(12.5)));
      }

      [Fact]
      public void TheExpiresCell_IsNever_ForAPermanentRange()
      {
         Assert.Equal("Never", IpRangeCells.Expires(false, DateTime.MinValue, Now, sameClock: true));
         Assert.Equal("23 min from now", IpRangeCells.Expires(true, Now.AddMinutes(23).AddSeconds(1), Now, sameClock: true));
      }

      /// <summary>
      /// The Relay cell counts a relay as the range's permissions card does
      /// (RelayExposure, review finding F229): either direction to an external
      /// address, an open relay Critical, a private network's a Warning, one
      /// address unmarked - so that the grid and the card say one thing of a range.
      /// </summary>
      [Theory]
      [InlineData("0.0.0.0", "255.255.255.255", false, false, false, false, "No", StatusLevel.Normal)]
      // The shipped Internet range: both directions, each after authentication.
      [InlineData("0.0.0.0", "255.255.255.255", true, true, true, true, "After sign-in", StatusLevel.Normal)]
      // The shipped "My computer" relays without a sign-in, and only that machine can.
      [InlineData("127.0.0.1", "127.0.0.1", true, false, false, false, "Without sign-in", StatusLevel.Normal)]
      [InlineData("127.0.0.1", "127.0.0.1", true, false, true, false, "Without sign-in", StatusLevel.Normal)]
      // A private network's range: every machine of it, a Warning, as on the card.
      [InlineData("10.1.0.0", "10.1.0.255", false, false, true, false, "Without sign-in", StatusLevel.Warning)]
      [InlineData("192.168.1.0", "192.168.1.255", true, false, false, false, "Without sign-in", StatusLevel.Warning)]
      // Anyone on the Internet: by any sender, or by one that gives a local domain.
      [InlineData("0.0.0.0", "255.255.255.255", true, true, true, false, "Open relay", StatusLevel.Critical)]
      [InlineData("0.0.0.0", "255.255.255.255", true, false, true, true, "Open relay", StatusLevel.Critical)]
      [InlineData("8.8.0.0", "8.8.255.255", false, false, true, false, "Open relay", StatusLevel.Critical)]
      public void TheRelayCell_MarksARelay_AsThePermissionsCardDoes(string lower, string upper, bool localToExternal, bool localNeedsAuth,
                                                                     bool externalToExternal, bool externalNeedsAuth, string word, StatusLevel level)
      {
         (string w, StatusLevel l) = IpRangeCells.Relay(lower, upper, localToExternal, localNeedsAuth, externalToExternal, externalNeedsAuth);
         Assert.Equal(word, w);
         Assert.Equal(level, l);
      }

      /// <summary>Banned now is the Auto-ban page's first card, its rows the bans, each with Lift ban.</summary>
      [Fact]
      public void TheAutoBanPage_OpensOnBannedNow()
      {
         string page = Read("ServerSettingsView.xaml.cs");
         string rows = Read("ServerSettingsView.BannedNow.cs");
         if (page == null || rows == null)
            return;

         Match body = Regex.Match(page, @"private void BuildAutoBan\(\)\s*\{(.*?)\n      \}", RegexOptions.Singleline);
         Assert.True(body.Success, "BuildAutoBan is gone.");
         string autoban = body.Groups[1].Value;
         int banned = autoban.IndexOf("new BannedNowRows()", StringComparison.Ordinal);
         int settings = autoban.IndexOf("Path = \"AutoBanOnLogonFailure\"", StringComparison.Ordinal);
         Assert.True(banned >= 0, "The Auto-ban page has no Banned now card.");
         Assert.True(banned < settings, "Banned now is not the Auto-ban page's first card.");
         Assert.DoesNotContain("delete it on the IP ranges page", autoban, StringComparison.Ordinal);

         Assert.Contains("AutoBanList.IsBan(name, (bool)range.Expires)", rows, StringComparison.Ordinal);
         Assert.Contains("range.Delete();", rows, StringComparison.Ordinal);
         // Asked first, Cancel taking Enter: the Control Panel cannot put a ban back.
         Assert.Matches(new Regex(@"MessageBox\.Confirm\([\s\S]{0,300}?L\(""_Lift ban""\), cancelByDefault: true\)"), rows);
         // The row's icon is named for its row, and the keyboard's way is the button under the list.
         Assert.Contains("F(\"Lift the ban on {0}\", Address)", rows, StringComparison.Ordinal);
         Assert.Contains("SelectionGate.Bind(grid_, lift_);", rows, StringComparison.Ordinal);
         Assert.Contains("GridKeys.Wire(grid_, null, GridKeys.Pressing(lift_), null);", rows, StringComparison.Ordinal);
      }

      [Fact]
      public void TheIpRangesPage_ShowsWhenARangeEnds_ItsRelay_AndTheBansAlone()
      {
         string xaml = Read("IPRangesView.xaml");
         string code = Read("IPRangesView.xaml.cs");
         if (xaml == null || code == null)
            return;

         Assert.Contains("Header=\"{loc:L 'Expires'}\" Binding=\"{Binding Expires}\"", xaml, StringComparison.Ordinal);
         Assert.Contains("Header=\"{loc:L 'Relay'}\"", xaml, StringComparison.Ordinal);
         // The Relay column is the grid's last: the columns before it keep no floor in the markup wider than
         // their words need - the name's 120 pixels, the permissions' 48 - so that at 1280 beside the sidebar
         // the grid fits and the Critical pill is whole, where it scrolled sideways and cut it to "Open rela".
         Assert.Contains("Header=\"{loc:L 'Name'}\" Binding=\"{Binding Name}\" Width=\"*\" MinWidth=\"120\"", xaml, StringComparison.Ordinal);
         foreach (string protocol in new[] { "Smtp", "Imap", "Pop3" })
            Assert.Matches(new Regex("x:Name=\"" + protocol + "Column\" Header=\"[A-Z0-9]+\" Binding=\"\\{Binding " + protocol + "\\}\" Width=\"Auto\" MinWidth=\"48\" />"), xaml);
         Assert.Matches(new Regex(@"StatusPill x:Name=""RelayPill"" Level=""\{Binding RelayLevel\}"""), xaml);
         Assert.Contains("<DataTrigger Binding=\"{Binding RelayMarked}\" Value=\"True\">", xaml, StringComparison.Ordinal);
         Assert.Contains("x:Name=\"BansOnlyBox\"", xaml, StringComparison.Ordinal);

         Assert.Contains("IsBan = AutoBanList.IsBan(name, expires)", code, StringComparison.Ordinal);
         Assert.Contains("ListSearchState.ShowNarrowed(RangeGrid, SearchBar.SearchText, CountPill,", code, StringComparison.Ordinal);
         Assert.Contains("item => item is RangeRow { IsBan: true }", code, StringComparison.Ordinal);
      }

      /// <summary>A filter of the page's own counts as a search does: what is shown of the whole list.</summary>
      [Fact]
      public void ANarrowedList_IsCounted_AsASearchIs()
      {
         Assert.Equal("Shown: 1 of 5", ListSearchCount.NarrowedPillText(1, 5, "5 IP ranges"));
         Assert.Equal("Shown: 0 of 5", ListSearchCount.NarrowedPillText(0, 5, "5 IP ranges"));
         Assert.Equal("0 IP ranges", ListSearchCount.NarrowedPillText(0, 0, "0 IP ranges"));
      }

      [Fact]
      public void ServerStatus_SendsTheBanCountToBannedNow()
      {
         string xaml = Read("StatusView.xaml");
         string code = Read("StatusView.xaml.cs");
         if (xaml == null || code == null)
            return;

         Assert.Contains("Click=\"OpenAutoBan_Click\"", xaml, StringComparison.Ordinal);
         Assert.Contains("NavigateTo(\"autoban\")", code, StringComparison.Ordinal);
      }

      private static string Read(string file)
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string path = Path.Join(directory.FullName, "ControlPanel", "Views", file);
            if (File.Exists(path))
               return File.ReadAllText(path);
         }

         return null;
      }
   }
}
