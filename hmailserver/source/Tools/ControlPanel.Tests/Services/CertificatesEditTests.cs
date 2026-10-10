// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F235: the SSL certificates page edited only a certificate's
   /// passphrase, so a certificate whose file had moved was put right by deleting it
   /// and adding it again - a new id, and every TCP/IP port that presented the old one
   /// left with none; no column said which ports present each, though the page's
   /// purpose is "which port uses which"; the form took PEM alone and said nothing of
   /// the .pfx the search sends readers here for; and the Certificate column sorted by
   /// its status word. Edit... saves a certificate in place, a Used by column names the
   /// ports, the form says how a .pfx is made into PEM, and the column sorts by how
   /// much a certificate asks for attention and then by how soon it expires.
   /// </summary>
   public class CertificatesEditTests
   {
      [Fact]
      public void UsedBy_NamesThePorts_OrSaysNoneOrNotKnown()
      {
         Assert.Equal("SMTP 465, IMAP 993", CertificateRows.UsedBy(new List<string> { "SMTP 465", "IMAP 993" }));
         Assert.Equal("Not used", CertificateRows.UsedBy(new List<string>()));
         // Ports that could not be read are not "no port".
         Assert.Equal("Not known", CertificateRows.UsedBy(null));
      }

      /// <summary>
      /// The Used by column cut a certificate's three ports to "SMTP 587, SMTP 4..." at
      /// 1280 by 800, the whole list in a tool tip the keyboard never reaches, and the
      /// details card did not name them (the skeptic's review of F235). The card says
      /// them whole, at Normal, with what they do - or that none does and how one does,
      /// or, at Information, that the ports could not be read - between the key's match
      /// and the passphrase, whose editor follows its line.
      /// </summary>
      [Fact]
      public void TheDetailsCard_SaysThePortsWhole()
      {
         (StatusLevel level, string word, string detail) = CertificateRows.UsedByLine(new List<string> { "SMTP 587", "SMTP 465", "IMAP 993" });
         Assert.Equal(StatusLevel.Normal, level);
         Assert.Equal("SMTP 587, SMTP 465, IMAP 993", word);
         Assert.Equal("These TCP/IP ports present this certificate to the clients that connect to them.", detail);

         (level, word, detail) = CertificateRows.UsedByLine(new List<string>());
         Assert.Equal(StatusLevel.Normal, level);
         Assert.Equal("Not used", word);
         Assert.Equal("No TCP/IP port presents this certificate. Choose it for a port on the TCP/IP ports page to use it.", detail);

         (level, word, detail) = CertificateRows.UsedByLine(null);
         Assert.Equal(StatusLevel.Information, level);
         Assert.Equal("Not known", word);
         Assert.Equal("The TCP/IP ports could not be read, so which of them present this certificate is not known.", detail);

         string page = Read("SslCertificatesView.xaml.cs");
         if (page == null)
            return;

         Assert.Contains("row.UsedByLine = new CertificateFinding(usedLevel, usedWord, usedDetail);", page, StringComparison.Ordinal);
         int pair = page.IndexOf("DetailLine(L(\"Key matches certificate\"), health.Pair)", StringComparison.Ordinal);
         int used = page.IndexOf("DetailLine(L(\"Used by\"), row.UsedByLine)", StringComparison.Ordinal);
         int passphrase = page.IndexOf("DetailLine(L(\"Passphrase\"), row.PassphraseLine)", StringComparison.Ordinal);
         Assert.True(pair > 0 && used > pair && passphrase > used, "The details card does not say Used by between the key's match and the passphrase.");
      }

      [Fact]
      public void TheCertificateColumn_SortsTheMostUrgentFirst_ThenTheSoonestToExpire()
      {
         var rows = new List<(string Name, StatusLevel Level, int? Days)>
         {
            ("good, a year", StatusLevel.Good, 365),
            ("expires soon", StatusLevel.Warning, 9),
            ("expired", StatusLevel.Critical, -3),
            ("good, a month", StatusLevel.Good, 40),
            ("missing file", StatusLevel.Critical, null),
            ("cannot be checked", StatusLevel.Information, null)
         };

         rows.Sort((a, b) => CertificateRows.Compare(a.Level, a.Days, b.Level, b.Days));

         Assert.Equal(new[] { "expired", "missing file", "expires soon", "cannot be checked", "good, a month", "good, a year" },
            rows.ConvertAll(r => r.Name).ToArray());
      }

      [Fact]
      public void EditSavesInPlace_ByTheCertificatesId()
      {
         string dialog = Read("CertificateDialog.cs");
         string page = Read("SslCertificatesView.xaml.cs");
         string xaml = Read("SslCertificatesView.xaml");
         if (dialog == null || page == null || xaml == null)
            return;

         // Found by its id, never by its name, and saved: the id is what the ports hold.
         Assert.Contains("if ((int)cert.ID != id)", dialog, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"cert\.Name = name;\s*cert\.CertificateFile = certificate;\s*cert\.PrivateKeyFile = key;\s*cert\.Save\(\);"), dialog);
         Assert.DoesNotContain("certs.Add()", dialog, StringComparison.Ordinal);
         Assert.DoesNotContain(".Delete()", dialog, StringComparison.Ordinal);
         Assert.Contains("dialog.KeepRoomForFieldErrors(3);", dialog, StringComparison.Ordinal);

         // Edit... is the toolbar's verb before Delete, and Enter, F2 and a double-click open it.
         Assert.True(xaml.IndexOf("x:Name=\"EditButton\"", StringComparison.Ordinal) < xaml.IndexOf("x:Name=\"DeleteButton\"", StringComparison.Ordinal),
            "Edit... is not before Delete selected.");
         Assert.Contains("SelectionGate.Bind(CertGrid, EditButton, DeleteButton);", page, StringComparison.Ordinal);
         Assert.Contains("GridKeys.Wire(CertGrid, GridKeys.Pressing(EditButton),", page, StringComparison.Ordinal);
         Assert.Contains("CertificateDialog.Edit(Window.GetWindow(this), row.Id)", page, StringComparison.Ordinal);
      }

      [Fact]
      public void UsedBy_IsAColumn_AndTheFormSaysWhatTheServerReads()
      {
         string page = Read("SslCertificatesView.xaml.cs");
         string xaml = Read("SslCertificatesView.xaml");
         if (page == null || xaml == null)
            return;

         Assert.Contains("Header=\"{loc:L 'Used by'}\" Binding=\"{Binding UsedBy}\"", xaml, StringComparison.Ordinal);
         Assert.Contains("row.UsedBy = CertificateRows.UsedBy(", page, StringComparison.Ordinal);
         Assert.Contains("PemHint.Text = CertificateDialog.PfxHint;", page, StringComparison.Ordinal);

         Assert.Contains("x:Name=\"CertificateColumn\"", xaml, StringComparison.Ordinal);
         Assert.Contains("Sorting=\"CertGrid_Sorting\"", xaml, StringComparison.Ordinal);
         Assert.Contains("view.CustomSort = new CertificateOrder_(direction);", page, StringComparison.Ordinal);
      }

      [Fact]
      public void ThePfxHint_GivesTheCommandsAndNamesPem()
      {
         string dialog = Read("CertificateDialog.cs");
         if (dialog == null)
            return;

         Assert.Contains("openssl pkcs12 -in cert.pfx -nokeys -out cert.pem", dialog, StringComparison.Ordinal);
         Assert.Contains("openssl pkcs12 -in cert.pfx -nocerts -nodes -out key.pem", dialog, StringComparison.Ordinal);
      }

      private static string Read(string file)
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string path = Path.Join(directory.FullName, "ControlPanel", "Views", file);
            if (File.Exists(path))
               return File.ReadAllText(path);
         }

         return null;
      }
   }
}
