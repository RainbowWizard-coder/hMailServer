// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What the TLS certificates page's rows say beyond the two files (review findings
   /// F235 and F236), with no WPF so that CertificatesEditTests holds it: which TCP/IP
   /// ports present each certificate, the order the Certificate column sorts in, the
   /// file name a path column shows, and whether the passphrase editor is offered.
   ///
   /// The page's purpose was "which port uses which", and no column said it; the
   /// Certificate column sorted by its status word, so that "Expired" came before
   /// "Expires soon" and both among the good; the path columns took the width the
   /// name needed for a folder that is the same on every row; and the passphrase
   /// editor was offered for a key that needs none.
   /// </summary>
   public static class CertificateRows
   {
      /// <summary>
      /// The ports that present a certificate, "SMTP 465, IMAP 993"; "Not used" when
      /// none does, and "Not known" when the ports could not be read (null).
      /// </summary>
      public static string UsedBy(IReadOnlyList<string> ports)
      {
         if (ports == null)
            return L("Not known");

         return ports.Count == 0 ? L("Not used") : string.Join(", ", ports);
      }

      /// <summary>
      /// The selected certificate's Used by line in the details card - its level, the
      /// words the column shows and the sentence after them - so that the whole list of
      /// ports is read at any width and from the keyboard: at 1280 by 800 the column
      /// cut a certificate's three ports to "SMTP 587, SMTP 4...", and its tool tip is the
      /// mouse's alone (the skeptic's review of F235).
      /// </summary>
      public static (StatusLevel Level, string Word, string Detail) UsedByLine(IReadOnlyList<string> ports)
      {
         if (ports == null)
            return (StatusLevel.Information, UsedBy(null), L("The TCP/IP ports could not be read, so which of them present this certificate is not known."));

         if (ports.Count == 0)
            return (StatusLevel.Normal, UsedBy(ports), Sentences(L("No TCP/IP port presents this certificate."), L("Choose it for a port on the TCP/IP ports page to use it.")));

         return (StatusLevel.Normal, UsedBy(ports), L("These TCP/IP ports present this certificate to the clients that connect to them."));
      }

      /// <summary>
      /// The order the Certificate column sorts in, ascending: the one asking for the
      /// most attention first - Critical, then Warning, and so on down to the good -
      /// and, of two at one level, the one that expires sooner; one whose expiry is
      /// not known after those that have one.
      /// </summary>
      public static int Compare(StatusLevel a, int? daysA, StatusLevel b, int? daysB)
      {
         int level = b.CompareTo(a);
         if (level != 0)
            return level;

         if (daysA == daysB)
            return 0;
         if (daysA == null)
            return 1;
         if (daysB == null)
            return -1;
         return daysA.Value.CompareTo(daysB.Value);
      }

      /// <summary>The file's name alone, which the path columns show; the whole path is the cell's tool tip.</summary>
      public static string FileName(string path)
      {
         string trimmed = (path ?? "").Trim();
         int slash = Math.Max(trimmed.LastIndexOf('\\'), trimmed.LastIndexOf('/'));
         return slash >= 0 && slash < trimmed.Length - 1 ? trimmed.Substring(slash + 1) : trimmed;
      }

      /// <summary>
      /// Whether the details card offers the passphrase editor as it opens: not for a
      /// key this computer read as unencrypted with no passphrase stored, which needs
      /// none - there it waits behind Set a passphrase anyway... - and for every other
      /// key, one that is encrypted, one that cannot be read from here, or one that has
      /// a passphrase stored, which the editor removes.
      /// </summary>
      public static bool OffersPassphrase(bool passphraseStored, bool passphraseUnknown, bool? keyIsEncrypted)
         => passphraseStored || passphraseUnknown || keyIsEncrypted != false;
   }
}
