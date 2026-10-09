// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Where the sidebar scrolls after a jump to a page - from the palette, a
   /// Welcome tile, a Related link, a page's own button or a tour - so that the
   /// page's whole group is on screen and not only its row. The jump ended with
   /// the row's BringIntoView, which scrolls as little as it can: a row below the
   /// fold stopped on the sidebar's bottom edge with the rest of its group under
   /// it - Discovery at the foot of a 670-pixel sidebar with API &amp; monitoring
   /// and REST API keys hidden, eight rows of the spam group hidden under Spam
   /// exemptions, eleven under Anti-spam settings reached by search (review
   /// finding F151). The breadcrumb names the group; the sidebar now shows it.
   ///
   /// WPF-free arithmetic on the sidebar's content coordinates, so that
   /// SidebarScrollTests holds the rule; MainWindow measures the rows and scrolls.
   /// </summary>
   public static class SidebarScroll
   {
      /// <summary>The room left above the group's heading and below its last page, in pixels.</summary>
      public const double Margin = 8;

      /// <summary>
      /// The offset to scroll the sidebar to, or null to leave it where it is.
      /// <paramref name="offset"/> and <paramref name="viewport"/> are the
      /// sidebar's scroll offset and height; the tops and bottoms are the group's
      /// (its heading to its last page) and the page row's, in the coordinates of
      /// the sidebar's whole content.
      ///
      /// The whole group on screen already: nothing moves, so a jump within what
      /// can be seen does not jolt the sidebar. A group that fits: the least
      /// scroll that shows all of it, its heading kept on screen when the two
      /// pull apart. A group taller than the sidebar: nothing, when the row is
      /// already whole on screen, and otherwise the row a third of the way down,
      /// with the pages before it above it.
      /// </summary>
      public static double? OffsetToShow(double offset, double viewport, double groupTop, double groupBottom,
         double rowTop, double rowBottom)
      {
         if (viewport <= 0 || double.IsNaN(viewport) || double.IsNaN(offset))
            return null;

         double top = groupTop - Margin;
         double bottom = groupBottom + Margin;

         if (top >= offset && bottom <= offset + viewport)
            return null;

         if (bottom - top <= viewport)
         {
            // The least movement that shows the whole group: down until its last
            // page is on screen, or up until its heading is.
            double target = top < offset ? top : bottom - viewport;
            return Math.Max(0, target);
         }

         if (rowTop >= offset && rowBottom <= offset + viewport)
            return null;

         return Math.Max(0, rowTop - viewport / 3);
      }
   }
}
