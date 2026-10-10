// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A tabbed dialog on the frame is sized to its tallest tab. The three cases
   /// the arithmetic has to get right: a tall tab is given its height, a set of
   /// short tabs is still given a dialog's worth, and no tab may push the footer
   /// off a small screen.
   /// </summary>
   public class DialogLayoutTests
   {
      [Fact]
      public void TheTallestTabDecidesTheHeightWhenItFits()
      {
         double maxWindow = DialogLayout.MaxWindowHeight(1040);   // 936
         Assert.Equal(500, DialogLayout.TabControlHeight(500, maxWindow));
      }

      [Fact]
      public void ShortTabsStillGetTheFloor()
      {
         double maxWindow = DialogLayout.MaxWindowHeight(1040);
         Assert.Equal(DialogLayout.MinTabControlHeight, DialogLayout.TabControlHeight(90, maxWindow));
      }

      [Fact]
      public void ATabTallerThanTheScreenIsCappedSoTheFooterStaysOnIt()
      {
         double maxWindow = DialogLayout.MaxWindowHeight(720);   // 648
         double height = DialogLayout.TabControlHeight(1200, maxWindow);
         Assert.Equal(maxWindow - DialogLayout.FrameChrome, height);
         Assert.True(height + DialogLayout.FrameChrome <= maxWindow);
      }

      [Fact]
      public void ATinyWorkAreaNeverProducesLessThanTheFloor()
      {
         double maxWindow = DialogLayout.MaxWindowHeight(200);
         Assert.Equal(DialogLayout.MinTabControlHeight, DialogLayout.TabControlHeight(1200, maxWindow));
      }

      [Fact]
      public void TheWindowCapIsTheDialogTokenFractionOfTheWorkArea()
      {
         Assert.Equal(1000 * DesignTokens.Dialog.MaxHeightFraction, DialogLayout.MaxWindowHeight(1000));
      }

      /// <summary>
      /// A tab whose height cannot be measured, asked for as infinity, still gets
      /// the ceiling, and as a finite number: an infinite height given to a tab
      /// control is one the layout would choke on.
      /// </summary>
      [Fact]
      public void AskingForMoreThanAnyScreenGivesTheCeiling()
      {
         double maxWindow = DialogLayout.MaxWindowHeight(1040);
         double ceiling = DialogLayout.TabControlHeight(double.PositiveInfinity, maxWindow);

         Assert.Equal(maxWindow - DialogLayout.FrameChrome, ceiling);
         Assert.True(double.IsFinite(ceiling));
         Assert.Equal(ceiling, DialogLayout.TabControlHeight(1_000_000, maxWindow));
      }

      /// <summary>
      /// The domain and account dialogs are as tall as their tallest tab within a
      /// cap, never the whole height a large screen allows (review finding F194):
      /// on a work area of 1,400 pixels the ceiling is 1,060 and the tab control
      /// was given all of it, General's three fields above 780 pixels of nothing.
      /// </summary>
      [Fact]
      public void ManyTabsAreCappedWhateverTheScreenAllows()
      {
         double large = DialogLayout.MaxWindowHeight(1400);
         Assert.Equal(DialogLayout.ManyTabsCap, DialogLayout.TabControlHeight(1100, large, DialogLayout.ListTabFloor, DialogLayout.ManyTabsCap));
         Assert.True(DialogLayout.ManyTabsCap < DialogLayout.TabControlHeight(1100, large));
      }

      [Fact]
      public void ManyShortTabsAreAsTallAsTheTallestOrTheListFloor()
      {
         double large = DialogLayout.MaxWindowHeight(1400);
         Assert.Equal(500, DialogLayout.TabControlHeight(500, large, DialogLayout.ListTabFloor, DialogLayout.ManyTabsCap));
         Assert.Equal(DialogLayout.ListTabFloor, DialogLayout.TabControlHeight(200, large, DialogLayout.ListTabFloor, DialogLayout.ManyTabsCap));
      }

      [Fact]
      public void ManyTabsOnASmallScreenStillKeepTheFooterOnIt()
      {
         double small = DialogLayout.MaxWindowHeight(720);   // 648
         double height = DialogLayout.TabControlHeight(1100, small, DialogLayout.ListTabFloor, DialogLayout.ManyTabsCap);
         Assert.Equal(small - DialogLayout.FrameChrome, height);
      }

      /// <summary>
      /// A dialog of a few tabs is as wide as its headers need, so that none is
      /// cut behind a chevron in German (review finding F193), and no wider than
      /// the dialog token allows.
      /// </summary>
      [Fact]
      public void ADialogIsWideEnoughForItsHeaders()
      {
         Assert.Equal(560, DialogLayout.WidthForStrip(400, 560));
         Assert.Equal(System.Math.Ceiling(540 + DialogLayout.StripSlack + 2 * DesignTokens.Space.Xl), DialogLayout.WidthForStrip(540, 560));
         Assert.Equal(DesignTokens.Dialog.MaxWidth, DialogLayout.WidthForStrip(2000, 560));
      }
   }
}
