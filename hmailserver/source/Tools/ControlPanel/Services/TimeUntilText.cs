// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// How long until a time the server keeps - a ban's end on the Auto-ban page, a
   /// queued message's next try - in the catalogued units Server status says its
   /// uptime in ("1 h 5 min"), with no WPF so that TimeUntilTextTests holds it.
   ///
   /// The server's times are its own clock's (a VT_DATE or a "yyyy-MM-dd HH:mm:ss"
   /// in its local time), and the server gives no clock of its own over COM, so a
   /// wait is worked out only where the two clocks are one - a session on this
   /// computer - as ServerUptimeText works out the uptime; elsewhere the time itself
   /// is shown, in the reader's short date and time, marked as the server's.
   /// </summary>
   public static class TimeUntilText
   {
      /// <summary>
      /// A wait in catalogued units: "less than a minute", "12 min", "1 h 5 min",
      /// "2 d 3 h". A wait that has passed is less than a minute.
      /// </summary>
      public static string Span(TimeSpan wait)
      {
         if (wait < TimeSpan.FromMinutes(1))
            return L("less than a minute");

         if (wait.TotalDays >= 1)
            return F("{0} d {1} h", (int)wait.TotalDays, wait.Hours);
         if (wait.TotalHours >= 1)
            return F("{0} h {1} min", wait.Hours, wait.Minutes);
         return F("{0} min", (int)wait.TotalMinutes);
      }

      /// <summary>The server's time in the reader's short date and time, marked as the server's.</summary>
      public static string ServerTime(DateTime when)
         => F("{0} (server time)", when.ToString("g", CultureInfo.CurrentCulture));

      /// <summary>
      /// A time the server gives as text, yyyy-MM-dd HH:mm:ss (or the date alone), or
      /// null when it is not one, or is the server's "never" of 1901 and before.
      /// </summary>
      public static DateTime? Parse(string serverTime)
      {
         string text = (serverTime ?? "").Trim();
         foreach (string format in new[] { "yyyy-MM-dd HH:mm:ss", "yyyy-MM-dd HH:mm:ss.fff", "yyyy-MM-dd" }) // no-loc: the server's formats
         {
            if (DateTime.TryParseExact(text, format, CultureInfo.InvariantCulture, DateTimeStyles.None, out DateTime when))
               return when.Year < 1970 ? null : when;
         }
         return null;
      }
   }
}
