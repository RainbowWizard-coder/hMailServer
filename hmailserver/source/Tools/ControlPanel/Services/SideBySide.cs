// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Whether parts laid side by side - a page's columns, two cards in a row -
   /// each keep their minimum width when they share a width by weight, as a
   /// grid's star columns share theirs. Where they would not, a page puts them
   /// one above the other rather than squeeze them until what they hold runs off
   /// their right edges, which is what the narrowest window (760 by 520) did to
   /// the domains page's Create account button and the rules page's match list.
   /// Plain numbers, so a test can hold the cases.
   /// </summary>
   public static class SideBySide
   {
      /// <summary>
      /// True when every part's share of <paramref name="width"/> - its weight over
      /// the weights together - is at least its minimum width. A width that is not
      /// known yet (zero, NaN or infinite) fits nothing.
      /// </summary>
      public static bool Fits(double width, IReadOnlyList<double> weights, IReadOnlyList<double> minWidths)
      {
         ArgumentNullException.ThrowIfNull(weights);
         ArgumentNullException.ThrowIfNull(minWidths);
         if (weights.Count != minWidths.Count)
            throw new ArgumentException("Every part needs a weight and a minimum width.", nameof(minWidths)); // no-loc: a programming error

         if (double.IsNaN(width) || double.IsInfinity(width) || width <= 0)
            return false;

         double total = 0;
         foreach (double weight in weights)
            total += Math.Max(0, weight);
         if (total <= 0)
            return false;

         for (int i = 0; i < weights.Count; i++)
         {
            if (width * Math.Max(0, weights[i]) / total < minWidths[i])
               return false;
         }

         return true;
      }
   }
}
