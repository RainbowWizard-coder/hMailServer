// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A page whose list fills the window (<see cref="PageFill"/>, laid out by
   /// Views/Scaffold/FillPanel). The numbers are the IP ranges page's own, read off
   /// renders of it (work item 79): the other parts are the toolbar, the
   /// permissions card and the add form with their gaps, and the floor is the list
   /// card's three rows (153) or, taller, the empty state that takes their place
   /// where there are none (157). Two layouts lost something: 6.3.4's gave the list what the
   /// window left after the other parts and nothing when that was negative, so in
   /// the reporter's window there was no list; 6.3.5's held the page to the
   /// viewport over a fixed 560 pixels, so in the narrowest window the add form
   /// ran past the page's end, where scrolling could not reach it. Where several
   /// parts fill (<see cref="PageFill.Share"/>), the numbers are the domains
   /// page's, whose two rows had the same fixed floor, 640 pixels.
   /// </summary>
   public class PageFillTests
   {
      private const double Floor = 157;

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

      // ---- several filling parts: the domains and rules pages -------------------------------
      //
      // The domains page's right-hand column, in English, read off renders of it: the
      // accounts card (weight 3) asks for 346 pixels at its floor - its title, search,
      // form and three rows of accounts - and the row of alias and list cards (weight
      // 2) for 279, its 12-pixel gap and the alias card's three rows and two-line form.
      // In 6.3.6 the two were a grid's 3* and 2* rows over a fixed 640 pixels.

      private static readonly double[] DomainColumnFloors = { 346, 279 };
      private static readonly double[] DomainColumnWeights = { 3, 2 };

      /// <summary>A 1600 by 1000 window (759 pixels of viewport): the floors fit, and the room is shared three to two, as the rows were.</summary>
      [Fact]
      public void SeveralParts_ShareTheRoomByWeight_WhenTheirFloorsFit()
      {
         double[] heights = PageFill.Share(759, 0, DomainColumnFloors, DomainColumnWeights);

         Assert.Equal(759 * 3 / 5.0, heights[0], 6);
         Assert.Equal(759 * 2 / 5.0, heights[1], 6);
      }

      /// <summary>
      /// A 1280 by 800 window (559 pixels): the floors come to 625, so each part has its
      /// floor and the page scrolls by 66, where a fixed 640 scrolled by 81 and, in the
      /// narrowest window, still cut the alias form off.
      /// </summary>
      [Fact]
      public void SeveralParts_KeepTheirFloors_WhenTheWindowIsShorterThanThem()
      {
         double[] heights = PageFill.Share(559, 0, DomainColumnFloors, DomainColumnWeights);

         Assert.Equal(DomainColumnFloors, heights);
         Assert.True(heights.Sum() > 559, "The parts' floors were squeezed into a window shorter than them.");
      }

      /// <summary>
      /// A part whose share would be under its floor has its floor, and the others share
      /// what is left: at 640 pixels three fifths for the accounts is 384 and two fifths
      /// for the alias and list cards 256, under their 279.
      /// </summary>
      [Fact]
      public void APartUnderItsFloor_HasItsFloor_AndTheOthersShareTheRest()
      {
         double[] heights = PageFill.Share(640, 0, DomainColumnFloors, DomainColumnWeights);

         Assert.Equal(279, heights[1]);
         Assert.Equal(640 - 279, heights[0]);
      }

      [Fact]
      public void SharedHeights_AreNeverLessThanTheirFloors_AndAddUpToTheRoomOrTheFloors()
      {
         double[] floors = { 157, 0, 240, 102 };
         double[] weights = { 1, 2, 1.3, 0 };
         for (double viewport = 0; viewport <= 1400; viewport += 11)
         {
            for (double others = 0; others <= 600; others += 37)
            {
               double[] heights = PageFill.Share(viewport, others, floors, weights);
               double room = Math.Max(0, viewport - others);
               for (int i = 0; i < floors.Length; i++)
                  Assert.True(heights[i] >= floors[i], "viewport " + viewport + ", others " + others + ", part " + i);
               Assert.Equal(Math.Max(room, floors.Sum()), heights.Sum(), 6);

               // A part with no weight takes nothing beyond its floor.
               Assert.Equal(floors[3], heights[3]);
            }
         }
      }

      [Fact]
      public void OnePart_SharesAsFillHeightFills()
      {
         for (double viewport = -50; viewport <= 1000; viewport += 9)
         {
            for (double others = 0; others <= 700; others += 23)
               Assert.Equal(PageFill.FillHeight(viewport, others, Floor), PageFill.Share(viewport, others, new[] { Floor }, new[] { 1.0 })[0]);
         }
      }

      [Fact]
      public void AViewportNotKnownYet_GivesEveryPartItsFloor()
      {
         foreach (double viewport in new[] { 0, double.NaN, double.PositiveInfinity, -40 })
            Assert.Equal(DomainColumnFloors, PageFill.Share(viewport, 0, DomainColumnFloors, DomainColumnWeights));
      }

      [Fact]
      public void EveryPart_NeedsAFloorAndAWeight()
      {
         Assert.Throws<ArgumentException>(() => PageFill.Share(500, 0, new[] { 100.0, 100 }, new[] { 1.0 }));
         Assert.Throws<ArgumentNullException>(() => PageFill.Share(500, 0, null, new[] { 1.0 }));
         Assert.Empty(PageFill.Share(500, 0, Array.Empty<double>(), Array.Empty<double>()));
      }

      // ---- layout rounding ------------------------------------------------------------------
      //
      // At 150 % scaling a pixel is two thirds of a device-independent one, and WPF
      // rounds a part's size to whole device pixels: the IP ranges page's list card,
      // with a MinHeight of 157, is 236 device pixels high and asks for 157.33. Held
      // to 157 by its floor, every part under it was drawn a device pixel higher than
      // FillPanel's first version drew it, and the page was that much shorter than
      // its parts.

      private const double RoundedAt150 = 236 / 1.5;

      [Fact]
      public void APartAskingForMoreThanItsShare_ByLayoutRounding_IsFlooredAtWhatItAsks()
      {
         Assert.Equal(RoundedAt150, PageFill.MeasuredFloor(Floor, Floor, RoundedAt150));

         // A share above the floor that rounding takes up raises the floor to it too.
         Assert.Equal(300 + 1 / 3.0, PageFill.MeasuredFloor(Floor, 300, 300 + 1 / 3.0));
      }

      [Fact]
      public void APartAskingForNoMoreThanItsShare_KeepsItsFloor()
      {
         Assert.Equal(Floor, PageFill.MeasuredFloor(Floor, Floor, Floor));
         Assert.Equal(Floor, PageFill.MeasuredFloor(Floor, 300, 300));
         Assert.Equal(Floor, PageFill.MeasuredFloor(Floor, 300, 120));
         Assert.Equal(Floor, PageFill.MeasuredFloor(Floor, Floor, double.NaN));
      }

      [Fact]
      public void FillPanel_FloorsAMeasuredPart_AtWhatItAsksFor()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "Scaffold", "FillPanel.cs"));
         string measure = WithheldRowTests.MemberText(code, "protected override Size MeasureOverride(Size availableSize)");
         Assert.True(measure != null, "FillPanel.MeasureOverride is gone.");
         Assert.True(measure.Contains("PageFill.MeasuredFloor(floors[i], heights[i], fill.DesiredSize.Height)", StringComparison.Ordinal),
            "FillPanel no longer floors a part it measured at what the part asks for, so layout rounding at 125 or 150 % moves the parts under it.");
         Assert.True(measure.IndexOf("PageFill.MeasuredFloor(", StringComparison.Ordinal) < measure.IndexOf("floors_ = floors;", StringComparison.Ordinal),
            "FillPanel keeps the floors it arranges by before a measured part's floor is raised.");
      }

      // ---- the device-pixel grid ------------------------------------------------------------
      //
      // The Suppression list in a 1280 by 800 window at 150 %, read off a layout dump
      // of the page: 521.33 pixels of viewport (782 device pixels) under the page's
      // title, a toolbar and a notice of 49.33 each, and two filling cards sharing
      // the rest two to one - the list, floored at 157, and the bounce counts, at 193
      // (a MinHeight of 181 and a 12-pixel margin). The counts' third of the room is
      // less than its floor, so it has its floor, and 193 is 289.5 device pixels:
      // measured at 193 the card asked for 290, 193.33, the parts came to 521.67,
      // layout rounding made that 522, and the page had a scroll bar the height of
      // the window in English and Japanese; in German the subtitle's third line left
      // a viewport whose fraction rounded the other way. A floor on the grid is asked
      // for exactly, and the parts sum to the viewport.

      private const double Pixel = 1 / 1.5;

      [Fact]
      public void AFloorBetweenTwoDevicePixels_IsRaisedToTheNext()
      {
         Assert.Equal(290 * Pixel, PageFill.SnapUp(193, Pixel), 6);
         Assert.Equal(236 * Pixel, PageFill.SnapUp(157, Pixel), 6);
         Assert.Equal(242 * 0.8, PageFill.SnapUp(193, 0.8), 6);
         Assert.Equal(194, PageFill.SnapUp(193.5, 1));
      }

      [Fact]
      public void AFloorOnTheGrid_Stays()
      {
         Assert.Equal(193, PageFill.SnapUp(193, 1));
         Assert.Equal(290 * Pixel, PageFill.SnapUp(290 * Pixel, Pixel), 6);

         // The arithmetic's own error does not take a value on the grid up a pixel:
         // 74 device pixels, 49.333..., divided by two thirds is 74.00000000000001.
         Assert.Equal(74 * Pixel, PageFill.SnapUp(49.333333333333336, Pixel), 6);
         for (int px = 0; px < 3000; px++)
         {
            Assert.Equal(px * Pixel, PageFill.SnapUp(px * Pixel, Pixel), 6);
            Assert.Equal(px * 0.8, PageFill.SnapUp(px * 0.8, 0.8), 6);
         }
      }

      [Fact]
      public void NoPixelSize_LeavesTheValue()
      {
         Assert.Equal(193, PageFill.SnapUp(193, 0));
         Assert.Equal(193, PageFill.SnapUp(193, -1));
         Assert.Equal(193, PageFill.SnapUp(193, double.NaN));
         Assert.Equal(193, PageFill.SnapUp(193, double.PositiveInfinity));
         Assert.True(double.IsNaN(PageFill.SnapUp(double.NaN, Pixel)));
         Assert.True(double.IsPositiveInfinity(PageFill.SnapUp(double.PositiveInfinity, Pixel)));
      }

      [Fact]
      public void TheSuppressionList_SumsToItsViewport_WhenItsFloorsAreOnTheGrid()
      {
         const double viewport = 782 * Pixel, others = 148 * Pixel;
         double[] floors = { PageFill.SnapUp(157, Pixel), PageFill.SnapUp(193, Pixel) };
         double[] weights = { 2, 1 };

         double[] heights = PageFill.Share(viewport, others, floors, weights);
         Assert.Equal(floors[1], heights[1]);
         Assert.Equal(344 * Pixel, heights[0], 6);
         Assert.Equal(viewport, others + heights.Sum(), 6);

         // Off the grid, the counts card is given 193, asks for 290 device pixels, has
         // its floor raised to that (MeasuredFloor), and the page is a third of a pixel
         // over the viewport, which layout rounding makes a whole one and a scroll bar.
         double[] offGrid = PageFill.Share(viewport, others, new[] { 157.0, 193 }, weights);
         double raised = PageFill.MeasuredFloor(193, offGrid[1], 290 * Pixel);
         Assert.True(others + offGrid[0] + raised > viewport + 1e-6, "The floors off the grid were the defect: the page no longer passes the viewport by a fraction of a pixel.");
      }

      [Fact]
      public void FillPanel_PutsAPartsFloor_OnTheDevicePixelGrid()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "Scaffold", "FillPanel.cs"));
         string measure = WithheldRowTests.MemberText(code, "protected override Size MeasureOverride(Size availableSize)");
         string arrange = WithheldRowTests.MemberText(code, "protected override Size ArrangeOverride(Size finalSize)");
         Assert.True(measure != null && arrange != null, "FillPanel's MeasureOverride or ArrangeOverride is gone.");
         Assert.Contains("floors[i] = PageFill.SnapUp(Floor(fill), pixel);", measure, StringComparison.Ordinal);
         Assert.Contains("PageFill.SnapUp(measured ? floors_[i] : fills[i].DesiredSize.Height, pixel)", arrange, StringComparison.Ordinal);
         Assert.Contains("VisualTreeHelper.GetDpi(this).DpiScaleY", code, StringComparison.Ordinal);
      }

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
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
