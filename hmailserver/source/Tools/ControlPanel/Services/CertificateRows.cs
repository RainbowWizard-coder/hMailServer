// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What the SSL certificates page's rows say beyond the two files (review finding
   /// F235), with no WPF so that CertificateRowsTests holds it: which TCP/IP
   /// ports present each certificate, and the order the Certificate column sorts in.
   ///
   /// The page's purpose was "which port uses which", and no column said it; the
   /// Certificate column sorted by its status word, so that "Expired" came before
   /// "Expires soon" and both among the good.
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
   }
}
