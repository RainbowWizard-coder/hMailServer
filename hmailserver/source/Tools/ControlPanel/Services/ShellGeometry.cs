// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// How wide the shell's sidebar is, and when it gives way to the rail beside the
   /// help pane (MainWindow.Geometry.cs). Free of WPF, so that the arithmetic is
   /// tested without a window.
   ///
   /// The sidebar was a star column of 0.28 beside the page's 1, so that the help
   /// pane, a column of fixed width, took its width from both: opened at 1280 it
   /// squeezed the sidebar from 280 pixels to its 200 floor, and three group names
   /// were cut - "Accounts &amp; doma…", "Mail flow &amp; deliv…", "Connections &amp; pr…"
   /// (review finding F021) - while at the window's opening size "Web services &amp;
   /// autoconfiguration" was the one page label cut in English and German (F022).
   /// The sidebar is now a share of the window's width, whatever the help pane
   /// does, so the pane takes its width from the page alone; and where that would
   /// leave the page narrower than <see cref="PageMinWidthBesideHelp"/>, the
   /// sidebar is the rail while the pane is open, and the tree again once it closes.
   /// </summary>
   public static class ShellGeometry
   {
      /// <summary>
      /// The sidebar's share beside the page's 1, as a star column's weight: 0.32 is
      /// 310 pixels of a 1280-pixel window, inside the clamp, where 0.28 was 280.
      /// </summary>
      public const double SidebarShare = 0.32;

      /// <summary>
      /// The narrowest a page is left beside the help pane before the sidebar gives
      /// way to the rail, as the review proposed (finding F021): room for a form's
      /// row, a toolbar and a list's columns. Beside the tree and a pane of 380, a
      /// window narrower than about 1180 pixels would leave the page less.
      /// </summary>
      public const double PageMinWidthBesideHelp = 512;

      /// <summary>
      /// The tree's width in a body this wide: its share of it, between
      /// <see cref="DesignTokens.Shell.SidebarMinWidth"/> and
      /// <see cref="DesignTokens.Shell.SidebarMaxWidth"/>. The help pane is not part
      /// of the sum, so opening it leaves the sidebar as it was.
      /// </summary>
      public static double SidebarWidth(double bodyWidth)
      {
         if (double.IsNaN(bodyWidth) || bodyWidth <= 0)
            return DesignTokens.Shell.SidebarMinWidth;

         double share = bodyWidth * SidebarShare / (1 + SidebarShare);
         return Math.Floor(Math.Clamp(share, DesignTokens.Shell.SidebarMinWidth, DesignTokens.Shell.SidebarMaxWidth));
      }

      /// <summary>
      /// Whether the sidebar is the rail while the help pane is open: when the tree
      /// and a pane of <paramref name="helpWidth"/> would leave the page less than
      /// <see cref="PageMinWidthBesideHelp"/>. A pane that is not open never asks for it.
      /// </summary>
      public static bool RailBesideHelp(double bodyWidth, double helpWidth)
         => helpWidth > 0 && bodyWidth > 0 && bodyWidth - SidebarWidth(bodyWidth) - helpWidth < PageMinWidthBesideHelp;
   }
}
