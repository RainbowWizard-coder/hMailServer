// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;

namespace hMailServer.ControlPanel.Views.Scaffold
{
   /// <summary>
   /// The column a page's settings cards stand in: a vertical stack as wide as it is
   /// allowed to be - the page's width, up to its MaxWidth, the form width
   /// (DesignTokens.FormWidth) - whatever its cards hold, so that every card of a page
   /// and of each of its tabs ends at one right edge. A StackPanel aligned left with a
   /// MaxWidth is as wide as its widest card's content, and the cards' right edges
   /// jumped from tab to tab: 761 pixels on Transport security's Encryption tab, 526 on
   /// the TLS versions &amp; ciphers page's Ciphers tab (review finding F295). It stays aligned left, so
   /// that in a window wider than the form the cards keep to the form width at the
   /// left, as they did.
   ///
   /// Measured with an unbounded width - in a scroll viewer that scrolls sideways - it
   /// is as wide as its content, as a StackPanel is (Services/FormColumn, held by
   /// FormColumnTests).
   /// </summary>
   public class FormColumnPanel : StackPanel
   {
      protected override Size MeasureOverride(Size constraint)
      {
         Size desired = base.MeasureOverride(constraint);

         // The constraint is already within MinWidth and MaxWidth, the margin taken off.
         if (Orientation == Orientation.Vertical)
            desired.Width = FormColumn.Width(desired.Width, constraint.Width);

         return desired;
      }
   }
}
