// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A page whose list fills the window (<see cref="PageFill"/>, laid out by
   /// Views/Scaffold/FillPanel). The numbers are the IP ranges page's own, read off
   /// renders of it (work item 79): the other parts are the toolbar, the
   /// permissions card and the add form with their gaps, and the floor is the list
   /// card's three rows. Two layouts lost something: 6.3.4's gave the list what the
   /// window left after the other parts and nothing when that was negative, so in
   /// the reporter's window there was no list; 6.3.5's held the page to the
   /// viewport over a fixed 560 pixels, so in the narrowest window the add form
   /// ran past the page's end, where scrolling could not reach it.
   /// </summary>
   public class PageFillTests
   {
      private const double Floor = 154;

      /// <summary>An English page in a 1280 by 800 window: everything fits, the list takes the rest, nothing scrolls.</summary>
      [Fact]
      public void WhenEverythingFits_TheListTakesWhatTheOtherPartsLeave()
      {
         const double viewport = 559, others = 393;

         Assert.Equal(166, PageFill.FillHeight(viewport, others, Floor));
         Assert.Equal(viewport, PageFill.PageHeight(viewport, others, Floor));
         Assert.False(PageFill.PageHeight(viewport, others, Floor) > viewport, "The page scrolls with room to spare.");
      }

      /// <summary>
      /// The reporter's window, in Portuguese at 125 %: 333 pixels of viewport under
      /// the page's title and 393 of other parts. 6.3.4 gave the list 333 - 393, which
      /// is less than nothing, and drew none of it; the list keeps its floor and the
      /// page scrolls instead.
      /// </summary>
      [Fact]
      public void AWindowShorterThanTheOtherParts_KeepsTheListAtItsFloor_AndScrolls()
      {
         const double viewport = 333, others = 393;

         Assert.True(viewport - others < 0, "The case is the one that lost the list: the other parts alone are taller than the window.");
         Assert.Equal(Floor, PageFill.FillHeight(viewport, others, Floor));
         Assert.Equal(others + Floor, PageFill.PageHeight(viewport, others, Floor));
         Assert.True(PageFill.PageHeight(viewport, others, Floor) > viewport, "The page does not scroll, so its lower parts are out of reach.");
      }

      /// <summary>
      /// The narrowest window (760 by 520, the page 560 wide) in Portuguese: the
      /// permissions wrap and the other parts come to 577 pixels. A page held to a
      /// fixed 560 ended above the add form; the page is always the parts and the
      /// floor, so every part of it can be scrolled to.
      /// </summary>
      [Fact]
      public void TheNarrowestWindow_IsAsTallAsItsParts_SoTheAddFormCanBeReached()
      {
         const double viewport = 264, others = 577, fixedFloorOf6_3_5 = 560;

         double page = PageFill.PageHeight(viewport, others, Floor);
         Assert.Equal(others + Floor, page);
         Assert.True(page > fixedFloorOf6_3_5, "The fixed floor 6.3.5 used was shorter than the page's parts, which is what cut the add form off.");
      }

      [Theory]
      [InlineData(0)]
      [InlineData(double.NaN)]
      [InlineData(double.PositiveInfinity)]
      [InlineData(-40)]
      public void AViewportNotKnownYet_GivesTheFloor(double viewport)
      {
         Assert.Equal(Floor, PageFill.FillHeight(viewport, 393, Floor));
         Assert.Equal(393 + Floor, PageFill.PageHeight(viewport, 393, Floor));
      }

      [Fact]
      public void TheFillingPart_IsNeverLessThanItsFloor_AndThePageNeverLessThanItsParts()
      {
         for (double viewport = 0; viewport <= 1400; viewport += 7)
         {
            for (double others = 0; others <= 900; others += 13)
            {
               double fill = PageFill.FillHeight(viewport, others, Floor);
               double page = PageFill.PageHeight(viewport, others, Floor);
               Assert.True(fill >= Floor, "viewport " + viewport + ", others " + others);
               Assert.True(page >= viewport && page >= others + Floor, "viewport " + viewport + ", others " + others);
               Assert.Equal(page, others + fill, 6);
            }
         }
      }

      [Fact]
      public void ANegativeOrMissingFloor_IsNoFloor()
      {
         Assert.Equal(0, PageFill.FillHeight(300, 400, -10));
         Assert.Equal(0, PageFill.FillHeight(300, 400, double.NaN));
         Assert.Equal(100, PageFill.FillHeight(500, 400, double.NaN));
      }
   }
}
