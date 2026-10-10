// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F231's last part: the Auto-ban page kept its two never-ban lists
   /// in two shapes - the list by address one comma-separated box saved with Save, the
   /// list by reverse name a table whose buttons write at once - and an entry of the
   /// address list the server could not read was said only in its error log. The list
   /// by address is a table too, each entry with what it spares, read as the server
   /// reads it (AutoBanFirewall::IsNeverBanned_, IPAddress::Parse).
   /// </summary>
   public class NeverBanAddressListTests
   {
      [Fact]
      public void TheEntries_AreApartAtACommaASemicolonOrWhiteSpace_AsTheServerSplitsThem()
      {
         Assert.Equal(new[] { "192.0.2.10", "203.0.113.0/24", "2001:db8::/32", "198.51.100.7" },
            NeverBanAddressList.Split(" 192.0.2.10, 203.0.113.0/24;2001:db8::/32\r\n198.51.100.7 ,").ToArray());
         Assert.Empty(NeverBanAddressList.Split(""));
         Assert.Empty(NeverBanAddressList.Split(null));
         Assert.Equal("192.0.2.10, 203.0.113.0/24", NeverBanAddressList.Join(new[] { "192.0.2.10", " ", "203.0.113.0/24 " }));
      }

      [Theory]
      [InlineData("192.0.2.10", "192.0.2.10", "192.0.2.10", true)]
      [InlineData("192.0.2.10/32", "192.0.2.10", "192.0.2.10", true)]
      // A block covers the addresses that share its first bits, whatever the rest are.
      [InlineData("203.0.113.5/24", "203.0.113.0", "203.0.113.255", false)]
      [InlineData("10.0.0.0/8", "10.0.0.0", "10.255.255.255", false)]
      [InlineData("0.0.0.0/0", "0.0.0.0", "255.255.255.255", false)]
      [InlineData("2001:db8::/32", "2001:db8::", "2001:db8:ffff:ffff:ffff:ffff:ffff:ffff", false)]
      [InlineData("2001:DB8::7", "2001:db8::7", "2001:db8::7", true)]
      [InlineData("fe80::1%3", "fe80::1", "fe80::1", true)]
      public void AnEntry_IsReadAsTheServerReadsIt(string entry, string first, string last, bool single)
      {
         Assert.True(NeverBanAddressList.TryRead(entry, out IPAddress low, out IPAddress high, out bool one), entry + " was refused.");
         Assert.Equal(IPAddress.Parse(first), low);
         Assert.Equal(IPAddress.Parse(last), high);
         Assert.Equal(single, one);
      }

      [Theory]
      // The forms IPAddress::Parse refuses on every platform, where .NET's own parser
      // would read "192.168.1" as 192.168.0.1 and "010.0.0.1" as 10.0.0.1.
      [InlineData("192.168.1")]
      [InlineData("192.168.1/24")]
      [InlineData("010.0.0.1")]
      [InlineData("1.2.3.04")]
      [InlineData("3232235777")]
      [InlineData("0x7f.0.0.1")]
      [InlineData("256.0.0.1")]
      [InlineData("192.0.2.7:25")]
      [InlineData("[::1]")]
      [InlineData("10.0.0.0/33")]
      [InlineData("2001:db8::/129")]
      [InlineData("10.0.0.0/")]
      [InlineData("10.0.0.0/x")]
      [InlineData("mail.example.net")]
      [InlineData("")]
      public void AnEntryTheServerIgnores_IsRefused(string entry)
         => Assert.False(NeverBanAddressList.TryRead(entry, out _, out _, out _), entry + " was read.");

      [Fact]
      public void EachRow_SaysWhatItSpares()
      {
         List<NeverBanAddressRow> rows = NeverBanAddressList.Rows("192.0.2.10, 203.0.113.0/24, office-pc");
         Assert.Equal(new[] { "192.0.2.10", "203.0.113.0/24", "office-pc" }, rows.Select(row => row.Entry).ToArray());
         Assert.Equal("One address", rows[0].Covers);
         Assert.Equal("203.0.113.0 - 203.0.113.255", rows[1].Covers);
         Assert.Equal("Ignored: not an address or a block", rows[2].Covers);
         Assert.False(rows[2].Readable);
         Assert.True(rows[0].Readable && rows[1].Readable);
      }

      [Fact]
      public void ATypedEntry_IsRefusedBeforeItIsWritten_WhenTheServerWouldNotReadIt_OrItIsListed()
      {
         var listed = new List<string> { "192.0.2.10", "2001:DB8::/32" };
         Assert.Equal("An address or a block is required.", NeverBanAddressList.Refusal("  ", listed));
         Assert.Equal("192.168.1 is not an address or a CIDR block.", NeverBanAddressList.Refusal("192.168.1", listed));
         // One entry a press: two are not one address.
         Assert.Equal("192.0.2.11, 192.0.2.12 is not an address or a CIDR block.", NeverBanAddressList.Refusal("192.0.2.11, 192.0.2.12", listed));
         Assert.Equal("2001:db8::/32 is on the list already.", NeverBanAddressList.Refusal("2001:db8::/32", listed));
         Assert.Null(NeverBanAddressList.Refusal(" 203.0.113.0/24 ", listed));
         Assert.Equal(new[] { "192.0.2.10" }, NeverBanAddressList.Without(listed, "2001:db8::/32").ToArray());
      }

      /// <summary>The page's row is the table, written by its own buttons at once, and the box is gone.</summary>
      [Fact]
      public void TheAutoBanPage_EditsTheListAsATable()
      {
         string page = Read("ServerSettingsView.xaml.cs");
         string rows = Read("ServerSettingsView.NeverBanAddresses.cs");

         Match body = Regex.Match(page, @"private void BuildAutoBan\(\)\s*\{(.*?)\n      \}", RegexOptions.Singleline);
         Assert.True(body.Success, "BuildAutoBan is gone.");
         Assert.Matches(new Regex(@"new NeverBanAddressRows\s*\{\s*Path = ""AutoBanNeverBan"","), body.Groups[1].Value);
         Assert.DoesNotMatch(new Regex(@"new IniText\s*\{\s*Path = ""AutoBanNeverBan"""), body.Groups[1].Value);
         // The neighbours' texts name the row by its caption.
         Assert.Contains("Label = L(\"Never auto-ban these addresses\")", body.Groups[1].Value, StringComparison.Ordinal);

         Assert.Contains("private sealed class NeverBanAddressRows : ComSetting, INotPersisted", rows, StringComparison.Ordinal);
         Assert.Contains("IniStore.Write(Path, NeverBanAddressList.Join(entries));", rows, StringComparison.Ordinal);
         Assert.Contains("SelectionGate.Bind(grid_, remove_);", rows, StringComparison.Ordinal);
         Assert.Contains("GridKeys.Wire(grid_, null, GridKeys.Pressing(remove_), GridKeys.Pressing(add_));", rows, StringComparison.Ordinal);
         // Asked first, with the destructive button: an address it spared can be banned again.
         Assert.Matches(new Regex(@"MessageBox\.Confirm\([\s\S]{0,300}?L\(""Re_move""\), destructive: true\)"), rows);
         // Each press reads the list again before it writes.
         Assert.True(Regex.Matches(rows, @"Read_\(out string failure\)").Count >= 3, "A press writes without reading the list again.");
      }

      private static string Read(string file) => FormLineAssert.ReadControlPanelFile(Path.Join("Views", file));
   }
}
