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
         Assert.True(Regex.Matches(body.Groups[1].Value, @"Page = ""certs""").Count >= 4, "The issued certificate's states do not open the SSL certificates page.");
         Assert.Contains("row.Action = NoticeActions.OpenPage(state.Page,", code, StringComparison.Ordinal);
      }

      [Fact]
      public void TheDirectory_ShowsLetsEncryptsUrl()
      {
         string code = Source();
         if (code == null)
            return;

         Assert.Matches(new Regex(@"Key = ""AcmeDirectoryUrl"",[^}]*Placeholder = ""https://acme-v02\.api\.letsencrypt\.org/directory"""), code);
      }

      private static string Source()
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string path = Path.Join(directory.FullName, "ControlPanel", "Views", "FeatureSettingsView.xaml.cs");
            if (File.Exists(path))
               return File.ReadAllText(path);
         }

         return null;
      }
   }
}
