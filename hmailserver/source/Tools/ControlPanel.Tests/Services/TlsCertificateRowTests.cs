// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The transport encryption overview's certificate rows are laid out as its listener
   /// rows are (review finding F326): the name and whether a listener uses it on the
   /// left, the sentence in the middle, the state in a pill of its own words on the
   /// right - Valid, Expires soon, Has expired, the SSL certificates page's word for a
   /// problem with the files, Not checked. They put the pill on the left with the
   /// level's word, "OK" or "Warning", before a run-on of the name, the finding and the
   /// use joined by a dash and commas, where the listener rows above say a state.
   /// </summary>
   public class TlsCertificateRowTests
   {
      private static readonly DateTime Today = new(2026, 10, 10);

      private static TlsCertificate Certificate(int? days, bool inUse, string problem = null, string word = null)
         => new()
         {
            Name = "mail.example.com",
            DaysRemaining = days,
            ExpiresOn = days == null ? null : Today.AddDays(days.Value).AddHours(12),
            InUse = inUse,
            Problem = problem,
            ProblemWord = word
         };

      [Theory]
      [InlineData(60, true, StatusLevel.Good, "Valid", "Expires in 60 days.")]
      [InlineData(60, false, StatusLevel.Good, "Valid", "Expires in 60 days.")]
      [InlineData(12, true, StatusLevel.Warning, "Expires soon", "Expires in 12 days.")]
      [InlineData(12, false, StatusLevel.Information, "Expires soon", "Expires in 12 days.")]
      [InlineData(1, true, StatusLevel.Warning, "Expires soon", "Expires in 1 day.")]
      [InlineData(0, true, StatusLevel.Warning, "Expires soon", "Expires today.")]
      [InlineData(-1, true, StatusLevel.Critical, "Has expired", "Expired 1 day ago.")]
      [InlineData(-5, false, StatusLevel.Warning, "Has expired", "Expired 5 days ago.")]
      public void ACertificatesRow_SaysItsStateInWordsOfItsOwn(int days, bool inUse, StatusLevel level, string state, string detail)
      {
         TlsCertificateRow row = TlsPosture.CertificateRow(new TlsPostureConfig(), Certificate(days, inUse), Today);

         Assert.Equal(level, row.Level);
         Assert.Equal(state, row.State);
         Assert.Equal(detail, row.Detail);
         Assert.Equal(inUse ? "In use by a listener" : "Not used by any listener", row.Usage);
      }

      /// <summary>The levels are the ones the notes give the same certificate, from the same days.</summary>
      [Theory]
      [InlineData(60, true)]
      [InlineData(31, true)]
      [InlineData(30, true)]
      [InlineData(12, false)]
      [InlineData(-3, true)]
      [InlineData(-3, false)]
      public void ARowsLevel_IsTheNotesLevel(int days, bool inUse)
      {
         var config = new TlsPostureConfig();
         TlsCertificate certificate = Certificate(days, inUse);
         config.Certificates.Add(certificate);

         TlsCertificateRow row = TlsPosture.CertificateRow(config, certificate, Today);
         TlsPostureNote note = null;
         foreach (TlsPostureNote candidate in TlsPosture.Notes(config, Today))
         {
            if (candidate.Text.Contains("\"mail.example.com\"", StringComparison.Ordinal))
               note = candidate;
         }

         if (days > TlsPosture.ExpiryWarningDays)
         {
            Assert.Null(note);
            Assert.Equal(StatusLevel.Good, row.Level);
         }
         else
         {
            Assert.NotNull(note);
            Assert.Equal(note.Level, row.Level);
         }
      }

      [Fact]
      public void AProblemWithTheFiles_IsTheCertificatesPagesWordAndSentence()
      {
         const string problem = "The certificate file does not exist on this machine.";

         TlsCertificateRow used = TlsPosture.CertificateRow(new TlsPostureConfig(), Certificate(200, true, problem, "File missing"), Today);
         Assert.Equal(StatusLevel.Critical, used.Level);
         Assert.Equal("File missing", used.State);
         Assert.Equal(problem, used.Detail);

         TlsCertificateRow unused = TlsPosture.CertificateRow(new TlsPostureConfig(), Certificate(200, false, problem, "File missing"), Today);
         Assert.Equal(StatusLevel.Warning, unused.Level);
         Assert.Equal("Not used by any listener", unused.Usage);
      }

      [Fact]
      public void ACertificateNotReadFromHere_IsNotChecked_AtTheLevelThatSaysNothing()
      {
         var remote = new TlsPostureConfig { CertificateFilesReadable = false };
         TlsCertificateRow row = TlsPosture.CertificateRow(remote, Certificate(60, true), Today);
         Assert.Equal(StatusLevel.Normal, row.Level);
         Assert.Equal("Not checked", row.State);
         Assert.Equal("Expiry not checked from here.", row.Detail);

         TlsCertificateRow unknown = TlsPosture.CertificateRow(new TlsPostureConfig(), Certificate(null, false), Today);
         Assert.Equal(StatusLevel.Normal, unknown.Level);
      }

      // ---- a certificate as the page reads it ------------------------------------

      /// <summary>
      /// A certificate that has expired, written to a file and read as the page reads it
      /// (CertificateInspector, then TlsPosture.TakeInspection), Has expired, in the row's
      /// own words. The certificate file's finding is Critical for every expired
      /// certificate, and the page took it as the problem, so that the row said the
      /// certificates grid's "Expired on 2026-09-27 (13 days ago)" - an ISO date, cut in
      /// German - with that page's long sentence beside it, and its Has expired was
      /// reached only by the hand-made certificates above (review finding F326).
      /// </summary>
      [Theory]
      [InlineData(13, true, StatusLevel.Critical, "Expired 13 days ago.")]
      [InlineData(13, false, StatusLevel.Warning, "Expired 13 days ago.")]
      [InlineData(400, true, StatusLevel.Critical, "Expired 400 days ago.")]
      [InlineData(1, true, StatusLevel.Critical, "Expired 1 day ago.")]
      [InlineData(0, true, StatusLevel.Critical, "Expired today.")]
      public void AnExpiredCertificate_AsThePageReadsIt_HasExpired(int daysAgo, bool inUse, StatusLevel level, string detail)
      {
         DateTime today = DateTime.Today;
         // Midday that many days ago, or, today, halfway between midnight and now: past,
         // and on that calendar day whatever the time the test runs at.
         DateTime expired = daysAgo == 0
            ? today + TimeSpan.FromTicks((DateTime.Now - today).Ticks / 2)
            : today.AddDays(-daysAgo).AddHours(12);

         using var files = new CertificateFiles(expired.AddDays(-90), expired);
         TlsCertificate certificate = AsThePageReadsIt(files, inUse);

         Assert.Null(certificate.Problem);
         Assert.Null(certificate.ProblemWord);
         Assert.True(certificate.DaysRemaining < 0);

         TlsCertificateRow row = TlsPosture.CertificateRow(new TlsPostureConfig(), certificate, today);
         Assert.Equal(level, row.Level);
         Assert.Equal("Has expired", row.State);
         Assert.Equal(detail, row.Detail);
      }

      /// <summary>
      /// The note on an expired certificate in use, read as the page reads it, is the
      /// expiry's, at Critical, and not "has a problem:" before the certificates page's
      /// sentence with its ISO date.
      /// </summary>
      [Fact]
      public void AnExpiredCertificateInUse_AsThePageReadsIt_IsTheExpiredNote()
      {
         DateTime today = DateTime.Today;
         using var files = new CertificateFiles(today.AddDays(-103).AddHours(12), today.AddDays(-13).AddHours(12));
         TlsCertificate certificate = AsThePageReadsIt(files, true);

         var config = new TlsPostureConfig();
         config.Certificates.Add(certificate);
         TlsPostureNote[] notes = TlsPosture.Notes(config, today)
            .Where(n => n.Text.Contains("\"mail.example.com\"", StringComparison.Ordinal))
            .ToArray();

         TlsPostureNote note = Assert.Single(notes);
         Assert.Equal(StatusLevel.Critical, note.Level);
         Assert.Equal(TlsPosture.ExpiredNote("mail.example.com", 13, true), note.Text);
         Assert.Equal(TlsPosture.CertificateRow(config, certificate, today).Level, note.Level);
      }

      /// <summary>
      /// A problem with the files is still the problem, past the expiry as well: an
      /// expired certificate whose key is another's does not match, and a certificate
      /// file that is not there is missing, each in the certificates page's word.
      /// </summary>
      [Fact]
      public void AProblemWithTheFiles_AsThePageReadsIt_IsStillTheProblem()
      {
         DateTime today = DateTime.Today;

         using (var mismatched = new CertificateFiles(today.AddDays(-90), today.AddDays(-13).AddHours(12), mismatchedKey: true))
         {
            TlsCertificateRow row = TlsPosture.CertificateRow(new TlsPostureConfig(), AsThePageReadsIt(mismatched, true), today);
            Assert.Equal(StatusLevel.Critical, row.Level);
            Assert.Equal("Does not match the certificate", row.State);
         }

         using (var missing = new CertificateFiles(today.AddDays(-10), today.AddDays(200)))
         {
            File.Delete(missing.CertificateFile);
            TlsCertificate certificate = AsThePageReadsIt(missing, true);
            Assert.Null(certificate.DaysRemaining);

            TlsCertificateRow row = TlsPosture.CertificateRow(new TlsPostureConfig(), certificate, today);
            Assert.Equal(StatusLevel.Critical, row.Level);
            Assert.Equal("File missing", row.State);
         }
      }

      /// <summary>A certificate that is fine, read as the page reads it, is Valid, with its days.</summary>
      [Fact]
      public void AGoodCertificate_AsThePageReadsIt_IsValid()
      {
         DateTime today = DateTime.Today;
         using var files = new CertificateFiles(today.AddDays(-10), today.AddDays(200).AddHours(12));
         TlsCertificate certificate = AsThePageReadsIt(files, true);

         Assert.Null(certificate.Problem);
         TlsCertificateRow row = TlsPosture.CertificateRow(new TlsPostureConfig(), certificate, today);
         Assert.Equal(StatusLevel.Good, row.Level);
         Assert.Equal("Valid", row.State);
         Assert.Equal("Expires in 200 days.", row.Detail);
      }

      private static TlsCertificate AsThePageReadsIt(CertificateFiles files, bool inUse)
      {
         var certificate = new TlsCertificate { Name = "mail.example.com", InUse = inUse };
         TlsPosture.TakeInspection(certificate, CertificateInspector.Inspect(files.CertificateFile, files.KeyFile, true));
         return certificate;
      }

      /// <summary>
      /// A self-signed certificate and its key, in PEM files in a folder of their own,
      /// valid from <c>notBefore</c> to <c>notAfter</c> (local time); the key another
      /// key's where <c>mismatchedKey</c>.
      /// </summary>
      private sealed class CertificateFiles : IDisposable
      {
         private readonly string folder_ = Path.Combine(Path.GetTempPath(), "tls-row-" + Guid.NewGuid().ToString("N"));

         public CertificateFiles(DateTime notBefore, DateTime notAfter, bool mismatchedKey = false)
         {
            Directory.CreateDirectory(folder_);
            CertificateFile = Path.Combine(folder_, "mail.crt");
            KeyFile = Path.Combine(folder_, "mail.key");

            using RSA key = RSA.Create(2048);
            var request = new CertificateRequest("CN=mail.example.com", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            using X509Certificate2 certificate = request.CreateSelfSigned(new DateTimeOffset(notBefore), new DateTimeOffset(notAfter));
            File.WriteAllText(CertificateFile, certificate.ExportCertificatePem());

            using RSA other = mismatchedKey ? RSA.Create(2048) : null;
            File.WriteAllText(KeyFile, (other ?? key).ExportPkcs8PrivateKeyPem());
         }

         public string CertificateFile { get; }

         public string KeyFile { get; }

         public void Dispose() => Directory.Delete(folder_, true);
      }

      /// <summary>The page draws the row in the listeners' three columns, the pill last, with no dash run-on.</summary>
      [Fact]
      public void ThePage_DrawsTheCertificatesInTheListenersLayout()
      {
         string code = SaveStateModelTests.ReadControlPanelFile("Views/TlsOverviewView.cs").Replace("\r\n", "\n");

         int start = code.IndexOf("private static void AddCertificateRow(", StringComparison.Ordinal);
         Assert.True(start >= 0, "TlsOverviewView.cs has no AddCertificateRow.");
         string row = code.Substring(start, code.IndexOf("private Card NegotiationCard(", start, StringComparison.Ordinal) - start);

         Assert.Contains("TlsPosture.CertificateRow(config, certificate, DateTime.Today)", row, StringComparison.Ordinal);
         Assert.Contains("Grid.SetColumn(identity, 0);", row, StringComparison.Ordinal);
         Assert.Contains("Grid.SetColumn(text, 1);", row, StringComparison.Ordinal);
         Assert.Contains("Grid.SetColumn(pill, 2);", row, StringComparison.Ordinal);
         Assert.Contains("Text = state,", row, StringComparison.Ordinal);
         Assert.DoesNotContain("SeverityWord", row, StringComparison.Ordinal);
         Assert.DoesNotContain("certificate.Name + \" — \"", code, StringComparison.Ordinal);

         // The certificates are read through TlsPosture.TakeInspection, which the tests above
         // hold, and not by a choice of the page's own.
         Assert.Contains("TlsPosture.TakeInspection(entry, CertificateInspector.Inspect(", code, StringComparison.Ordinal);
         Assert.DoesNotContain("FirstProblem", code, StringComparison.Ordinal);
      }
   }
}
