// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The arithmetic of a page whose lists fill the window: a list takes the
   /// height the page's other parts leave, down to a floor, below which the page
   /// is taller than the window and scrolls. The other parts always get their own
   /// height. Plain numbers, so a test can hold the cases;
   /// <c>Views/Scaffold/FillPanel</c> lays a page out by them.
   ///
   /// The IP ranges page is why it exists (work item 79). In 6.3.4 its list sat
   /// in a grid's star row between rows sized to their content and the page did
   /// not scroll, so in a window shorter than the other parts - a maximised
   /// window on a 768-pixel screen, or 125 % scaling on a larger one - the star
   /// row was given nothing and the list was not there at all. 6.3.5 put the page
   /// in a scroll viewer over a fixed 560-pixel floor, which kept the list but
   /// knew nothing of the parts' real heights: in the narrowest window, where the
   /// permissions card wraps, the add form under it was cut off below the floor,
   /// where scrolling could not reach it. The domains and rules pages had the
   /// same fixed floor over two star rows, so several parts share the room there
   /// (<see cref="Share"/>).
   /// </summary>
   public static class PageFill
   {
      /// <summary>
      /// The height the filling part is given: what the viewport leaves after the
      /// other parts, never less than its floor. A viewport that is not known yet -
      /// zero, NaN or infinite, as on the first layout pass - gives the floor. It
      /// is <see cref="Share"/> with one part.
      /// </summary>
      public static double FillHeight(double viewport, double otherParts, double floor)
         => Share(viewport, otherParts, new[] { floor }, new[] { 1.0 })[0];

      /// <summary>
      /// The page's height: the other parts and the filling one. It is the viewport
      /// when the floor fits under the other parts, and more - so the page scrolls -
      /// when it does not; it is never less than the parts need.
      /// </summary>
      public static double PageHeight(double viewport, double otherParts, double floor)
         => Math.Max(Positive(viewport), Positive(otherParts) + Positive(floor));

      /// <summary>
      /// A filling part's floor once it has been measured at its share: its floor,
      /// or what it asks for where that is more than its share. WPF caps what a part
      /// asks for at what it is offered, but layout rounding can take it a device
      /// pixel past: at 150 % scaling a MinHeight of 157 is 236 device pixels, 157.33,
      /// and that is what the IP ranges page's list card asks for. Held to 157, the
      /// parts under it were drawn a device pixel higher than FillPanel's first
      /// version drew them, and the page came out that much shorter than its parts.
      /// </summary>
      public static double MeasuredFloor(double floor, double share, double asked)
         => Positive(asked) > Positive(share) ? Math.Max(Positive(floor), Positive(asked)) : Positive(floor);

      /// <summary>
      /// A floor on the device-pixel grid: <paramref name="value"/> raised to the
      /// next whole device pixel, <paramref name="pixel"/> being one in
      /// device-independent units (two thirds at 150 % scaling, four fifths at
      /// 125). A filling part's floor is handed out as its share, and WPF rounds
      /// what a part asks for to whole device pixels, so a floor between two of
      /// them is asked for a fraction of a pixel over, and the parts together
      /// pass a viewport they fit by that fraction, which layout rounding makes
      /// a whole pixel and the page's scroll viewer a bar. The Suppression list's
      /// bounce counts card, always at its floor in a 1280 by 800 window, has a
      /// MinHeight of 181 and a 12-pixel margin, 193, which at 150 % is 289.5
      /// device pixels: measured at 193 it asked for 290, 193.33, and the page
      /// had a scroll bar the height of the window in English and Japanese, and
      /// none in German, whose taller subtitle left a viewport whose fraction
      /// rounded the other way. A floor on the grid is asked for exactly, and the
      /// parts sum to the viewport. A value on the grid already stays there, the
      /// arithmetic's own error allowed for; no pixel size, or a nonsense one,
      /// leaves the value alone.
      /// </summary>
      public static double SnapUp(double value, double pixel)
         => pixel > 0 && !double.IsInfinity(pixel) && !double.IsNaN(value) && !double.IsInfinity(value)
            ? Math.Ceiling(value / pixel - 1e-9) * pixel
            : value;

      /// <summary>
      /// The heights of several filling parts - the domains page's accounts card
      /// and the row of alias and list cards under it, say - which share what the
      /// viewport leaves after the other parts in proportion to their weights, as a
      /// grid's star rows share theirs, except that no part is given less than its
      /// floor: a part whose share would be less has its floor, and the others
      /// share what is left. When the floors together are more than the viewport
      /// leaves, every part has its floor and the page is taller than the window.
      /// A part with no weight has its floor. The heights add up to what the
      /// viewport leaves, or to the floors where they are more.
      /// </summary>
      public static double[] Share(double viewport, double otherParts, IReadOnlyList<double> floors, IReadOnlyList<double> weights)
      {
         ArgumentNullException.ThrowIfNull(floors);
         ArgumentNullException.ThrowIfNull(weights);
         if (floors.Count != weights.Count)
            throw new ArgumentException("Every part needs a floor and a weight.", nameof(weights)); // no-loc: a programming error

         int count = floors.Count;
         var heights = new double[count];
         var atFloor = new bool[count];
         double room = Math.Max(0, Positive(viewport) - Positive(otherParts));

         // A part held at its floor stays there: what it takes beyond its share
         // only leaves the others less, so at most one round per part.
         bool settled = false;
         while (!settled)
         {
            double left = room;
            double weight = 0;
            for (int i = 0; i < count; i++)
            {
               if (atFloor[i])
                  left -= Positive(floors[i]);
               else
                  weight += Positive(weights[i]);
            }

            settled = true;
            for (int i = 0; i < count; i++)
            {
               if (atFloor[i])
                  continue;

               double share = weight > 0 ? Math.Max(0, left) * Positive(weights[i]) / weight : 0;
               if (share < Positive(floors[i]))
               {
                  atFloor[i] = true;
                  settled = false;
               }
               else
               {
                  heights[i] = share;
               }
            }
         }

         for (int i = 0; i < count; i++)
         {
            if (atFloor[i])
               heights[i] = Positive(floors[i]);
         }

         return heights;
      }

      private static double Positive(double value)
         => double.IsNaN(value) || double.IsInfinity(value) || value < 0 ? 0 : value;
   }
}
