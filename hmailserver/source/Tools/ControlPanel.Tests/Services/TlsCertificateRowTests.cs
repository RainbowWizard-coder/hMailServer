// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
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
      }
   }
}
