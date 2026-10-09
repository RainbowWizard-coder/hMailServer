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
         Assert.Equal("After reinitialise", SettingApplies.Caption("reinitialize"));
         Assert.Equal("After a restart", SettingApplies.Caption("restart"));
      }

      [Fact]
      public void TheCaptionIsReadAsTheServerMightSpellIt()
      {
         Assert.Equal("Applies on save", SettingApplies.Caption(" NOW "));
         Assert.Equal("After reinitialise", SettingApplies.Caption("Reinitialize"));
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
         Assert.Contains("reinitialise", reinitialize);
         Assert.DoesNotContain("restart", reinitialize);

         string restart = SettingApplies.Banner(0, 1);
         Assert.Contains("1", restart);
         Assert.Contains("restart", restart);
         Assert.DoesNotContain("reinitialise", restart);

         string both = SettingApplies.Banner(3, 1);
         Assert.Contains("3", both);
         Assert.Contains("1", both);
         Assert.Contains("reinitialise", both);
         Assert.Contains("restart", both);
      }

      /// <summary>
      /// One waiting setting is said in the singular and several in the plural
      /// (Loc.Plural), where the bar said "1 saved setting(s) wait" for both, and
      /// the server's verb is spelt the British way in its sentence (review
      /// finding F036).
      /// </summary>
      [Fact]
      public void TheBannerCountsInTheSingularAndThePlural()
      {
         Assert.Equal("1 saved setting waits for a reinitialise.", SettingApplies.Banner(1, 0));
         Assert.Equal("2 saved settings wait for a reinitialise.", SettingApplies.Banner(2, 0));
         Assert.Equal("1 saved setting waits for a service restart.", SettingApplies.Banner(0, 1));
         Assert.Equal("1 saved setting waits for a reinitialise, and 3 for a service restart.", SettingApplies.Banner(1, 3));
         Assert.DoesNotContain("(s)", SettingApplies.Banner(5, 5));
      }

      [Fact]
      public void TheMarksAreReadFromOneObject()
      {
         var marks = SettingApplies.ParseMarks("{\"RestApiPort\":\"reinitialize\",\"LogLevel\":\"now\"}");
         Assert.Equal("reinitialize", marks["restapiport"]);
         Assert.Equal("now", marks["LogLevel"]);
         Assert.Empty(SettingApplies.ParseMarks(null));
         Assert.Empty(SettingApplies.ParseMarks(" "));

         // Not an object: a failed read, which the caller does not remember.
         Assert.ThrowsAny<System.Text.Json.JsonException>(() => SettingApplies.ParseMarks("[\"RestApiPort\"]"));
      }

      [Fact]
      public void AnOlderServerIsAMissingMemberAndAMomentsFailureIsNot()
      {
         Assert.True(SettingApplies.IsMissingMember(new System.Runtime.InteropServices.COMException("Unknown name.", unchecked((int)0x80020006))));
         Assert.True(SettingApplies.IsMissingMember(new System.Runtime.InteropServices.COMException("Member not found.", unchecked((int)0x80020003))));
         Assert.True(SettingApplies.IsMissingMember(new System.Reflection.TargetInvocationException(new System.MissingMemberException("Settings", "IniSettingAppliesAll"))));

         // The negative controls: the server is there and did not answer this time.
         Assert.False(SettingApplies.IsMissingMember(new System.Runtime.InteropServices.COMException("The RPC server is unavailable.", unchecked((int)0x800706BA))));
         Assert.False(SettingApplies.IsMissingMember(new System.InvalidOperationException("a moment's failure")));
         Assert.False(SettingApplies.IsMissingMember(null));
      }

      [Fact]
      public void ApplyNowSaysWhatARestartStillOwes()
      {
         string all = SettingApplies.AppliedSentence(0);
         Assert.Contains("in effect", all);
         Assert.DoesNotContain("restart", all);

         // A key marked restart is not taken by a reinitialise, and the sentence
         // does not say the saved settings are in effect while it waits.
         string some = SettingApplies.AppliedSentence(2);
         Assert.Contains("2", some);
         Assert.Contains("restart", some);
         Assert.DoesNotContain("the saved settings are in effect", some);
      }

      [Fact]
      public void ApplyNowSaysWhenItEndsADrain()
      {
         Assert.Contains("maintenance mode", SettingApplies.ApplyNowQuestion(draining: true));
         Assert.DoesNotContain("maintenance mode", SettingApplies.ApplyNowQuestion(draining: false));
         Assert.Contains("Reinitialise", SettingApplies.ApplyNowQuestion(draining: false));
      }
   }
}
