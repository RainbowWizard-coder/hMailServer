// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The floor of a list that shows its rows whole in the page rather than scrolling
   /// inside it: the Domains page's list of domains in the one-column arrangement of
   /// a narrow window, where it was held at three rows and needed a scroll bar of its
   /// own for the fourth domain, inside the page's, over the rows' delete buttons
   /// (review finding F101). Free of WPF, so that the arithmetic is tested. Not
   /// FillPanel.ListRowsFloor, the fixed floor of a grid of three rows in its card.
   /// </summary>
   public static class WholeRowsFloor
   {
      /// <summary>The fewest rows a list keeps room for, as every list page's floor does.</summary>
      public const int FewestRows = 3;

      /// <summary>
      /// The most rows a list shows whole before it scrolls inside the page again, so
      /// that a server of hundreds of domains is not a page of hundreds of rows.
      /// </summary>
      public const int MostRows = 25;

      /// <summary>
      /// The pixels a ListView keeps above and below its rows, and one for layout
      /// rounding, which at 125, 150 or 175 % can take one off.
      /// </summary>
      public const double Chrome = 3;

      /// <summary>
      /// The height of a list of <paramref name="rows"/> rows, each
      /// <paramref name="rowPitch"/> from the top of one to the top of the next: room
      /// for <see cref="FewestRows"/> at least and <see cref="MostRows"/> at most, with
      /// <see cref="Chrome"/>.
      /// </summary>
      public static double For(int rows, double rowPitch)
         => Math.Clamp(rows, FewestRows, MostRows) * rowPitch + Chrome;

      /// <summary>
      /// A row's pitch, the top of one to the top of the next: its container's height
      /// and the margin the container keeps above and below it. The height alone left
      /// the margin out - the theme's list item keeps two pixels - and a floor built on
      /// it was a row's margin short for every row: four domains of 36.67 pixels were
      /// floored at 149.67 where they took 154.67, and the fourth still scrolled inside
      /// the list in the narrowest window.
      /// </summary>
      public static double Pitch(double height, double marginTop, double marginBottom)
         => height + marginTop + marginBottom;
   }
}
