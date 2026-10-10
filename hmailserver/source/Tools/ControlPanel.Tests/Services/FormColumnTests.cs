// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The width of a page's column of cards (Views/Scaffold/FormColumnPanel; review finding
   /// F295): a StackPanel aligned left with a MaxWidth of 760 was as wide as its widest
   /// card's content, so that the cards of Transport security's Encryption tab ended at 761
   /// pixels and those of the SSL/TLS page's Ciphers tab at 526.
   /// </summary>
   public class FormColumnTests
   {
      /// <summary>Narrow cards stand in a column as wide as the page allows: every tab's cards end at one edge.</summary>
      [Theory]
      [InlineData(526, 760)]
      [InlineData(761, 760)]
      [InlineData(0, 760)]
      [InlineData(300, 520)]
      public void TheColumn_IsAsWideAsItIsAllowed(double content, double allowed)
         => Assert.Equal(System.Math.Max(content, allowed), FormColumn.Width(content, allowed));

      /// <summary>The two tabs of the finding, side by side: one edge.</summary>
      [Fact]
      public void TwoTabsOfDifferentContent_EndAtOneEdge()
         => Assert.Equal(FormColumn.Width(526, DesignTokens.FormWidth), FormColumn.Width(744, DesignTokens.FormWidth));

      /// <summary>Content wider than the column - a box with a MinWidth - is not cut: the column is its width, as a StackPanel's is.</summary>
      [Fact]
      public void ContentWiderThanTheColumn_KeepsItsWidth()
         => Assert.Equal(812, FormColumn.Width(812, 760));

      /// <summary>With no limit - in a scroll viewer that scrolls sideways - the column is its content's width, never infinite.</summary>
      [Theory]
      [InlineData(double.PositiveInfinity)]
      [InlineData(double.NaN)]
      public void WithNoLimit_TheColumnIsItsContentsWidth(double allowed)
         => Assert.Equal(526, FormColumn.Width(526, allowed));
   }
}
