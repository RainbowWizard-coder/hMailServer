// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Update row of the Status page (UpdateStatusText) for each of the eight
   /// states, the administrator's install's two among them, with and without the
   /// last apply's outcome in front.
   /// </summary>
   [Collection("Loc")]
   public class UpdateStatusTextTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      // The feed's day and the last check's time are the reader's (ServerTimeText);
      // the test's culture is the machine's, so the expected text is built the same way.
      [Theory]
      [InlineData(0, "Not checked yet")]
      [InlineData(1, "This is the latest release, checked {checked}")]
      [InlineData(2, "6.3.7 is available (published {published})")]
      [InlineData(3, "6.3.7 is downloaded and verified (published {published}); Install update applies it")]
      [InlineData(4, "Installing 6.3.7: the service will stop and start")]
      [InlineData(5, "The last update step failed: the feed is down")]
      [InlineData(6, "Preparing the install of 6.3.7: taking the backup")]
      [InlineData(7, "6.3.7 is ready to be installed; the install goes on in its window")]
      public void EachStateHasItsLine(int state, string expected)
      {
         string text = UpdateStatusText.For(state, "6.3.7", "2026-10-19", "2026-10-20 10:00", "the feed is down", null, "backup");
         expected = expected.Replace("{checked}", ServerTimeText.Marked(new DateTime(2026, 10, 20, 10, 0, 0)), StringComparison.Ordinal)
            .Replace("{published}", ServerTimeText.Date(new DateTime(2026, 10, 19)), StringComparison.Ordinal);
         Assert.StartsWith(expected, text);
      }

      [Fact]
      public void TheLastOutcomeComesFirst()
      {
         string text = UpdateStatusText.For(1, "", "", "", "", "ok 6.3.7: the installer succeeded and the service is running (installed by HOST\\admin)");
         Assert.StartsWith("Last update: ok 6.3.7: the installer succeeded", text);
         Assert.Contains("This is the latest release", text);
      }

      [Theory]
      [InlineData("check", "checking the downloaded installer")]
      [InlineData("backup", "taking the backup")]
      [InlineData("rollback", "fetching the version to go back to")]
      [InlineData("ready", "ready")]
      [InlineData(null, "checking the downloaded installer")]
      [InlineData("something newer", "checking the downloaded installer")]
      public void EachPreparationStepHasItsWords(string step, string expected)
      {
         Assert.Equal(expected, UpdateStatusText.PreparationStep(step));
      }
   }
}
