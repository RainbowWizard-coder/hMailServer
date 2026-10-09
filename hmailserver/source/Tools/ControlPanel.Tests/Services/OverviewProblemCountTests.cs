// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The count beside an overview's title says "No problems found" only where
   /// every value was read. A partial read leaves out the notes that follow from
   /// what could not be read, and the count said "No problems found", in green,
   /// beside "Some values could not be read: ... The sections below may be
   /// incomplete." and "Where this server stands cannot be said: the listeners
   /// could not be read." on the transport encryption overview, and beside "What
   /// happens to a message cannot be said: the thresholds could not be read." on
   /// the spam filtering overview.
   /// </summary>
   public class OverviewProblemCountTests
   {
      [Fact]
      public void NoProblemFound_IsSaid_OnlyWhenEveryValueWasRead()
      {
         Assert.Equal((StatusLevel.Good, "No problems found"), OverviewProblemCount.Of(0, StatusLevel.Good, everythingRead: true));
         Assert.Null(OverviewProblemCount.Of(0, StatusLevel.Good, everythingRead: false));
      }

      /// <summary>A problem that was found is counted whatever else could not be read: it is there.</summary>
      [Fact]
      public void TheProblemsFound_AreCounted_AtTheWorstOnesLevel_WhateverWasRead()
      {
         foreach (bool everythingRead in new[] { true, false })
         {
            Assert.Equal((StatusLevel.Warning, "1 problem"), OverviewProblemCount.Of(1, StatusLevel.Warning, everythingRead));
            Assert.Equal((StatusLevel.Critical, "3 problems"), OverviewProblemCount.Of(3, StatusLevel.Critical, everythingRead));
         }
      }
   }
}
