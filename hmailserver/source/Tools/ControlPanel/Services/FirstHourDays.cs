// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The days the server's setup checklist gives in its own form (SetupChecklist.cpp's
   /// UtcDate, yyyy-MM-dd), said in the reader's short date: the day a setting profile
   /// was applied, kept, put back, declined or failed, and the day the stored-secrets key
   /// was exported. The checklist said "The Small office profile was applied on
   /// 2026-10-01." in every language (the owner's decision 13: the reader's culture for
   /// dates, ISO only in text that describes a format). Out of ExternalSetupChecks, which
   /// reads the server, so that FirstHourDaysTests holds it.
   /// </summary>
   public static class FirstHourDays
   {
      /// <summary>The argument of each first-hour text that is such a day.</summary>
      private static readonly Dictionary<string, int> DayArguments = new(StringComparer.Ordinal)
      {
         ["profile_applied"] = 1,
         ["profile_kept"] = 1,
         ["profile_reverted"] = 1,
         ["profile_failed"] = 1,
         ["profile_declined"] = 0,
         ["stored_secrets_key_exported"] = 1,
      };

      /// <summary>A finding's arguments as the page says them: its day in the reader's short date, every other argument as it came.</summary>
      public static List<string> ReaderArguments(string code, IEnumerable<string> args)
      {
         var shown = new List<string>(args ?? Array.Empty<string>());
         if (DayArguments.TryGetValue(code ?? "", out int at) && at < shown.Count && ServerTimeText.Parse(shown[at]) is DateTime day)
            shown[at] = ServerTimeText.Date(day);
         return shown;
      }
   }
}
