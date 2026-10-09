// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views;

namespace hMailServer.ControlPanel
{
   /// <summary>
   /// The sidebar after a jump shows the page's whole group, not only its row
   /// (review finding F151): NavigateTo serves the palette, the Welcome tiles,
   /// the Related links, the pages' own buttons, Back and Forward and the tours,
   /// and ended with the row's BringIntoView, which stopped a row below the fold
   /// on the sidebar's bottom edge with the rest of its group hidden under it.
   /// Where the sidebar scrolls is Services/SidebarScroll's rule; this measures
   /// the rows and scrolls the tree's viewer.
   /// </summary>
   public partial class MainWindow
   {
      /// <summary>
      /// Scrolls the sidebar so that <paramref name="row"/>'s group is on screen,
      /// once the selection and the expanded groups have been laid out. A
      /// top-level page - Welcome, the Dashboard, About - has no group and is
      /// brought into view as before, and so is a row the tree cannot measure: a
      /// tree that is not shown (the rail) or one that scrolls by items.
      /// </summary>
      private void ShowGroupOf_(TreeViewItem row)
      {
         if (row.Parent is not TreeViewItem group)
         {
            row.BringIntoView();
            return;
         }

         Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
         {
            ScrollViewer viewer = PageCommands.Descendants_(NavTree).OfType<ScrollViewer>().FirstOrDefault();
            if (viewer == null || viewer.CanContentScroll || !row.IsVisible || !group.IsVisible || viewer.ViewportHeight <= 0)
            {
               row.BringIntoView();
               return;
            }

            double groupTop = ContentTop_(group, viewer);
            double rowTop = ContentTop_(row, viewer);
            double? to = SidebarScroll.OffsetToShow(viewer.VerticalOffset, viewer.ViewportHeight,
               groupTop, groupTop + group.ActualHeight, rowTop, rowTop + row.ActualHeight);
            if (to.HasValue)
               viewer.ScrollToVerticalOffset(to.Value);
         }));
      }

      /// <summary>The top of <paramref name="element"/> in the coordinates of the viewer's whole content.</summary>
      private static double ContentTop_(FrameworkElement element, ScrollViewer viewer)
         => element.TransformToAncestor(viewer).Transform(new Point(0, 0)).Y + viewer.VerticalOffset;
   }
}
