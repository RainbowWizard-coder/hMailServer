// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The width of the column a page's cards stand in (Views/Scaffold/FormColumnPanel):
   /// the width it is allowed - the page's, up to its MaxWidth - whatever its cards hold,
   /// so that every card of a page and of each of its tabs ends at one right edge, where a
   /// StackPanel aligned left was as wide as its widest card's content and the edge moved
   /// from tab to tab (review finding F295). With no limit - measured in a scroll viewer
   /// that scrolls sideways - it is its content's width, as a StackPanel's is; and it is
   /// never narrower than its content, which a StackPanel would overflow.
   /// </summary>
   public static class FormColumn
   {
      public static double Width(double contentWidth, double allowedWidth)
      {
         if (double.IsInfinity(allowedWidth) || double.IsNaN(allowedWidth))
            return contentWidth;

         return System.Math.Max(contentWidth, allowedWidth);
      }
   }
}
