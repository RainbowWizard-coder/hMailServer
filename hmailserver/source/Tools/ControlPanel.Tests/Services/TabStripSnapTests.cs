// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using System.IO;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// An overflowing tab strip moves by whole headers (review findings F193 and
   /// F107): by pixels it began "… ing and disclaimer" and ended "Tagg". The strip
   /// here is six headers of 100 pixels with a 4-pixel gap, 620 pixels in all, in a
   /// viewport of 250, so that it scrolls 370.
   /// </summary>
   public class TabStripSnapTests
   {
      private const double Viewport = 250;
      private const double Scrollable = 370;

      private static readonly List<(double Left, double Width)> Strip = new()
      {
         (0, 100), (104, 100), (208, 100), (312, 100), (416, 100), (520, 100)
      };

      [Fact]
      public void Forward_PutsTheHeaderCutAtTheRightAtTheLeftEdge()
      {
         // At 0 the third header (208..308) is cut by the right edge at 250.
         Assert.Equal(208, TabStripSnap.Forward(Strip, 0, Viewport, Scrollable));
         // At 208 the fifth (416..516) is cut at 458.
         Assert.Equal(370, TabStripSnap.Forward(Strip, 208, Viewport, Scrollable));
      }

      [Fact]
      public void Forward_NeverPassesTheEndOfTheStrip()
      {
         Assert.Equal(Scrollable, TabStripSnap.Forward(Strip, Scrollable, Viewport, Scrollable));
         Assert.Equal(Scrollable, TabStripSnap.Forward(Strip, 312, Viewport, Scrollable));
      }

      [Fact]
      public void Back_GoesBackByTheHeadersThatFitAndKeepsTheFirstWholeOneInView()
      {
         // At 312 the first header shown is the fourth; a step back shows the two
         // before it whole and ends where the fourth begins.
         Assert.Equal(104, TabStripSnap.Back(Strip, 312, Viewport, Scrollable));
         Assert.Equal(0, TabStripSnap.Back(Strip, 104, Viewport, Scrollable));
      }

      [Fact]
      public void Back_FromTheEndShowsTheHeaderTheEndCutWhole()
      {
         // At the end, 370, the left edge cuts the fourth header (312..412): the
         // step back keeps it whole at the right.
         double back = TabStripSnap.Back(Strip, Scrollable, Viewport, Scrollable);
         Assert.Equal(208, back);
         Assert.True(412 <= back + Viewport);
      }

      [Fact]
      public void Back_AtTheStartStaysThere()
         => Assert.Equal(0, TabStripSnap.Back(Strip, 0, Viewport, Scrollable));

      [Fact]
      public void ANotchIsOneHeader()
      {
         Assert.Equal(104, TabStripSnap.Notch(Strip, 0, Scrollable, 1));
         Assert.Equal(208, TabStripSnap.Notch(Strip, 104, Scrollable, 1));
         Assert.Equal(104, TabStripSnap.Notch(Strip, 208, Scrollable, -1));
         Assert.Equal(0, TabStripSnap.Notch(Strip, 50, Scrollable, -1));
         Assert.Equal(Scrollable, TabStripSnap.Notch(Strip, 312, Scrollable, 1));
      }

      [Fact]
      public void Reveal_BringsAHeaderInByAWholeHeaderStep()
      {
         // The fourth (312..412) from 0: the first edge from which it fits is 208.
         Assert.Equal(208, TabStripSnap.Reveal(Strip, 3, 0, Viewport, Scrollable));
         // The first from 208: its own left edge.
         Assert.Equal(0, TabStripSnap.Reveal(Strip, 0, 208, Viewport, Scrollable));
         // A header already whole leaves the strip where it is.
         Assert.Equal(104, TabStripSnap.Reveal(Strip, 2, 104, Viewport, Scrollable));
         // The last: the end of the strip.
         Assert.Equal(Scrollable, TabStripSnap.Reveal(Strip, 5, 0, Viewport, Scrollable));
      }

      /// <summary>
      /// A header already whole, where the strip's left edge cuts the one before it -
      /// what Windows' least-distance scroll to the selected header leaves: the strip
      /// moves on to the next header's edge, the selected one staying whole.
      /// </summary>
      [Fact]
      public void Reveal_MovesPastAHeaderCutAtTheLeft()
      {
         // At 50 the first header (0..100) is cut; the second (104..204) is whole.
         Assert.Equal(104, TabStripSnap.Reveal(Strip, 1, 50, Viewport, Scrollable));
         // The third (208..308) whole at 60 too: the strip moves to the second's edge.
         Assert.Equal(104, TabStripSnap.Reveal(Strip, 2, 60, Viewport, Scrollable));
         // Not past the header revealed: at 150 the second header is cut and is the one revealed.
         Assert.Equal(104, TabStripSnap.Reveal(Strip, 1, 150, Viewport, Scrollable));
      }

      [Fact]
      public void ACutHeaderIsSaidAtTheEdgeThatCutsIt()
      {
         Assert.False(TabStripSnap.CutAtLeft(Strip, 0));
         Assert.True(TabStripSnap.CutAtRight(Strip, 0, Viewport));

         // On a header's edge the left is whole.
         Assert.False(TabStripSnap.CutAtLeft(Strip, 208));
         Assert.True(TabStripSnap.CutAtRight(Strip, 208, Viewport));

         // At the end the right is whole and the left cuts the fourth.
         Assert.True(TabStripSnap.CutAtLeft(Strip, Scrollable));
         Assert.False(TabStripSnap.CutAtRight(Strip, Scrollable, Viewport));

         // In the gap between two headers nothing is cut.
         Assert.False(TabStripSnap.CutAtRight(Strip, 0, 206));
      }

      /// <summary>
      /// A chevron is live while a header is cut or out of sight on its side,
      /// not while pixels are: with the last header whole a few pixels short of
      /// the strip's end, the right chevron has no tab to go to.
      /// </summary>
      [Fact]
      public void AChevronIsLiveWhileAHeaderIsBeyondIt()
      {
         Assert.True(TabStripSnap.MoreAfter(Strip, 0, Viewport));
         Assert.True(TabStripSnap.MoreAfter(Strip, 312, Viewport));
         Assert.False(TabStripSnap.MoreAfter(Strip, 370, Viewport));
         // The last header (520..620) whole at 372 of a strip with a trailing gap.
         Assert.False(TabStripSnap.MoreAfter(Strip, 372, Viewport));

         Assert.False(TabStripSnap.MoreBefore(Strip, 0));
         Assert.True(TabStripSnap.MoreBefore(Strip, 104));
      }

      [Fact]
      public void AStripThatFitsDoesNotMove()
      {
         var fits = new List<(double Left, double Width)> { (0, 100), (104, 100) };
         Assert.Equal(0, TabStripSnap.Forward(fits, 0, Viewport, 0));
         Assert.Equal(0, TabStripSnap.Back(fits, 0, Viewport, 0));
         Assert.Equal(0, TabStripSnap.Notch(fits, 0, 0, 1));
         Assert.False(TabStripSnap.CutAtRight(fits, 0, Viewport));
      }

      /// <summary>
      /// A header wider than the strip still moves the strip on, one header at a
      /// time, rather than leaving the chevron doing nothing.
      /// </summary>
      [Fact]
      public void AHeaderWiderThanTheStripStillMovesOn()
      {
         var wide = new List<(double Left, double Width)> { (0, 300), (304, 100), (408, 100) };
         double scrollable = 508 - Viewport;
         double forward = TabStripSnap.Forward(wide, 0, Viewport, scrollable);
         Assert.True(forward > 0);
         Assert.Equal(0, TabStripSnap.Back(wide, forward, Viewport, scrollable));
      }

      /// <summary>
      /// The strip's chevrons, its wheel and the header brought in by keyboard or
      /// click all go through the snap, and a cut edge fades through an opacity
      /// mask on the strip - never a colour laid over it, which would have to be
      /// the colour of whatever the strip is on.
      /// </summary>
      [Fact]
      public void TheStripScrollsThroughTheSnapAndFadesItsCutEdges()
      {
         string code = ReadControlPanelFile(Path.Join("Services", "TabStripScrolling.cs"));
         Assert.Contains("TabStripSnap.Back(headers", code, System.StringComparison.Ordinal);
         Assert.Contains("TabStripSnap.Forward(headers", code, System.StringComparison.Ordinal);
         Assert.Contains("TabStripSnap.Notch(", code, System.StringComparison.Ordinal);
         Assert.Contains("TabStripSnap.Reveal(", code, System.StringComparison.Ordinal);
         Assert.Contains("RequestBringIntoViewEvent", code, System.StringComparison.Ordinal);
         Assert.Contains("viewer.OpacityMask = mask;", code, System.StringComparison.Ordinal);
         Assert.DoesNotContain("BringIntoView();", code, System.StringComparison.Ordinal);
         Assert.DoesNotContain("PageFraction", code, System.StringComparison.Ordinal);
      }

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(System.AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
