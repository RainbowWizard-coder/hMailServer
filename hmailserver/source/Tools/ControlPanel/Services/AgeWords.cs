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
            return Plural(minutes, "{0} minute ago", "{0} minutes ago");
         }

         if (age.TotalDays < 2)
         {
            int hours = (int)age.TotalHours;
            return Plural(hours, "{0} hour ago", "{0} hours ago");
         }

         return F("{0} days ago", (int)age.TotalDays);
      }

      /// <summary>
      /// The same age in a grid's cell, in each language's short units - just now,
      /// minutes, hours up to two days, then days - where the whole phrase is the cell's
      /// tool tip: "less than a minute ago" took the Quarantine page's Held column to
      /// 140 pixels and "vor weniger als einer Minute" to 190, and the addresses and the
      /// subject beside it were cut (review finding F261).
      /// </summary>
      public static string Short(TimeSpan age)
      {
         if (age < TimeSpan.Zero)
            age = TimeSpan.Zero;

         if (age.TotalMinutes < 1)
            return L("just now");

         if (age.TotalHours < 1)
            return F("{0} min ago", (int)age.TotalMinutes);

         if (age.TotalDays < 2)
            return F("{0} h ago", (int)age.TotalHours);

         return F("{0} d ago", (int)age.TotalDays);
      }
   }
}
