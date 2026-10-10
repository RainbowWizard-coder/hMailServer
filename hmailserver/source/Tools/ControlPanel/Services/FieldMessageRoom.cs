// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The arithmetic of the room a dialog keeps under its body for its fields'
   /// messages (FluentDialogWindow.KeepRoomForFieldErrors, review finding F213):
   /// the body's floor is its height without the messages shown in it and the
   /// room more, no higher than the window can still grow, and it is taken again
   /// whenever the body's height changes, so that a message grows into the room
   /// while a row shown or collapsed grows or shrinks the dialog as it always did.
   /// A floor measured once, when the dialog loaded, kept the access-control entry
   /// as tall as with its account box after Anyone collapsed the box. Without WPF,
   /// so the tests can hold it.
   /// </summary>
   public static class FieldMessageRoom
   {
      /// <summary>
      /// The body's floor and what it adds to the body's height without its
      /// messages: <paramref name="bodyHeight"/> is the body's height as laid out,
      /// <paramref name="messages"/> the height the messages shown in it take
      /// (<see cref="Covered"/>), <paramref name="room"/> the room asked for,
      /// <paramref name="maxWindowHeight"/> the window's cap and
      /// <paramref name="chrome"/> the window's height outside the body - the
      /// title bar, the heading, the footer. No room while
      /// <paramref name="refusableShown"/> is false: the rows that can be refused
      /// are collapsed, and nothing can be said under them. A floor of 0 is none.
      /// </summary>
      public static (double Floor, double Grows) Take(double bodyHeight, double messages, double room, double maxWindowHeight, double chrome, bool refusableShown)
      {
         double content = Math.Max(0, bodyHeight - messages);

         // Only what the window can still grow: at the cap the body scrolls, and a
         // floor past it would only be blank to scroll through.
         double grows = refusableShown ? Math.Max(0, Math.Min(room, maxWindowHeight - chrome - content)) : 0;
         return grows > 0 ? (content + grows, grows) : (0, 0);
      }

      /// <summary>
      /// The height that spans from top to bottom cover together: messages one
      /// under another take the sum of theirs, and messages beside each other on
      /// one line take the tallest of them once.
      /// </summary>
      public static double Covered(IEnumerable<(double Top, double Bottom)> spans)
      {
         double covered = 0;
         double top = 0, bottom = double.NegativeInfinity;
         foreach ((double Top, double Bottom) span in spans.Where(s => s.Bottom > s.Top).OrderBy(s => s.Top))
         {
            if (span.Top <= bottom)
            {
               bottom = Math.Max(bottom, span.Bottom);
               continue;
            }

            if (bottom > top)
               covered += bottom - top;
            top = span.Top;
            bottom = span.Bottom;
         }

         if (bottom > top)
            covered += bottom - top;
         return covered;
      }
   }
}
