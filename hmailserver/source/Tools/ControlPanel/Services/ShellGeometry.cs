// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;

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
   /// autoconfiguration" was cut in English, and seven page labels in German (F022).
   /// The sidebar is now a share of the window's width, whatever the help pane
   /// does, so the pane takes its width from the page alone; and where that would
   /// leave the page narrower than <see cref="PageMinWidthBesideHelp"/>, the
   /// sidebar is the rail while the pane is open, and the tree again once it closes.
   /// At 1280 its 310 pixels hold every English page label whole and five of the
   /// seven German ones; "Übersicht der Transportverschlüsselung" and "Servergrenzen
   /// und Experteneinstellungen" still end in an ellipsis, whole in their tool tips.
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

      /// <summary>The narrowest a group's segment of the breadcrumb, or the search's marker, is cut to before the page's name is cut at all: a word's first letters and the ellipsis.</summary>
      public const double BreadcrumbGroupFloor = 48;

      /// <summary>The narrowest the page's name is cut to before a group's segment goes under <see cref="BreadcrumbGroupFloor"/>: a word and the ellipsis.</summary>
      public const double BreadcrumbPageFloor = 96;

      /// <summary>The narrowest a group's segment is ever cut to: the button's ellipsis, still there to press.</summary>
      public const double BreadcrumbGroupLeast = 24;

      /// <summary>
      /// The widths of the breadcrumb's parts on one line <paramref name="available"/>
      /// pixels wide (Views/Scaffold/BreadcrumbTrailPanel): each as wide as it is where
      /// the line holds them all. Where it does not, the page's name has the
      /// priority, as the part that says where the reader is: the groups' segments
      /// and the search's marker are cut first, to <see cref="BreadcrumbGroupFloor"/>;
      /// then the marker is left out; then the page's name is cut, to
      /// <see cref="BreadcrumbPageFloor"/>; then the groups' segments, to
      /// <see cref="BreadcrumbGroupLeast"/>; and only then the page's name, to what is
      /// left. Each cut is shared among the parts it takes from by what each can
      /// give. A separator keeps its width. In a bar that left the trail less than
      /// the group's words, beside the help pane in the narrowest window, the group
      /// took all of it and the page's name was given nothing, not even its
      /// ellipsis (review finding F047).
      /// </summary>
      public static double[] BreadcrumbWidths(double available, IReadOnlyList<double> desired, IReadOnlyList<BreadcrumbPart> parts)
      {
         ArgumentNullException.ThrowIfNull(desired);
         ArgumentNullException.ThrowIfNull(parts);
         if (desired.Count != parts.Count)
            throw new ArgumentException("A part for each width.", nameof(parts));

         var widths = new double[desired.Count];
         double total = 0;
         for (int i = 0; i < widths.Length; i++)
         {
            widths[i] = double.IsNaN(desired[i]) || desired[i] < 0 ? 0 : desired[i];
            total += widths[i];
         }

         if (double.IsNaN(available) || double.IsPositiveInfinity(available) || total <= available)
            return widths;

         double excess = total - Math.Max(0, available);
         excess = Cut_(widths, parts, part => part == BreadcrumbPart.Group || part == BreadcrumbPart.Marker, BreadcrumbGroupFloor, excess);
         excess = Cut_(widths, parts, part => part == BreadcrumbPart.Marker, 0, excess);
         excess = Cut_(widths, parts, part => part == BreadcrumbPart.Page, BreadcrumbPageFloor, excess);
         excess = Cut_(widths, parts, part => part == BreadcrumbPart.Group, BreadcrumbGroupLeast, excess);
         Cut_(widths, parts, part => part == BreadcrumbPart.Page, 0, excess);
         return widths;
      }

      /// <summary>Takes up to <paramref name="excess"/> from the parts <paramref name="cut"/> picks, none under <paramref name="floor"/> (nor under its own width), each by what it can give; what is still to take.</summary>
      private static double Cut_(double[] widths, IReadOnlyList<BreadcrumbPart> parts, Func<BreadcrumbPart, bool> cut, double floor, double excess)
      {
         if (excess <= 0)
            return 0;

         double slack = 0;
         for (int i = 0; i < widths.Length; i++)
         {
            if (cut(parts[i]))
               slack += Math.Max(0, widths[i] - floor);
         }

         if (slack <= 0)
            return excess;

         double take = Math.Min(excess, slack);
         for (int i = 0; i < widths.Length; i++)
         {
            if (cut(parts[i]) && widths[i] > floor)
               widths[i] = Math.Max(floor, widths[i] - take * (widths[i] - floor) / slack);
         }

         return excess - take;
      }
   }

   /// <summary>What a part of the breadcrumb is, for the order in which <see cref="ShellGeometry.BreadcrumbWidths"/> cuts it.</summary>
   public enum BreadcrumbPart
   {
      /// <summary>A separator: never cut.</summary>
      Fixed,

      /// <summary>A group's segment: cut first, and never past its ellipsis.</summary>
      Group,

      /// <summary>The "found by search" marker before the trail: cut with the groups, then left out before the page's name is cut.</summary>
      Marker,

      /// <summary>The page's name, the trail's last part: cut last.</summary>
      Page
   }
}
