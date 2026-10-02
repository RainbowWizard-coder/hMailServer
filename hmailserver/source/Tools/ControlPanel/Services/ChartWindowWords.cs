// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// How much time a chart covers, in words, for its X axis title (AccessibleChartCard):
   /// seconds under a minute and a half, minutes under an hour and a half, hours beyond.
   ///
   /// One of a unit is a sentence of its own - "last second" - and not "last 1
   /// seconds", which is what a number put into the plural said, in English and in the
   /// languages whose plural the catalogue writes out (Danish, German, Swedish, ...).
   /// The catalogue has no plural forms, so each count that needs another word is a
   /// whole sentence; the languages that write the unit short (Czech, Finnish, Polish,
   /// Russian, Ukrainian: "s", "min", "h") and those without a plural (Japanese,
   /// Chinese, Turkish's "saniye") need nothing more. By the rounding below a minute
   /// and an hour are never one, so only a second can be; the three are written the
   /// same way all the same.
   /// </summary>
   public static class ChartWindowWords
   {
      public static string Of(TimeSpan span)
      {
         if (span.TotalSeconds < 90)
         {
            int seconds = Math.Max(1, (int)Math.Round(span.TotalSeconds));
            return seconds == 1 ? L("last second") : F("last {0} seconds", seconds);
         }

         if (span.TotalMinutes < 90)
         {
            int minutes = Math.Max(1, (int)Math.Round(span.TotalMinutes));
            return minutes == 1 ? L("last minute") : F("last {0} minutes", minutes);
         }

         int hours = Math.Max(1, (int)Math.Round(span.TotalHours));
         return hours == 1 ? L("last hour") : F("last {0} hours", hours);
      }
   }
}
