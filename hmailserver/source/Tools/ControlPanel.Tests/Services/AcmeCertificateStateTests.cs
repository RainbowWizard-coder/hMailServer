// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F237: the Certificates (ACME) page's one live fact - a certificate
   /// issued, valid until, expired, or none yet - was the card's last row, below seven
   /// fields and so below the fold at 1280 by 800; with Issue and renew automatically
   /// ticked and no host name, the page saved it and then blamed the network ("if it
   /// persists, the CA could not reach this server"); and the ACME directory URL had no
   /// placeholder. The state is the card's first row with the way to the SSL
   /// certificates page, a list with no host name is said at Critical and alone, and the
   /// directory shows the Let's Encrypt URL.
   /// </summary>
   public class AcmeCertificateStateTests
   {
      [Theory]
      [InlineData("mail.example.com", true)]
      [InlineData("mail.example.com, mta-sts.example.com", true)]
      [InlineData(" , mail.example.com", true)]
      [InlineData("", false)]
      [InlineData("   ", false)]
      [InlineData(" , ,", false)]
      [InlineData(null, false)]
      public void AHostName_IsReadAsTheServerReadsTheList(string domains, bool any)
         => Assert.Equal(any, AcmeHostNames.Any(domains));

      // AcmeClient::ParseDomainList and UnusableDomains, case for case: the hosts the
      // server orders for, and the names it leaves out as written.
      [Theory]
      [InlineData("mail.example.com", "mail.example.com", "")]
      [InlineData(" Mail.Example.COM. , mta-sts.example.com,mail.example.com", "mail.example.com|mta-sts.example.com", "")]
      [InlineData("*.example.com", "", "*.example.com")]
      [InlineData("192.0.2.7", "", "192.0.2.7")]
      [InlineData("[2001:db8::1]", "", "[2001:db8::1]")]
      [InlineData("bücher.example.com", "", "bücher.example.com")]
      [InlineData("xn--bcher-kva.example.com", "xn--bcher-kva.example.com", "")]
      [InlineData("mail example.com, a;b.example.com", "", "mail example.com|a;b.example.com")]
      [InlineData("-bad.example.com, bad-.example.com, a..example.com, example.123", "", "-bad.example.com|bad-.example.com|a..example.com|example.123")]
      [InlineData("4.example.com, *.example.com, mail.example.com", "4.example.com|mail.example.com", "*.example.com")]
      [InlineData("Kelvin.example.com", "", "Kelvin.example.com")]
      [InlineData("mail.example.com ", "", "mail.example.com ")]
      [InlineData(" , .", "", "")]
      [InlineData(null, "", "")]
      public void TheHosts_AreTheServers_AndTheNamesLeftOutAreNamed(string domains, string usable, string unusable)
      {
         Assert.Equal(Split(usable), AcmeHostNames.Usable(domains));
         Assert.Equal(Split(unusable), AcmeHostNames.Unusable(domains));
      }

      [Fact]
      public void AHostName_IsHeldToTheServersLengths()
      {
         Assert.True(AcmeHostNames.IsUsableHostName(new string('a', 63) + ".example.test"));
         Assert.False(AcmeHostNames.IsUsableHostName(new string('a', 64) + ".example.test"));
         string long254 = string.Concat(System.Linq.Enumerable.Repeat("a.", 127)).Substring(0, 252) + "aa";
         Assert.Equal(254, long254.Length);
         Assert.False(AcmeHostNames.IsUsableHostName(long254));
         Assert.True(AcmeHostNames.IsUsableHostName("localhost"));
      }

      [Fact]
      public void OnlyNamesNoCertificateCanCarry_AreSaidAtCritical_BeforeTheNetwork()
      {
         string code = Source();
         if (code == null)
            return;

         Match body = Regex.Match(code, @"private WarningState ComputeAcmeCertificateState_\(\)\s*\{(.*?)\n      \}", RegexOptions.Singleline);
         Assert.True(body.Success, "ComputeAcmeCertificateState_ is gone.");
         string state = body.Groups[1].Value.Replace("\r\n", "\n");

         int none = state.IndexOf("if (AcmeHostNames.Usable(acmeDomains).Count == 0)", StringComparison.Ordinal);
         int remote = state.IndexOf("if (!ServerSession.IsLocalSession)", StringComparison.Ordinal);
         int network = state.IndexOf("the CA could not reach this server", StringComparison.Ordinal);
         Assert.True(none >= 0, "The page does not say that no name it holds can be ordered for.");
         Assert.True(none < remote && none < network, "That no name can be ordered for is said after the network.");
         Assert.Contains("Level = StatusLevel.Critical,\n               Text = F(\"No certificate can be issued: no certificate can name {0}, which the server leaves out.", state, StringComparison.Ordinal);
         Assert.Contains("string.Join(\", \", AcmeHostNames.Unusable(acmeDomains))", state, StringComparison.Ordinal);
      }

      [Fact]
      public void TheSetupChecklist_ReadsTheListAsTheServerDoes()
      {
         string code = Source("Services", "ExternalSetupChecks.cs");
         if (code == null)
            return;

         Match body = Regex.Match(code, @"private SetupItem AcmeItem\(.*?\n      \}", RegexOptions.Singleline);
         Assert.True(body.Success, "AcmeItem is gone.");
         string item = body.Value;

         // Every name not blank was one whose DNS record and port were to be confirmed.
         Assert.DoesNotContain(".Split(',')", item, StringComparison.Ordinal);
         Assert.Contains("AcmeHostNames.Usable(domainList)", item, StringComparison.Ordinal);
         Assert.Contains("AcmeHostNames.Unusable(domainList)", item, StringComparison.Ordinal);
         Assert.Contains("no certificate can name this, so the server leaves it out of every order", item, StringComparison.Ordinal);
         Assert.Contains("can be named in a certificate, so no certificate is ever requested", item, StringComparison.Ordinal);
      }

      private static string[] Split(string names)
         => names.Length == 0 ? Array.Empty<string>() : names.Split('|');

      [Fact]
      public void TheCertificatesState_IsTheCardsFirstRow()
      {
         string code = Source();
         if (code == null)
            return;

         Assert.Matches(new Regex(@"Aid = ""AcmeCertificateState"",\s*First = true,\s*Compute = ComputeAcmeCertificateState_"), code);
         Assert.Contains("fields.Children.Insert(0, first);", code, StringComparison.Ordinal);
      }

      [Fact]
      public void NoHostName_IsSaidAtCritical_BeforeAndInPlaceOfTheNetwork()
      {
         string code = Source();
         if (code == null)
            return;

         Match body = Regex.Match(code, @"private WarningState ComputeAcmeCertificateState_\(\)\s*\{(.*?)\n      \}", RegexOptions.Singleline);
         Assert.True(body.Success, "ComputeAcmeCertificateState_ is gone.");
         string state = body.Groups[1].Value;

         int none = state.IndexOf("if (!AcmeHostNames.Any(LiveText_(\"AcmeDomains\", \"\")))", StringComparison.Ordinal);
         int remote = state.IndexOf("if (!ServerSession.IsLocalSession)", StringComparison.Ordinal);
         int network = state.IndexOf("the CA could not reach this server", StringComparison.Ordinal);
         Assert.True(none >= 0, "The page does not say that no host name is set.");
         Assert.True(none < remote && none < network, "No host name is said after the network.");
         Assert.Contains("Level = StatusLevel.Critical,\n               Text = L(\"No certificate can be issued without a host name: add at least one to the host names below.\")",
            state.Replace("\r\n", "\n"), StringComparison.Ordinal);
      }

      [Fact]
      public void AnIssuedCertificate_CarriesTheWayToTheCertificatesPage()
      {
         string code = Source();
         if (code == null)
            return;

         Match body = Regex.Match(code, @"private WarningState ComputeAcmeCertificateState_\(\)\s*\{(.*?)\n      \}", RegexOptions.Singleline);
         Assert.True(body.Success);
         Assert.True(Regex.Matches(body.Groups[1].Value, @"Page = ""certs""").Count >= 4, "The issued certificate's states do not open the TLS certificates page.");
         Assert.Contains("row.Action = NoticeActions.OpenPage(state.Page,", code, StringComparison.Ordinal);
      }

      [Fact]
      public void TheDirectory_ShowsLetsEncryptsUrl()
      {
         string code = Source();
         if (code == null)
            return;

         // The box holds Let's Encrypt's URL as its default value; a placeholder of the
         // same URL showed it again as an example (the final check of Tier 2).
         Assert.Matches(new Regex(@"Key = ""AcmeDirectoryUrl"",[^}]*Default = ""https://acme-v02\.api\.letsencrypt\.org/directory"""), code);
         Assert.DoesNotMatch(new Regex(@"Key = ""AcmeDirectoryUrl"",[^}]*Placeholder ="), code);
      }

      private static string Source(string folder = "Views", string file = "FeatureSettingsView.xaml.cs")
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string path = Path.Join(directory.FullName, "ControlPanel", folder, file);
            if (File.Exists(path))
               return File.ReadAllText(path);
         }

         return null;
      }
   }
}
