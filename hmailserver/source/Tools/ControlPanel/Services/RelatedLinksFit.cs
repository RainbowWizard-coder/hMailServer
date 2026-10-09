// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// How many of a page's Related links are drawn beside the breadcrumb: as many
   /// as fit, in the order the navigation map lists them, with a drop-down for the
   /// rest. The bar drew the first three whatever the room, so the pairs the map
   /// lists on purpose further down were never drawn - the queue's stalled mail,
   /// LDAP's directory synchronisation, the greylisting exemptions' bypass list,
   /// SSL/TLS's Auto-ban, the block lists' IP ranges - and twelve pages were the
   /// drawn link of none (review finding F271). WPF-free, so that
   /// RelatedLinksFitTests holds the rule; MainWindow.Related.cs measures.
   /// </summary>
   public static class RelatedLinksFit
   {
      /// <summary>
      /// The number of links, from the first, that are drawn: all of them when
      /// their <paramref name="widths"/> fit in <paramref name="available"/>, and
      /// otherwise as many as fit beside the drop-down of the rest, whose width is
      /// <paramref name="moreWidth"/> - none, when not even the first does.
      /// </summary>
      public static int Count(IReadOnlyList<double> widths, double available, double moreWidth)
      {
         if (widths == null || widths.Count == 0)
            return 0;

         double total = 0;
         foreach (double width in widths)
            total += width;
         if (total <= available)
            return widths.Count;

         int count = 0;
         double used = 0;
         while (count < widths.Count && used + widths[count] + moreWidth <= available)
         {
            used += widths[count];
            count++;
         }

         return count;
      }
   }
}
