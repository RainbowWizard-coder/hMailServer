// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// How long ago something was, in the catalogued words the backup page says the
   /// age of its last run in: less than a minute, minutes, hours up to two days,
   /// then days. No WPF, so the Quarantine page's held times and the message-store
   /// consistency card's scan share it with the tests.
   /// </summary>
   public static class AgeWords
   {
      public static string Ago(TimeSpan age)
      {
         if (age < TimeSpan.Zero)
            age = TimeSpan.Zero;

         if (age.TotalMinutes < 1)
            return L("less than a minute ago");

         if (age.TotalHours < 1)
         {
            int minutes = (int)age.TotalMinutes;
            return minutes == 1 ? L("1 minute ago") : F("{0} minutes ago", minutes);
         }

         if (age.TotalDays < 2)
         {
            int hours = (int)age.TotalHours;
            return hours == 1 ? L("1 hour ago") : F("{0} hours ago", hours);
         }

         return F("{0} days ago", (int)age.TotalDays);
      }
   }
}
