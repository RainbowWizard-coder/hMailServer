// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// A time of day typed as the 24-hour clock writes it - 18:30, 7:05, 23:59:59 -
   /// for a box beside a date picker: the IP range dialog's expiry, which was one
   /// free-text box of "YYYY-MM-DD HH:MM:SS" that showed a stored time while
   /// "This range expires" was off (review finding F197). Held by DependentFieldsTests.
   /// </summary>
   public static class TimeOfDayText
   {
      // TimeSpan's own format letters: h and hh are the hours of a day, 0 to 23.
      private static readonly string[] Forms = { "h\\:mm", "hh\\:mm", "h\\:mm\\:ss", "hh\\:mm\\:ss" };

      /// <summary>
      /// The time of day the text is; an empty box is midnight, the start of the day
      /// the date picker names. False for anything that is not a time of one day.
      /// </summary>
      public static bool TryParse(string text, out TimeSpan time)
      {
         string trimmed = (text ?? "").Trim();
         if (trimmed.Length == 0)
         {
            time = TimeSpan.Zero;
            return true;
         }

         foreach (string form in Forms)
         {
            if (TimeSpan.TryParseExact(trimmed, form, CultureInfo.InvariantCulture, out time) && time < TimeSpan.FromDays(1))
               return true;
         }

         time = TimeSpan.Zero;
         return false;
      }

      /// <summary>The time as the box shows it: HH:MM, and the seconds only where there are some.</summary>
      public static string Format(DateTime when)
         => when.ToString(when.Second == 0 ? "HH:mm" : "HH:mm:ss", CultureInfo.InvariantCulture);
   }
}
