// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A jump to a page shows the page's whole group in the sidebar (review
   /// finding F151). The jump ended with the row's BringIntoView, which scrolls
   /// as little as it can, so a row below the fold stopped on the sidebar's
   /// bottom edge with the rest of its group hidden. The numbers are the
   /// review's: a sidebar 670 pixels high at 1280 by 800, rows 27 pixels apart,
   /// the largest group a heading and fourteen rows, about 405 pixels.
   /// </summary>
   public class SidebarScrollTests
   {
      private const double Viewport = 670;
      private const double Row = 27;

      /// <summary>The group already whole on screen: nothing moves.</summary>
      [Fact]
      public void AGroupOnScreen_MovesNothing()
      {
         Assert.Null(SidebarScroll.OffsetToShow(0, Viewport, 100, 100 + 6 * Row, 100 + 3 * Row, 100 + 4 * Row));
         Assert.Null(SidebarScroll.OffsetToShow(400, Viewport, 420, 900, 800, 827));
      }

      /// <summary>
      /// Discovery, low in Monitoring &amp; troubleshooting, reached from the top
      /// of the sidebar: the whole group comes on screen, its last page and its
      /// heading both, where the row stopped on the bottom edge.
      /// </summary>
      [Fact]
      public void AGroupBelowTheFold_ComesWholeOnScreen()
      {
         double groupTop = 1000, groupBottom = groupTop + 15 * Row;
         double rowTop = groupTop + 12 * Row;

         double? to = SidebarScroll.OffsetToShow(0, Viewport, groupTop, groupBottom, rowTop, rowTop + Row);

         Assert.NotNull(to);
         Assert.True(groupBottom + SidebarScroll.Margin <= to.Value + Viewport, "The group's last page is still below the fold.");
         Assert.True(groupTop - SidebarScroll.Margin >= to.Value, "The group's heading went off the top.");
      }

      /// <summary>A group above the top of the sidebar: scrolled up until its heading is on screen, with the margin above it.</summary>
      [Fact]
      public void AGroupAboveTheTop_IsScrolledUpToItsHeading()
      {
         double? to = SidebarScroll.OffsetToShow(1200, Viewport, 300, 300 + 8 * Row, 300 + 2 * Row, 300 + 3 * Row);

         Assert.Equal(300 - SidebarScroll.Margin, to);
      }

      /// <summary>A group partly on screen moves by as little as shows all of it.</summary>
      [Fact]
      public void AGroupPartlyOnScreen_MovesByTheLeastThatShowsAllOfIt()
      {
         double groupTop = 500, groupBottom = 500 + 10 * Row;
         double? to = SidebarScroll.OffsetToShow(0, Viewport, groupTop, groupBottom, groupTop + Row, groupTop + 2 * Row);

         Assert.Equal(groupBottom + SidebarScroll.Margin - Viewport, to);
      }

      /// <summary>
      /// A group taller than the sidebar - a short window - cannot be shown
      /// whole: the row is left where it is when it is whole on screen, and
      /// otherwise put a third of the way down.
      /// </summary>
      [Fact]
      public void AGroupTallerThanTheSidebar_PutsTheRowAThirdOfTheWayDown()
      {
         const double shortViewport = 300;
         double groupTop = 200, groupBottom = 200 + 15 * Row;

         Assert.Null(SidebarScroll.OffsetToShow(200, shortViewport, groupTop, groupBottom, 300, 327));

         double rowTop = groupTop + 14 * Row;
         Assert.Equal(rowTop - shortViewport / 3, SidebarScroll.OffsetToShow(0, shortViewport, groupTop, groupBottom, rowTop, rowTop + Row));
      }

      /// <summary>Never above the top of the content, and nothing for a sidebar that has no height yet.</summary>
      [Fact]
      public void TheOffset_IsNeverBelowZero_AndAnUnmeasuredSidebarMovesNothing()
      {
         Assert.Equal(0, SidebarScroll.OffsetToShow(100, Viewport, 2, 2 + 4 * Row, 2, 2 + Row));
         Assert.Null(SidebarScroll.OffsetToShow(0, 0, 900, 1000, 900, 927));
         Assert.Null(SidebarScroll.OffsetToShow(double.NaN, Viewport, 900, 1000, 900, 927));
      }

      /// <summary>
      /// The window asks the rule after the jump has been laid out, of the
      /// sidebar's own viewer, and keeps BringIntoView for a top-level page and
      /// for a tree it cannot measure; NavigateTo no longer ends in the row's
      /// BringIntoView, and the tree scrolls by pixels.
      /// </summary>
      [Fact]
      public void TheWindow_ScrollsTheGroupAfterTheJump()
      {
         string shell = ReadControlPanelFile("MainWindow.xaml.cs");
         string navigate = WithheldRowTests.MemberText(shell, "private void NavigateTo(string key, bool fromSearch)");
         Assert.True(navigate != null, "MainWindow.NavigateTo is gone.");
         Assert.Contains("ShowGroupOf_(leaf);", navigate, StringComparison.Ordinal);
         Assert.DoesNotContain("leaf.BringIntoView();", navigate, StringComparison.Ordinal);

         string scroll = ReadControlPanelFile("MainWindow.GroupScroll.cs");
         Assert.Contains("DispatcherPriority.Loaded", scroll, StringComparison.Ordinal);
         Assert.Contains("SidebarScroll.OffsetToShow(viewer.VerticalOffset, viewer.ViewportHeight,", scroll, StringComparison.Ordinal);
         Assert.Contains("if (row.Parent is not TreeViewItem group)", scroll, StringComparison.Ordinal);
         Assert.Contains("viewer.CanContentScroll", scroll, StringComparison.Ordinal);

         string xaml = ReadControlPanelFile("MainWindow.xaml");
         Assert.Contains("ScrollViewer.CanContentScroll=\"False\"", xaml, StringComparison.Ordinal);
      }

      private static string ReadControlPanelFile(string relativePath)
      {
         for (var at = new DirectoryInfo(AppContext.BaseDirectory); at != null; at = at.Parent)
         {
            string candidate = Path.Join(at.FullName, "ControlPanel", relativePath);
            if (File.Exists(candidate) && Directory.Exists(Path.Join(at.FullName, "ControlPanel", "Views")))
               return File.ReadAllText(candidate);
         }

         throw new FileNotFoundException("the Control Panel's " + relativePath + " is not above " + AppContext.BaseDirectory);
      }
   }
}
