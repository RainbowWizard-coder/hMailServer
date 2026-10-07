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
