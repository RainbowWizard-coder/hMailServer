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
   /// The captions and marks of the certificates and access pages (review finding
   /// F311): the apply marks said "Applies on save", "After reinitialise" and "After
   /// a restart"; SSL/TLS's tab was "Ciphers" over its card "Ciphers & verification"
   /// (German left the word untranslated); the IP range's four boxes each repeated
   /// "Require auth:" under a caption that says it; Transport security's trust
   /// anchors were captioned by their format string; a certificate's details said
   /// again what the word beside each said; the status columns, "Certificate" and
   /// "Private key", read as repeats of the file columns; and the page loaded under a
   /// bare "Loading…". The auto-ban captions the finding names are T3.1 item 9's
   /// (review finding F143), which puts them in the sign-in vocabulary.
   /// </summary>
   public class CertificatesAndAccessCopyTests
   {
      [Fact]
      public void TheCiphersTabIsNamedAsItsCard()
      {
         string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         Assert.Contains("Tab(L(\"Ciphers & verification\")).Cards.Add(ciph);", page, StringComparison.Ordinal);
         Assert.DoesNotContain("Tab(L(\"Ciphers\"))", page, StringComparison.Ordinal);
      }

      [Fact]
      public void TheRangeDirectionsAreCaptionedByTheirDirection_TheTabsCaptionTheirHelp()
      {
         string dialog = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "IPRangeDialog.cs"));
         Assert.DoesNotContain("Require auth: ", dialog, StringComparison.Ordinal);
         // Each direction once, a row of its Allow and Only after SMTP authentication boxes, which
         // the row's caption and the column's name (DeliveryRow) - t213b's Delivery tab (F229).
         foreach (string caption in new[] { "_Local to local", "Lo_cal to external", "E_xternal to local", "External to external (rela_y)" })
            Assert.Contains("(L(\"" + caption + "\"), ", dialog, StringComparison.Ordinal);
         Assert.Contains("p.Children.Add(DeliveryRow.BuildGrid(new[]", dialog, StringComparison.Ordinal);
      }

      [Fact]
      public void TheTrustAnchorsAreCaptioned_WithTheFormatAndAnExampleUnderThem()
      {
         string feature = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml.cs"));
         Assert.Contains("Label = L(\"Trust anchors (DS records, separated by semicolons)\"),", feature, StringComparison.Ordinal);
         Match blurb = Regex.Match(feature, "Key = \"DnssecTrustAnchors\",[^}]*Blurb = L\\(\"([^\"]*)\"\\)");
         Assert.True(blurb.Success, "The trust anchors have no blurb.");
         // The example is the IANA root's KSK-2017, as DnssecResolver's built-in anchors spell it.
         Assert.Contains("20326 8 2 E06D44B80B8F1D39A95C0B0D7C65D08458E880409BBC683457104237C7F8EC8D", blurb.Groups[1].Value, StringComparison.Ordinal);
         Assert.DoesNotContain("tag alg digesttype hex", feature, StringComparison.Ordinal);
      }

      [Fact]
      public void TheCertificatesStatusColumnsAndLoadingSayWhatTheyAre()
      {
         string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "SslCertificatesView.xaml"));
         Assert.Contains("Header=\"{loc:L 'Certificate status'}\"", page, StringComparison.Ordinal);
         Assert.Contains("Header=\"{loc:L 'Key status'}\"", page, StringComparison.Ordinal);
         Assert.Contains("<scaffold:LoadingState x:Name=\"Loading\" Text=\"{loc:L 'Checking the certificate files…'}\"", page, StringComparison.Ordinal);
      }

      [Fact]
      public void ACertificatesDetailSaysOnlyWhatItsWordDoesNot()
      {
         string inspector = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Services", "CertificateInspector.cs"));
         // The word says the date; the sentence beside it no longer says it again.
         foreach (string repeat in new[] { "The certificate expired on {0}.", "The certificate is not valid until {0}.", "The certificate expires on {0}",
                                           "parses correctly and is valid until", "The key file exists and is", "identical to the certificate's public key" })
            Assert.DoesNotContain(repeat, inspector, StringComparison.Ordinal);
         foreach (string detail in new[] { "The file parses as a PEM certificate, the format the server loads.", "A plain (unencrypted) PEM, so no passphrase is needed.",
                                           "The private key's public half is the certificate's public key.",
                                           "Renew it before then: once it expires the ports keep listening, but clients that check the date refuse to connect." })
            Assert.Contains("L(\"" + detail + "\")", inspector, StringComparison.Ordinal);

         // The passphrase line beside "No passphrase needed".
         string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "SslCertificatesView.xaml.cs"));
         Assert.Contains("L(\"The key file is not encrypted, and none is stored.\")", page, StringComparison.Ordinal);
      }
   }
}
