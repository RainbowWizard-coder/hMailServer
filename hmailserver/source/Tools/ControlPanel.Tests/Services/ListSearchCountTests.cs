// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// What a list page's count and empty state say while its search filters the
   /// rows it read (Services/ListSearchCount; docs/ControlPanelDesign.md, List
   /// pages; review findings F288 and F210): "Shown: 2 of 40" while a search is
   /// typed in over a list that has entries, the list's own count otherwise, and
   /// the no-match sentence where the search hides every entry. The routes page,
   /// among others, said "2 routes" beside a blank card while a search hid both.
   /// </summary>
   [Collection("Loc")]
   public class ListSearchCountTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      [Theory]
      [InlineData("rou", 1, 2, "Shown: 1 of 2")]
      [InlineData("qqzz", 0, 2, "Shown: 0 of 2")]
      // A search that every entry matches still says it is filtering.
      [InlineData("e", 2, 2, "Shown: 2 of 2")]
      // No search, or one of spaces alone, which ListSearch reads as none.
      [InlineData("", 2, 2, "2 routes")]
      [InlineData("   ", 2, 2, "2 routes")]
      [InlineData(null, 2, 2, "2 routes")]
      // A search over an empty list: the list's own count beside its empty state.
      [InlineData("rou", 0, 0, "0 routes")]
      public void ThePill_CountsWhatTheSearchShows_WhileOneIsTypedIn(string query, int shown, int total, string text)
      {
         Assert.Equal(text, ListSearchCount.PillText(query, shown, total, total + " routes"));
      }

      [Theory]
      [InlineData("qqzz", 0, 2, true)]
      [InlineData("rou", 1, 2, false)]
      [InlineData("", 0, 2, false)]
      // An empty list says it is empty, with the way to add the first entry, whatever is typed.
      [InlineData("qqzz", 0, 0, false)]
      public void TheEmptyState_SaysNothingMatches_OnlyWhereTheSearchHidesEveryEntry(string query, int shown, int total, bool nothing)
      {
         Assert.Equal(nothing, ListSearchCount.NothingMatches(query, shown, total));
      }

      [Fact]
      public void TheNoMatchSentence_NamesWhatWasTyped_Trimmed()
      {
         Assert.Equal("No entry in this list has \"qqzz\" in it.", ListSearchCount.NothingMatchesText("  qqzz "));
      }
   }
}
