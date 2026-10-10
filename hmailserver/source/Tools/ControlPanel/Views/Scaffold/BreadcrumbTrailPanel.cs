// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using hMailServer.ControlPanel.Services;

namespace hMailServer.ControlPanel.Views.Scaffold
{
   /// <summary>
   /// The breadcrumb's trail on one line (MainWindow.UpdateBreadcrumb): its parts
   /// left to right, each as wide as it is where the line holds them all, and cut
   /// short where it does not in the order <see cref="ShellGeometry.BreadcrumbWidths"/>
   /// gives - the groups' segments first, the page's name last. The page's name is
   /// the trail's last part; a separator is a TextBlock and keeps its width; a
   /// group's segment is a button, and anything else is the "found by search"
   /// marker, the two cut first, each ending in its ellipsis. A DockPanel that
   /// filled its last part with what was left measured the group's button first,
   /// gave it all the width the bar had beside the help pane in the narrowest
   /// window, and the page's name none: the bar named the group and not the page
   /// (review finding F047).
   /// </summary>
   public class BreadcrumbTrailPanel : Panel
   {
      /// <summary>What a child is to the trail, by its place and its kind.</summary>
      private BreadcrumbPart PartOf_(int index, UIElement child)
         => index == InternalChildren.Count - 1 ? BreadcrumbPart.Page
            : child is TextBlock ? BreadcrumbPart.Fixed
            : child is ButtonBase ? BreadcrumbPart.Group
            : BreadcrumbPart.Marker;

      protected override Size MeasureOverride(Size availableSize)
      {
         UIElementCollection children = InternalChildren;
         var desired = new double[children.Count];
         var parts = new BreadcrumbPart[children.Count];
         var unbounded = new Size(double.PositiveInfinity, availableSize.Height);
         for (int i = 0; i < children.Count; i++)
         {
            children[i].Measure(unbounded);
            desired[i] = children[i].DesiredSize.Width;
            parts[i] = PartOf_(i, children[i]);
         }

         double[] widths = ShellGeometry.BreadcrumbWidths(availableSize.Width, desired, parts);

         double width = 0;
         double height = 0;
         for (int i = 0; i < children.Count; i++)
         {
            // A part that is cut is measured again in the width it is given, so that
            // its words end there in their ellipsis; one that is not keeps its own
            // measure: measured again in its own width, a group's button at 150 %
            // gave its words some three pixels less than they took, and they were
            // cut where the line had room for them.
            // Never under nothing: a share worked out to a hair below zero is no
            // width a Size can hold, and a layout that throws ends the program.
            if (widths[i] < desired[i])
               children[i].Measure(new Size(Math.Max(0, widths[i]), availableSize.Height));
            width += children[i].DesiredSize.Width;
            height = Math.Max(height, children[i].DesiredSize.Height);
         }

         return new Size(width, height);
      }

      protected override Size ArrangeOverride(Size finalSize)
      {
         // Each part at the width it measured to, the one after it straight after:
         // words cut short end at their ellipsis, not where their share ended.
         UIElementCollection children = InternalChildren;
         double x = 0;
         for (int i = 0; i < children.Count; i++)
         {
            double width = children[i].DesiredSize.Width;
            children[i].Arrange(new Rect(x, 0, width, finalSize.Height));
            x += width;
         }

         return finalSize;
      }
   }
}
