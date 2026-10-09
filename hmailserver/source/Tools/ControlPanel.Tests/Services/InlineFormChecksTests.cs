// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// What the forms check of a value before they send it (review findings F129
   /// and F212): the IP ranges page made a priority that was no number 15, the
   /// routes page made a port of "25a" 25, both without a word, and the incoming
   /// relays page sent "abc" to "xyz" as a range for the server to refuse. A
   /// number is a number or it is refused, an address is what the server reads as
   /// one, and a range's ends are of one family with the first not above the last.
   /// </summary>
   public class InlineFormChecksTests
   {
      [Theory]
      [InlineData("15", 15)]
      [InlineData(" 30 ", 30)]
      [InlineData("-5", -5)]
      [InlineData("0", 0)]
      public void AWholeNumber_IsTaken(string text, int expected)
      {
         Assert.True(InlineFormChecks.TryWholeNumber(text, int.MinValue, int.MaxValue, out int value));
         Assert.Equal(expected, value);
      }

      [Theory]
      [InlineData("")]
      [InlineData("   ")]
      [InlineData(null)]
      [InlineData("high")]
      [InlineData("25a")]
      [InlineData("1e3")]
      [InlineData("1,000")]
      [InlineData("1.5")]
      [InlineData("99999999999")]
      public void NothingElse_IsAWholeNumber(string text)
      {
         Assert.False(InlineFormChecks.TryWholeNumber(text, int.MinValue, int.MaxValue, out _));
      }

      [Theory]
      [InlineData("1", true)]
      [InlineData("25", true)]
      [InlineData("65535", true)]
      [InlineData("0", false)]
      [InlineData("65536", false)]
      [InlineData("-25", false)]
      [InlineData("25a", false)]
      [InlineData("", false)]
      public void APort_IsAWholeNumberFrom1To65535(string text, bool taken)
      {
         Assert.Equal(taken, InlineFormChecks.TryPort(text, out _));
      }

      [Theory]
      [InlineData("10.0.0.1", true)]
      [InlineData(" 192.168.1.255 ", true)]
      [InlineData("0.0.0.0", true)]
      [InlineData("2001:db8::1", true)]
      [InlineData("::", true)]
      [InlineData("abc", false)]
      [InlineData("10.1", false)]
      [InlineData("010.0.0.1", false)]
      [InlineData("10.0.0.256", false)]
      [InlineData("0.0.0", false)]
      [InlineData("[::1]", false)]
      [InlineData("", false)]
      public void AnAddress_IsWhatTheServerReadsAsOne(string text, bool address)
      {
         Assert.Equal(address, InlineFormChecks.IsAddress(text));
      }

      [Theory]
      [InlineData("10.0.0.0", "10.0.0.255", InlineFormChecks.RangeOrder.InOrder)]
      [InlineData("10.0.0.5", "10.0.0.5", InlineFormChecks.RangeOrder.InOrder)]
      [InlineData("9.255.255.255", "10.0.0.0", InlineFormChecks.RangeOrder.InOrder)]
      [InlineData("10.0.0.9", "10.0.0.1", InlineFormChecks.RangeOrder.Reversed)]
      [InlineData("10.0.1.0", "10.0.0.255", InlineFormChecks.RangeOrder.Reversed)]
      [InlineData("2001:db8::1", "2001:db8::ffff", InlineFormChecks.RangeOrder.InOrder)]
      [InlineData("2001:db8::2", "2001:db8::1", InlineFormChecks.RangeOrder.Reversed)]
      [InlineData("10.0.0.1", "2001:db8::1", InlineFormChecks.RangeOrder.MixedFamilies)]
      public void ARangesEnds_AreOfOneFamily_TheFirstNotAboveTheLast(string lower, string upper, InlineFormChecks.RangeOrder expected)
      {
         Assert.Equal(expected, InlineFormChecks.OrderOf(lower, upper));
      }

      [Fact]
      public void ARangeWithAnEndThatIsNoAddress_HasNoOrder()
      {
         Assert.Null(InlineFormChecks.OrderOf("abc", "10.0.0.1"));
         Assert.Null(InlineFormChecks.OrderOf("10.0.0.1", "xyz"));
      }

      [Theory]
      [InlineData("smtp.partner.example.net", true)]
      [InlineData("mailhub", true)]
      [InlineData("192.0.2.25", true)]
      [InlineData("2001:db8::25", true)]
      [InlineData("", false)]
      [InlineData("smtp partner", false)]
      [InlineData("smtp_relay.example.com", false)]
      [InlineData("-bad.example.com", false)]
      [InlineData("smtp..example.com", false)]
      public void AHost_IsAnAddressOrAHostName(string text, bool host)
      {
         Assert.Equal(host, InlineFormChecks.IsHost(text));
      }
   }
}
