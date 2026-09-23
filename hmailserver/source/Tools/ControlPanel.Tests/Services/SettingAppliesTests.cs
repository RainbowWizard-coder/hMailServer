// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The words beside a field kept in the settings store, and the pending bar's
   /// sentence (docs/SettingsApply.md). The server decides the mark; these hold
   /// the page to saying what the mark means, and to saying nothing for a mark it
   /// does not know rather than a guess.
   /// </summary>
   public class SettingAppliesTests
   {
      [Fact]
      public void EachMarkHasItsCaption()
      {
         Assert.Equal("Applies on save", SettingApplies.Caption("now"));
         Assert.Equal("After reinitialize", SettingApplies.Caption("reinitialize"));
         Assert.Equal("After a restart", SettingApplies.Caption("restart"));
      }

      [Fact]
      public void TheCaptionIsReadAsTheServerMightSpellIt()
      {
         Assert.Equal("Applies on save", SettingApplies.Caption(" NOW "));
         Assert.Equal("After reinitialize", SettingApplies.Caption("Reinitialize"));
      }

      [Fact]
      public void AMarkThePageDoesNotKnowShowsNothing()
      {
         // An older server answers nothing; a newer one may add a word. Either way
         // the field shows no mark rather than a wrong one - the negative control
         // for the three above.
         Assert.Null(SettingApplies.Caption(null));
         Assert.Null(SettingApplies.Caption(""));
         Assert.Null(SettingApplies.Caption("soon"));
         Assert.Null(SettingApplies.Caption("applies after a service restart"));
      }

      [Fact]
      public void ThePendingListIsReadOnePerLine()
      {
         Assert.Empty(SettingApplies.ParsePending(null));
         Assert.Empty(SettingApplies.ParsePending(""));
         Assert.Equal(new[] { "RestApiPort", "ManageSieveServerPort" },
            SettingApplies.ParsePending("RestApiPort\r\nManageSieveServerPort\r\n"));
         Assert.Equal(new[] { "RestApiPort" }, SettingApplies.ParsePending(" RestApiPort \n\n"));
      }

      [Fact]
      public void TheBannerSaysWhatWaitsAndIsAbsentWhenNothingDoes()
      {
         Assert.Null(SettingApplies.Banner(0, 0));

         string reinitialize = SettingApplies.Banner(2, 0);
         Assert.Contains("2", reinitialize);
         Assert.Contains("reinitialize", reinitialize);
         Assert.DoesNotContain("restart", reinitialize);

         string restart = SettingApplies.Banner(0, 1);
         Assert.Contains("1", restart);
         Assert.Contains("restart", restart);
         Assert.DoesNotContain("reinitialize", restart);

         string both = SettingApplies.Banner(3, 1);
         Assert.Contains("3", both);
         Assert.Contains("1", both);
         Assert.Contains("reinitialize", both);
         Assert.Contains("restart", both);
      }

      [Fact]
      public void ApplyNowSaysWhenItEndsADrain()
      {
         Assert.Contains("maintenance mode", SettingApplies.ApplyNowQuestion(draining: true));
         Assert.DoesNotContain("maintenance mode", SettingApplies.ApplyNowQuestion(draining: false));
         Assert.Contains("Reinitialize", SettingApplies.ApplyNowQuestion(draining: false));
      }
   }
}
