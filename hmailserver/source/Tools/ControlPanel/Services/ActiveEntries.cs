// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The pill beside the title of a list whose entries can each be switched off:
   /// how many are on, of how many there are (review finding F268). The SURBL
   /// servers and DNS blacklists pages counted every entry - "3 blacklists" - where
   /// the server asks only the active ones, and the spam overview said "2 of 3
   /// active" for the same list. A label and two numbers, which no language has to
   /// decline: "Active: 2 of 3".
   /// </summary>
   public static class ActiveEntries
   {
      /// <summary>
      /// How many of the rows have the list's switch on - its value the Boolean
      /// true - or -1 where there is no switch to count or no row, whose pill
      /// stays the list's own count beside the empty state. A value that is
      /// false, null, missing or not a Boolean counts as off.
      /// </summary>
      public static int Count(IEnumerable<IReadOnlyDictionary<string, object>> rowValues, string activeProp)
      {
         if (activeProp == null || rowValues == null)
            return -1;

         int rows = 0, active = 0;
         foreach (IReadOnlyDictionary<string, object> values in rowValues)
         {
            rows++;
            if (values != null && values.TryGetValue(activeProp, out object on) && on is true)
               active++;
         }
         return rows == 0 ? -1 : active;
      }

      /// <summary>"Active: 2 of 3".</summary>
      public static string PillText(int active, int total) => F("Active: {0} of {1}", active, total);

      /// <summary>
      /// Warning when there are entries and none of them is on, so that a list
      /// that checks nothing does not read as one that checks; Normal otherwise,
      /// and for a count that was not taken (a negative one).
      /// </summary>
      public static StatusLevel PillLevel(int active, int total) =>
         active == 0 && total > 0 ? StatusLevel.Warning : StatusLevel.Normal;
   }
}
