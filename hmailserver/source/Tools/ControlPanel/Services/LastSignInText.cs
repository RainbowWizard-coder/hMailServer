// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The account dialog's Last sign-in, without the window, so that LastSignInTextTests
   /// can hold it. Review finding F292: the row showed Account.LastLogonTime as
   /// Convert.ToString gave it - the date in whatever form the machine's culture
   /// wrote, with no word that it is the server's clock - and a date the server keeps
   /// for no sign-in (an OLE date of 0, 30 December 1899) as that date.
   ///
   /// The server's time is shown in the reader's short date and time with "(server
   /// time)", as the Server status page's start is (ServerUptimeText.Started; the
   /// owner's decision on dates).
   /// </summary>
   public static class LastSignInText
   {
      // An OLE date of 0 is the server's "no date"; nobody signed in before 1970.
      private static readonly DateTime Earliest = new(1970, 1, 1);

      /// <summary>
      /// The last sign-in as Account.LastLogonTime gives it over COM (a date) or as the
      /// server writes it (yyyy-MM-dd HH:mm:ss): in the reader's short date and time,
      /// marked as the server's time, or Never where there is none.
      /// </summary>
      public static string Format(object lastLogonTime)
      {
         DateTime? when = lastLogonTime switch
         {
            DateTime date => date,
            string text when DateTime.TryParseExact(text.Trim(), "yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture, DateTimeStyles.None, out DateTime parsed) => parsed,
            _ => null,
         };

         if (when is DateTime signedIn && signedIn >= Earliest)
            return F("{0} (server time)", signedIn.ToString("g", CultureInfo.CurrentCulture));

         // A text the server wrote in some other form is its own words, shown as they are.
         if (when is null && lastLogonTime is string other && !string.IsNullOrWhiteSpace(other))
            return other.Trim();

         return L("Never");
      }
   }
}
