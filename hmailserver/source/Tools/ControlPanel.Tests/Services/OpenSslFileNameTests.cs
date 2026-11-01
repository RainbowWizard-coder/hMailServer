// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The local checks hold a file's name to the rule the server holds it to before it
   /// hands the name to OpenSSL on Windows (FileUtilities::FileNameBytes, through
   /// FileUtilities::OpenSslPath; HM7536 in the error log). The server refuses a listener's
   /// certificate or key file, a port's client-certificate CA file or OAuth2PublicKeyFile
   /// so named and reads nothing from it, while this panel reads the same file by its
   /// wide name, which always works: the certificates page said "Valid until" and the
   /// setup checklist "Done" for a file whose port did not start.
   /// </summary>
   public class OpenSslFileNameTests
   {
      private static string Name(params int[] characters)
      {
         var text = new char[characters.Length];
         for (int i = 0; i < characters.Length; i++)
            text[i] = (char)characters[i];
         return @"C:\" + new string(text) + @"\ca.pem";
      }

      // U+00C3 U+00A9: C3 A9 on code page 1252, which is U+00E9 in UTF-8.
      private static readonly string TwoCharacters = Name(0x00C3, 0x00A9);

      // U+00E9 alone: E9 on code page 1252, which is no UTF-8.
      private static readonly string Eacute = Name(0x00E9);

      // JOSE'S with a capital E acute and a closing quotation mark: C9 92 on code page
      // 1252, which is U+0252 in UTF-8.
      private static readonly string Joses = Name('J', 'O', 'S', 0x00C9, 0x2019, 'S');

      // A Cyrillic letter, which code page 1252 does not have and 1251 does (C6).
      private static readonly string Zhe = Name(0x0416);

      // A letter 1252 does not have and would put a best-fit A in the place of.
      private static readonly string Amacron = Name(0x0100);

      // Half of a surrogate pair, and a whole pair.
      private static readonly string HalfPair = Name(0xD800);
      private static readonly string WholePair = Name(0xD83D, 0xDE00);

      [Theory]
      [InlineData(1252u)]
      [InlineData(1251u)]
      [InlineData(932u)]
      [InlineData(OpenSslFileName.Utf8CodePage)]
      public void ANameInAscii_IsHandedOver(uint codePage)
      {
         Assert.False(OpenSslFileName.IsRefused(@"C:\Program Files\hMailServer\Data\ACME\fullchain.pem", codePage));
         Assert.False(OpenSslFileName.IsRefused("", codePage));
         Assert.False(OpenSslFileName.IsRefused(null, codePage));
      }

      /// <summary>The server's own cases (FileUtilitiesTester::TestFileNameEncoding), and the rest of its rule, on code page 1252.</summary>
      [Fact]
      public void OnCodePage1252_ANameIsRefused_AsTheServerRefusesIt()
      {
         Assert.True(OpenSslFileName.IsRefused(TwoCharacters, 1252), "C3 A9 reads as U+00E9 in UTF-8.");
         Assert.True(OpenSslFileName.IsRefused(Joses, 1252), "C9 92 reads as U+0252 in UTF-8.");
         Assert.True(OpenSslFileName.IsRefused(Zhe, 1252), "1252 has no Cyrillic.");
         Assert.True(OpenSslFileName.IsRefused(Amacron, 1252), "A best-fit letter was taken for U+0100.");
         Assert.True(OpenSslFileName.IsRefused(HalfPair, 1252), "Half of a surrogate pair was handed over.");
         Assert.True(OpenSslFileName.IsRefused("C:\\a\0b\\ca.pem", 1252), "U+0000 was handed over.");

         Assert.False(OpenSslFileName.IsRefused(Eacute, 1252), "E9 alone is no UTF-8, and the file's own name in 1252.");
      }

      [Fact]
      public void OnCodePage1251_ACyrillicName_IsHandedOver()
      {
         Assert.False(OpenSslFileName.IsRefused(Zhe, 1251));
      }

      /// <summary>
      /// Under the UTF-8 code page the server hands the UTF-8 over, which is the file's own
      /// name to OpenSSL, and refuses only what UTF-8 cannot carry or a narrow call would cut.
      /// </summary>
      [Fact]
      public void UnderTheUtf8CodePage_OnlyHalfAPairAndU0000_AreRefused()
      {
         uint utf8 = OpenSslFileName.Utf8CodePage;
         Assert.False(OpenSslFileName.IsRefused(TwoCharacters, utf8));
         Assert.False(OpenSslFileName.IsRefused(Joses, utf8));
         Assert.False(OpenSslFileName.IsRefused(Zhe, utf8));
         Assert.False(OpenSslFileName.IsRefused(Amacron, utf8));
         Assert.False(OpenSslFileName.IsRefused(WholePair, utf8));

         Assert.True(OpenSslFileName.IsRefused(HalfPair, utf8));
         Assert.True(OpenSslFileName.IsRefused(Name(0xDE00, 'a'), utf8));
         Assert.True(OpenSslFileName.IsRefused("C:\\a\0b\\ca.pem", utf8));
      }

      /// <summary>
      /// A certificate and its key in a folder whose name the server refuses on this
      /// computer are Critical on the certificates page, saying error 7536, where the same
      /// two files in a folder named in ASCII are Valid: it is the name alone.
      /// </summary>
      [Fact]
      public void ACertificateInAFolderTheServerRefuses_IsCritical_NamingError7536()
      {
         string refusedFolder = null;
         foreach (string candidate in new[] { "x" + TwoCharacters.Substring(3, 2), "x" + Zhe.Substring(3, 1), "x" + HalfPair.Substring(3, 1) })
         {
            if (OpenSslFileName.IsRefusedHere(candidate))
            {
               refusedFolder = candidate;
               break;
            }
         }
         Assert.SkipWhen(refusedFolder == null, "No candidate name is refused under this computer's code page.");

         string root = Path.Combine(Path.GetTempPath(), "openssl-name-" + Guid.NewGuid().ToString("N"));
         try
         {
            string plain = Path.Combine(root, "plain");
            string refused = Path.Combine(root, refusedFolder);
            Directory.CreateDirectory(plain);
            Directory.CreateDirectory(refused);

            using (RSA key = RSA.Create(2048))
            {
               var request = new CertificateRequest("CN=mail.example.com", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
               using X509Certificate2 certificate = request.CreateSelfSigned(DateTimeOffset.Now.AddDays(-1), DateTimeOffset.Now.AddDays(200));
               foreach (string folder in new[] { plain, refused })
               {
                  File.WriteAllText(Path.Combine(folder, "mail.crt"), certificate.ExportCertificatePem());
                  File.WriteAllText(Path.Combine(folder, "mail.key"), key.ExportPkcs8PrivateKeyPem());
               }
            }

            CertificateHealth good = CertificateInspector.Inspect(Path.Combine(plain, "mail.crt"), Path.Combine(plain, "mail.key"), true);
            Assert.Equal(StatusLevel.Good, good.CertificateFile.Level);
            Assert.Equal(StatusLevel.Good, good.PrivateKeyFile.Level);
            Assert.Equal(StatusLevel.Good, good.Pair.Level);

            string certificateFile = Path.Combine(refused, "mail.crt");
            string keyFile = Path.Combine(refused, "mail.key");
            Assert.True(File.Exists(certificateFile) && File.Exists(keyFile), "The files in the refused folder were not written.");

            CertificateHealth health = CertificateInspector.Inspect(certificateFile, keyFile, true);
            Assert.Equal(StatusLevel.Critical, health.CertificateFile.Level);
            Assert.Equal("Name refused", health.CertificateFile.Word);
            Assert.Contains(certificateFile, health.CertificateFile.Detail, StringComparison.Ordinal);
            Assert.Contains("server error 7536", health.CertificateFile.Detail, StringComparison.Ordinal);
            Assert.Contains("fails to start", health.CertificateFile.Detail, StringComparison.Ordinal);
            Assert.Null(health.ExpiresOn);

            Assert.Equal(StatusLevel.Critical, health.PrivateKeyFile.Level);
            Assert.Equal("Name refused", health.PrivateKeyFile.Word);
            Assert.Contains(keyFile, health.PrivateKeyFile.Detail, StringComparison.Ordinal);
            Assert.Contains("server error 7536", health.PrivateKeyFile.Detail, StringComparison.Ordinal);

            Assert.NotEqual(StatusLevel.Good, health.Pair.Level);

            // Read from another computer the files are not looked at, and nothing is said of the name.
            CertificateHealth remote = CertificateInspector.Inspect(certificateFile, keyFile, false);
            Assert.Equal(StatusLevel.Information, remote.CertificateFile.Level);
            Assert.Equal("Cannot be checked from here", remote.CertificateFile.Word);
         }
         finally
         {
            if (Directory.Exists(root))
               Directory.Delete(root, true);
         }
      }

      /// <summary>
      /// The setup checklist's items that read these files ask the server's rule before
      /// they look for the file and call it present or good: the TLS private keys, the
      /// client-certificate CA files and OAuth2PublicKeyFile. They read the server over
      /// COM, so they are held here by their source.
      /// </summary>
      [Theory]
      [InlineData("keyFile")]
      [InlineData("caFile")]
      [InlineData("publicKeyFile")]
      public void TheSetupChecklist_AsksTheServersRule_BeforeItLooksForTheFile(string variable)
      {
         string code = SaveStateModelTests.ReadControlPanelFile("Services/ExternalSetupChecks.cs");

         int rule = code.IndexOf("OpenSslFileName.IsRefusedHere(" + variable + ")", StringComparison.Ordinal);
         int exists = code.IndexOf("File.Exists(" + variable + ")", StringComparison.Ordinal);
         Assert.True(rule >= 0, "ExternalSetupChecks does not ask OpenSslFileName about " + variable + ".");
         Assert.True(exists >= 0, "ExternalSetupChecks no longer looks for " + variable + ".");
         Assert.True(rule < exists, "ExternalSetupChecks looks for " + variable + " before it asks the server's rule.");

         string after = code.Substring(rule, Math.Min(900, code.Length - rule));
         Assert.Contains("OpenSslFileName.Explanation()", after, StringComparison.Ordinal);
         Assert.Contains("SetupItemState.ActionNeeded", after, StringComparison.Ordinal);
      }

      [Fact]
      public void TheExplanation_NamesTheServersError()
      {
         Assert.Contains("server error 7536", OpenSslFileName.Explanation(), StringComparison.Ordinal);
         Assert.Contains("UTF-8", OpenSslFileName.Explanation(), StringComparison.Ordinal);
      }
   }
}
