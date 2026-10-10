// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Globalization;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The words the Message trace page says an event's SMTP status in, and how long
   /// its events are kept, without the window (Views/MessageTraceView draws them), so
   /// that MessageTraceStatusTests holds them. Review finding F253: the Status column
   /// was the bare reply code - 250, 451, 550 - right-aligned like a count, with
   /// nothing to say that one is a success, the next a deferral and the last a
   /// refusal.
   /// </summary>
   public static class MessageTraceStatus
   {
      /// <summary>The level a reply code is shown at: a success Good, a deferral a Warning, a refusal Critical; anything else, and no code, Normal.</summary>
      public static StatusLevel LevelOf(int code)
         => code is >= 200 and < 300 ? StatusLevel.Good
            : code is >= 400 and < 500 ? StatusLevel.Warning
            : code is >= 500 and < 600 ? StatusLevel.Critical
            : StatusLevel.Normal;

      /// <summary>
      /// The reply code with the word for its class - "OK 250", "Deferred 451",
      /// "Refused 550" - beside the mark of its level (StatusPill); a code of no class
      /// as the number alone, and an event with no code (0) as nothing.
      /// </summary>
      public static string TextOf(int code)
         => code is >= 200 and < 300 ? F("OK {0}", code)
            : code is >= 400 and < 500 ? F("Deferred {0}", code)
            : code is >= 500 and < 600 ? F("Refused {0}", code)
            : code == 0 ? ""
            : code.ToString(CultureInfo.CurrentCulture);

      /// <summary>How long the trace keeps its events, by MessageTraceRetentionDays: 0 keeps every event.</summary>
      public static string KeptFor(int days)
         => days <= 0
            ? L("Events are kept with no time limit.")
            : Plural(days, "Events are kept {0} day.", "Events are kept {0} days.");
   }
}
