// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What the Delivery queue page says of a queued message and of what its buttons did
   /// to the rows chosen (review finding F248), with no WPF so that DeliveryQueueTests
   /// holds it. The page promised "why each is waiting" and had no column that said it -
   /// the server's locked flag, a message being delivered now, was read and dropped -
   /// and Deliver now and Remove acted on the first of the rows chosen alone.
   /// </summary>
   public static class QueueWords
   {
      /// <summary>
      /// Where a queued message stands, from the server's locked flag and its next
      /// try: being delivered now; due, its next try passed or never set (the server's
      /// 1901); the wait to its next try where the server's clock is this computer's;
      /// and, where it may not be, the next try itself, in the reader's date and time
      /// and marked as the server's - the State column holds what the Next try column
      /// did, which a grid of eight columns cut to a word at 1280 pixels in German. A
      /// time in none of the server's formats is shown as the server wrote it.
      /// </summary>
      public static string State(bool locked, string nextTry, DateTime now, bool sameClock)
      {
         if (locked)
            return L("Delivering now");

         if (TimeUntilText.IsNever(nextTry))
            return L("Due for delivery");

         DateTime? at = TimeUntilText.Parse(nextTry);
         if (at == null)
            return F("Next try at {0}", F("{0} (server time)", nextTry.Trim()));

         if (!sameClock)
            return F("Next try at {0}", TimeUntilText.ServerTime(at.Value));

         TimeSpan wait = at.Value - now;
         return wait < TimeSpan.FromMinutes(1)
            ? L("Due for delivery")
            : F("Next try in {0}", TimeUntilText.Span(wait));
      }

      /// <summary>
      /// The State cell's tool tip: the next try in the reader's date and time, marked
      /// as the server's, where the cell says the wait to it; null where there is no
      /// time to give - a message being delivered, or one never tried.
      /// </summary>
      public static string StateTip(bool locked, string nextTry)
      {
         DateTime? at = TimeUntilText.Parse(nextTry);
         return locked || at == null ? null : F("Next try at {0}", TimeUntilText.ServerTime(at.Value));
      }

      /// <summary>
      /// The order the State column sorts in, ascending: the messages being delivered,
      /// then those due, then by their next try, soonest first; a time in none of the
      /// server's formats last.
      /// </summary>
      public static DateTime Order(bool locked, string nextTry)
      {
         if (locked)
            return DateTime.MinValue;
         if (TimeUntilText.IsNever(nextTry))
            return DateTime.MinValue.AddTicks(1);
         return TimeUntilText.Parse(nextTry) ?? DateTime.MaxValue;
      }

      /// <summary>What Deliver now did to the messages chosen: the one by its id, more by their number.</summary>
      public static string Retried(IReadOnlyList<string> ids)
      {
         if (ids.Count == 1)
            return F("Delivery retriggered for message {0}.", ids[0]);

         return Plural(ids.Count, "Delivery retriggered for {0} message.", "Delivery retriggered for {0} messages.");
      }

      /// <summary>What Remove did to the messages chosen: the one by its id, more by their number.</summary>
      public static string Removed(IReadOnlyList<string> ids)
      {
         if (ids.Count == 1)
            return F("Message {0} removed.", ids[0]);

         return Plural(ids.Count, "{0} message removed.", "{0} messages removed.");
      }

      /// <summary>
      /// What Remove says when the server refused one of the messages chosen: those
      /// removed before it, then the one it refused and why - the rest stay in the
      /// list read again, which the page shows.
      /// </summary>
      public static string RemoveRefused(IReadOnlyList<string> removed, string refusedId, string reason)
      {
         string refused = F("Could not remove message {0}: {1}", refusedId, reason);
         return removed.Count == 0 ? refused : Sentences(Removed(removed), refused);
      }
   }
}
