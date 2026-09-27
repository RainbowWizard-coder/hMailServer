// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The setup checklist's host name item: a name the Internet can look up is
   /// done, and everything a receiver cannot compare with DNS - nothing set (the
   /// Windows computer name goes out), a bare label, an address, a private suffix -
   /// needs action.
   /// </summary>
   public class HostNameVerdictTests
   {
      [Theory]
      [InlineData(null)]
      [InlineData("")]
      [InlineData("   ")]
      public void NothingSet_IsEmpty(string name)
         => Assert.Equal(HostNameVerdict.Kind.Empty, HostNameVerdict.Judge(name));

      [Theory]
      [InlineData("WORK-01")]
      [InlineData("mailserver")]
      [InlineData("192.0.2.25")]
      [InlineData("[192.0.2.25]")]
      [InlineData("2001:db8::25")]
      [InlineData("mail example.com")]
      [InlineData("mail..example.com")]
      [InlineData("-mail.example.com")]
      [InlineData("mail_1.example.com")]
      [InlineData("posta.exämple.com")]
      public void NotAFullDnsName(string name)
         => Assert.Equal(HostNameVerdict.Kind.NotADnsName, HostNameVerdict.Judge(name));

      [Theory]
      [InlineData("mail.contoso.local")]
      [InlineData("exchange.corp")]
      [InlineData("mx.example")]
      [InlineData("mail.example.test")]
      [InlineData("server.home.arpa")]
      [InlineData("mail.LAN")]
      public void APrivateSuffix_IsNotAPublicName(string name)
         => Assert.Equal(HostNameVerdict.Kind.PrivateName, HostNameVerdict.Judge(name));

      [Theory]
      [InlineData("mail.example.com")]
      [InlineData("mail.example.com.")]
      [InlineData(" MX1.Example.Co.UK ")]
      [InlineData("examplify.com")]
      [InlineData("xn--mail-9na.example.org")]
      public void AFullName_IsPublic(string name)
         => Assert.Equal(HostNameVerdict.Kind.PublicName, HostNameVerdict.Judge(name));
   }
}
