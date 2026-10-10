// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Windows;
using hMailServer.ControlPanel.Services;

namespace hMailServer.ControlPanel
{
   /// <summary>
   /// The sidebar's width beside the help pane (Services/ShellGeometry; review
   /// findings F021 and F022). The sidebar was a star column beside the page, and
   /// the help pane, a column of fixed width, took its width from both: opened at
   /// 1280 it squeezed the tree to its 200-pixel floor and cut three group names.
   /// The tree is now a share of the window's width, set in pixels as the window
   /// changes, so that the pane takes its width from the page alone; where that
   /// would leave the page less than ShellGeometry.PageMinWidthBesideHelp, the
   /// sidebar is the rail while the pane is open - a glyph opening its group's
   /// pages in the flyout, as below the collapse threshold - and the tree again
   /// once the pane closes. Neither is recorded as the administrator's choice.
   /// </summary>
   public partial class MainWindow
   {
      // The administrator expanded the sidebar while the help pane had made it the
      // rail: the tree stays beside the pane until the pane is opened or closed again.
      private bool treeBesideHelpChosen_;

      /// <summary>The width the sidebar, the page and the help pane share: the window's content.</summary>
      private double BodyWidth_ => Content is FrameworkElement body && body.ActualWidth > 0 ? body.ActualWidth : ActualWidth;

      /// <summary>The help pane's width while it is open, as set or as the splitter left it; 0 while it is closed.</summary>
      private double HelpWidth_ => !helpOpen_ ? 0 : HelpColumn.Width.IsAbsolute ? HelpColumn.Width.Value : HelpColumn.ActualWidth;

      /// <summary>Whether the help pane makes the sidebar the rail: open, beside a page it would leave too narrow, and not overruled.</summary>
      private bool RailForHelp_() => helpOpen_ && !treeBesideHelpChosen_ && ShellGeometry.RailBesideHelp(BodyWidth_, HelpWidth_);

      /// <summary>The tree's width for the window as it is now, while the tree shows.</summary>
      private void SizeSidebar_()
      {
         if (!sidebarShown_ || railShowing_)
            return;

         double width = ShellGeometry.SidebarWidth(BodyWidth_);
         if (!SidebarColumn.Width.IsAbsolute || SidebarColumn.Width.Value != width)
            SidebarColumn.Width = new GridLength(width);
      }

      /// <summary>
      /// The help pane opened or closed, or its splitter moved: the sidebar is the
      /// rail or the tree again by the room the page is left, and an expansion the
      /// administrator chose beside the pane lasts only as long as the pane.
      /// </summary>
      private void HelpPaneMoved_(bool openedOrClosed)
      {
         if (openedOrClosed)
            treeBesideHelpChosen_ = false;
         ApplySidebar_();
      }

      private void WireShellGeometry_()
      {
         // The splitter's drag changes the pane's width; the sidebar follows once it
         // ends, so that it does not turn into the rail and back under the pointer.
         HelpSplitter.DragCompleted += (s, e) => HelpPaneMoved_(false);
      }
   }
}
