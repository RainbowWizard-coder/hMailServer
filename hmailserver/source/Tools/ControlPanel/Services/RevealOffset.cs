// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Where a setting found by search is scrolled to as its page shows it
   /// (Views/SettingReveal). The row was brought into view with BringIntoView, which
   /// scrolls as little as it can: a row below the fold - the usual case for a search
   /// hit - ended at the viewport's lower edge, the ring drawn round it outside its
   /// edge was cut there, and what follows it stayed out of sight. The row is put a
   /// third of the way down the viewport instead, with room above it for the ring and
   /// below it for the ring and the start of what follows; a row already in view with
   /// that room is left where it is. The arithmetic, apart from WPF, so that it is
   /// tested.
   /// </summary>
   public static class RevealOffset
   {
      /// <summary>The room kept above the row: the ring, drawn four pixels outside it, and a little air.</summary>
      public const double Above = 12;

      /// <summary>The room kept below the row: the ring, and the start of what follows it - the next row's caption.</summary>
      public const double Below = 60;

      /// <summary>The least room the ring itself needs at either edge: four pixels outside the row and its two-pixel pen.</summary>
      public const double Ring = 6;

      /// <summary>
      /// The vertical offset to scroll to, or null to leave the offset as it is: null
      /// when the row and its room are in view already; otherwise the offset that puts
      /// the row's top a third of the way down the viewport - higher where the row and
      /// the room below it would not fit under that, but not nearer the top than
      /// <see cref="Above"/> - within what the viewer can scroll. A row with no such room
      /// in the viewport, which a short window's tab can be, is centred in it, its top no
      /// nearer the edge than the ring needs (<see cref="Ring"/>).
      /// </summary>
      /// <param name="offset">The viewer's vertical offset now.</param>
      /// <param name="viewport">The height of its viewport.</param>
      /// <param name="scrollable">How far it can scroll: its extent less its viewport.</param>
      /// <param name="top">The row's top, measured from the viewport's top.</param>
      /// <param name="height">The row's height.</param>
      public static double? For(double offset, double viewport, double scrollable, double top, double height)
      {
         if (viewport <= 0 || double.IsNaN(top) || double.IsNaN(height))
            return null;

         if (top - Above >= 0 && top + height + Below <= viewport)
            return null;

         double at = Math.Min(viewport / 3, viewport - height - Below);
         if (at < Above)
            at = Math.Max(Ring, (viewport - height) / 2);
         double target = Math.Clamp(offset + top - at, 0, Math.Max(0, scrollable));
         return Math.Abs(target - offset) < 0.5 ? null : target;
      }
   }
}
