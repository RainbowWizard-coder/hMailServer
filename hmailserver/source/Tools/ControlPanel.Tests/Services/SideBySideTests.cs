// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Parts laid side by side keep their minimum widths, or go one above the other
   /// (<see cref="SideBySide"/>). The widths are the pages' own: the domains page's
   /// columns share its width two to three and need 280 and 400 pixels, its alias
   /// and list cards half each and 270, and the rules page's criteria and actions
   /// half each and 340. The page is 512 pixels wide in the narrowest window
   /// (760 by 520), 728 in a 1024 by 768 one and 922 in a 1280 by 800 one, and
   /// the rules page in the account dialog's Rules tab 544.
   /// </summary>
   public class SideBySideTests
   {
      private static readonly double[] DomainColumnWeights = { 2, 3 };
      private static readonly double[] DomainColumnMinWidths = { 280, 400 };
      private static readonly double[] Halves = { 1, 1 };

      [Theory]
      [InlineData(512, false)]
      [InlineData(699, false)]
      [InlineData(700, true)]
      [InlineData(752, true)]
      [InlineData(952, true)]
      [InlineData(1118, true)]
      public void TheDomainsPage_HasTwoColumns_WhereBothKeepTheirWidths(double width, bool twoColumns)
         => Assert.Equal(twoColumns, SideBySide.Fits(width, DomainColumnWeights, DomainColumnMinWidths));

      /// <summary>
      /// The narrowest window's 512 pixels gave the alias and list cards 104 and 116
      /// side by side beside the domains column; in one column they would have 256 each,
      /// still short of a list row's three buttons and an address, so they go one
      /// above the other. In a 1280 by 800 window the column is 546 wide beside the
      /// sidebar's 310 pixels and they stay side by side, 273 each.
      /// </summary>
      [Theory]
      [InlineData(232, false)]
      [InlineData(512, false)]
      [InlineData(539, false)]
      [InlineData(540, true)]
      [InlineData(546, true)]
      public void TheAliasAndListCards_AreSideBySide_WhereEachKeeps270(double width, bool sideBySide)
         => Assert.Equal(sideBySide, SideBySide.Fits(width, Halves, new double[] { 270, 270 }));

      [Theory]
      [InlineData(512, false)]
      [InlineData(544, false)]
      [InlineData(680, true)]
      [InlineData(952, true)]
      public void TheRuleCriteriaAndActions_AreSideBySide_WhereEachKeeps340(double width, bool sideBySide)
         => Assert.Equal(sideBySide, SideBySide.Fits(width, Halves, new double[] { 340, 340 }));

      [Theory]
      [InlineData(0)]
      [InlineData(-1)]
      [InlineData(double.NaN)]
      [InlineData(double.PositiveInfinity)]
      public void AWidthNotKnownYet_FitsNothing(double width)
         => Assert.False(SideBySide.Fits(width, Halves, new double[] { 1, 1 }));

      [Fact]
      public void PartsWithNoWeight_FitNothing_AndEveryPartNeedsAMinimum()
      {
         Assert.False(SideBySide.Fits(1000, new double[] { 0, 0 }, new double[] { 10, 10 }));
         Assert.Throws<ArgumentException>(() => SideBySide.Fits(1000, Halves, new double[] { 10 }));
         Assert.Throws<ArgumentNullException>(() => SideBySide.Fits(1000, null, new double[] { 10 }));
      }
   }
}
