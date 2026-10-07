// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The arithmetic of a page with one part that fills the window: a list that
   /// takes the height the page's other parts leave, down to a floor, below which
   /// the page is taller than the window and scrolls. The other parts always get
   /// their own height. Plain numbers, so a test can hold the cases;
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
   /// where scrolling could not reach it.
   /// </summary>
   public static class PageFill
   {
      /// <summary>
      /// The height the filling part is given: what the viewport leaves after the
      /// other parts, never less than its floor. A viewport that is not known yet -
      /// zero, NaN or infinite, as on the first layout pass - gives the floor.
      /// </summary>
      public static double FillHeight(double viewport, double otherParts, double floor)
         => Math.Max(Positive(floor), Positive(viewport) - Positive(otherParts));

      /// <summary>
      /// The page's height: the other parts and the filling one. It is the viewport
      /// when the floor fits under the other parts, and more - so the page scrolls -
      /// when it does not; it is never less than the parts need.
      /// </summary>
      public static double PageHeight(double viewport, double otherParts, double floor)
         => Math.Max(Positive(viewport), Positive(otherParts) + Positive(floor));

      private static double Positive(double value)
         => double.IsNaN(value) || double.IsInfinity(value) || value < 0 ? 0 : value;
   }
}
