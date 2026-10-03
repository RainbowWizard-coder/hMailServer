// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Net;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// An address in text as the server reads one: the same table as the server's
   /// IPAddressTester (Common/TCPIP/IPAddress.cpp), so that what the Control Panel
   /// lets through is what the server takes, on Windows as on Linux.
   /// </summary>
   public class ServerAddressTextTests
   {
      [Theory]
      [InlineData("192.0.2.7", "192.0.2.7")]
      [InlineData("0.0.0.0", "0.0.0.0")]
      [InlineData("255.255.255.255", "255.255.255.255")]
      [InlineData("10.0.0.1", "10.0.0.1")]
      [InlineData("::", "::")]
      [InlineData("::1", "::1")]
      [InlineData("2001:db8::7", "2001:db8::7")]
      [InlineData("2001:DB8::7", "2001:db8::7")]
      [InlineData("1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8")]
      [InlineData("1:2:3:4:5:6:7::", "1:2:3:4:5:6:7:0")]
      [InlineData("::1:2:3:4:5:6:7", "0:1:2:3:4:5:6:7")]
      [InlineData("0001::", "1::")]
      [InlineData("::ffff:192.0.2.7", "::ffff:192.0.2.7")]
      [InlineData("1:2:3:4:5:6:1.2.3.4", "1:2:3:4:5:6:102:304")]
      [InlineData("1:2:3:4:5::1.2.3.4", "1:2:3:4:5:0:102:304")]
      [InlineData("fe80::1%3", "fe80::1%3")]
      [InlineData("fe80::1%4294967295", "fe80::1%4294967295")]
      [InlineData("fe80::1%no-such-if0", "fe80::1")]
      public void AnAddressTheServerTakes_IsTakenAsTheSameAddress(string text, string expected)
      {
         Assert.True(ServerAddressText.TryParse(text, out IPAddress address), text);
         Assert.Equal(IPAddress.Parse(expected), address);
      }

      [Theory]
      // The older IPv4 forms System.Net.IPAddress.TryParse and Windows' own parser
      // read and the server does not.
      [InlineData("192.0.2")]
      [InlineData("192.0")]
      [InlineData("192")]
      [InlineData("10.1")]
      [InlineData("3232235777")]
      [InlineData("4294967295")]
      [InlineData("0x7f.1")]
      [InlineData("0x7f.0.0.1")]
      [InlineData("0xffffffff")]
      [InlineData("0300.0250.0.1")]
      [InlineData("192.168.001.001")]
      [InlineData("010.0.0.1")]
      [InlineData("1.2.3.04")]
      [InlineData("00.0.0.0")]
      // An address with something around it.
      [InlineData("192.0.2.7:25")]
      [InlineData("[::1]")]
      [InlineData("[::1]:25")]
      [InlineData("[2001:db8::7]")]
      [InlineData("[192.0.2.7]")]
      [InlineData(" 192.0.2.7")]
      [InlineData("192.0.2.7 ")]
      [InlineData("192.0.2.7.")]
      [InlineData("IPv6:2001:db8::7")]
      // And no address anywhere.
      [InlineData(null)]
      [InlineData("")]
      [InlineData("256.0.0.1")]
      [InlineData("1..2.3")]
      [InlineData("1.2.3.4.5")]
      [InlineData("::ffff:1.2.3")]
      [InlineData("::ffff:01.2.3.4")]
      [InlineData("1:2:3:4:5:6:7:1.2.3.4")]
      [InlineData("1:2:3:4:5:6:7:8::")]
      [InlineData("::1:2:3:4:5:6:7:8")]
      [InlineData("1:2:3:4:5:6:7")]
      [InlineData(":1::")]
      [InlineData("1::2::3")]
      [InlineData(":::")]
      [InlineData("1::2:")]
      [InlineData("00001::")]
      [InlineData("fe80::1%")]
      [InlineData("fe80::1%4294967296")]
      [InlineData("fe80::1%eth 0")]
      [InlineData("fe80::1%3%4")]
      public void WhatTheServerRefuses_IsRefused(string text)
      {
         Assert.False(ServerAddressText.IsAddress(text), text ?? "(null)");
      }

      /// <summary>
      /// The negative control of the table above: System.Net reads these as
      /// addresses, which is why the Control Panel no longer asks it.
      /// </summary>
      [Theory]
      [InlineData("10.1")]
      [InlineData("010.0.0.1")]
      [InlineData("0x7f.1")]
      [InlineData("3232235777")]
      public void SystemNetTakesWhatTheServerRefuses(string text)
      {
         Assert.True(IPAddress.TryParse(text, out _), text);
         Assert.False(ServerAddressText.IsAddress(text), text);
      }
   }
}
