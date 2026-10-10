// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The Server status page's start time and uptime, without the window, so that
   /// ServerUptimeTextTests can hold them. Review finding F247: the uptime was the
   /// server's local start time (Status.StartTime, Time::GetCurrentDateTime on the
   /// server) subtracted from the Control Panel's own clock, wrong by the difference
   /// whenever the two are different computers in different time zones, and its
   /// units were "d h m" in every language ("5d 3h 12m" in Japanese).
   ///
   /// The server gives no clock of its own over COM, so the uptime is worked out only
   /// where the two clocks are one - a session on this computer - and the start is
   /// shown in the reader's date format as the server's time, which it is.
   /// </summary>
   public static class ServerUptimeText
   {
      /// <summary>The start time as the server writes it, yyyy-MM-dd HH:mm:ss, or null.</summary>
      public static DateTime? ParseStart(string startTime)
         => DateTime.TryParseExact((startTime ?? "").Trim(), "yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture, DateTimeStyles.None, out DateTime started)
            ? started
            : null;

      /// <summary>The start, in the reader's short date and time, marked as the server's time.</summary>
      public static string Started(string startTime)
      {
         if (ParseStart(startTime) is DateTime started)
            return ServerTimeText.Marked(started);

         return string.IsNullOrWhiteSpace(startTime) ? "-" : startTime;
      }

      /// <summary>
      /// The heading over the counts kept since the service started: with the start,
      /// marked as the server's time as the Started row is, where the server gave one
      /// (the owner's decision 13); the words alone where it did not. The heading
      /// showed the start in the reader's format with no marker, under a Started row
      /// that had one.
      /// </summary>
      public static string SinceStart(string startTime)
         => ParseStart(startTime) is null ? L("Since the service started") : F("Since the service started at {0}", Started(startTime));

      /// <summary>
      /// How long the service has been up, in catalogued units, when the server's
      /// clock is this computer's (<paramref name="sameClock"/>); otherwise that it is
      /// not known here. <paramref name="compact"/> is the Dashboard's tile: days and
      /// hours alone past a day, and a dash where the uptime is not known here.
      /// </summary>
      public static string Uptime(string startTime, DateTime now, bool sameClock, bool compact = false)
      {
         if (ParseStart(startTime) is not DateTime started)
            return "-";

         if (!sameClock)
            return compact ? "-" : L("Not known here: the server is on another computer");

         TimeSpan up = now - started;
         if (up < TimeSpan.Zero)
            up = TimeSpan.Zero;

         if (up.TotalDays >= 1)
            return compact
               ? F("{0} d {1} h", (int)up.TotalDays, up.Hours)
               : F("{0} d {1} h {2} min", (int)up.TotalDays, up.Hours, up.Minutes);
         if (up.TotalHours >= 1)
            return F("{0} h {1} min", up.Hours, up.Minutes);
         return F("{0} min", (int)up.TotalMinutes);
      }
   }
}
