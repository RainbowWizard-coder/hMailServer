// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;

namespace hMailServer.ControlPanel.Views.Scaffold
{
   /// <summary>
   /// Keeps an <see cref="EmptyState"/> laid over a <see cref="DataGrid"/> under the
   /// grid's column headers, so that it is centred where the rows would be, as its
   /// style says, and never drawn across the headers.
   ///
   /// Laid over the whole grid, the empty state was centred over the headers as
   /// well: where the card is no taller than the empty state - the narrowest
   /// window, now that a list card is floored at what it holds (review finding
   /// F207) - its glyph sat at the top of the grid, 32 pixels down, against the
   /// headers' line under a header row of 33 pixels, and across it under the
   /// quarantine's, whose check box and four icon buttons make a row of about 48.
   /// The headers are measured rather than assumed, so a taller header row - a
   /// larger font, another theme - keeps the empty state under it too.
   /// </summary>
   public static class EmptyStateUnderHeaders
   {
      /// <summary>
      /// Gives <paramref name="empty"/> a top margin that puts it under the column
      /// headers of <paramref name="grid"/>, when the grid is loaded and whenever
      /// the headers change height. The two share the cell they are laid in.
      /// </summary>
      public static void Keep(EmptyState empty, DataGrid grid)
      {
         if (empty == null || grid == null)
            return;

         DataGridColumnHeadersPresenter headers = null;

         void Place()
         {
            if (headers == null)
            {
               headers = FindHeaders_(grid);
               if (headers == null)
                  return;
               headers.SizeChanged += (_, _) => Place();
            }

            // A grid that is not shown - a page whose read failed hides it - has
            // no header height to measure; the margin waits for the next one.
            if (!headers.IsVisible || headers.ActualHeight <= 0)
               return;

            double top;
            try
            {
               // The headers' foot in the coordinates of the cell the empty state
               // is laid in: the grid's own border is above them.
               Visual cell = VisualTreeHelper.GetParent(empty) as Visual ?? grid;
               top = headers.TransformToAncestor(cell).Transform(new Point(0, headers.ActualHeight)).Y;
            }
            catch (InvalidOperationException)
            {
               // Not in one tree with the empty state: the headers' own height.
               top = headers.ActualHeight;
            }

            top = Math.Max(0, Math.Round(top));
            Thickness margin = empty.Margin;
            if (margin.Top != top)
               empty.Margin = new Thickness(margin.Left, top, margin.Right, margin.Bottom);
         }

         grid.Loaded += (_, _) => Place();
         if (grid.IsLoaded)
            Place();
      }

      private static DataGridColumnHeadersPresenter FindHeaders_(DependencyObject root)
      {
         int count = VisualTreeHelper.GetChildrenCount(root);
         for (int i = 0; i < count; i++)
         {
            DependencyObject child = VisualTreeHelper.GetChild(root, i);
            if (child is DataGridColumnHeadersPresenter found)
               return found;

            DataGridColumnHeadersPresenter below = FindHeaders_(child);
            if (below != null)
               return below;
         }

         return null;
      }
   }
}
