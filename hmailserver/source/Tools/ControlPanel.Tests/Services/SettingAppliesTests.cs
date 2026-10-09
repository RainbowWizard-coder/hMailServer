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

         // What Apply now takes, and that it takes it (review finding F010).
         Assert.Equal("2 saved settings wait for a reinitialise - Apply now takes them.", SettingApplies.Banner(2, 0));
         Assert.Equal("1 saved setting waits for a reinitialise - Apply now takes it.", SettingApplies.Banner(1, 0));

         // A restart alone, which Apply now does not take, and which the bar's Restart
         // the service does.
         Assert.Equal("1 saved setting waits for a service restart.", SettingApplies.Banner(0, 1));
         Assert.Equal("4 saved settings wait for a service restart.", SettingApplies.Banner(0, 4));

         // Both, in two sentences, each counting its own way.
         Assert.Equal("3 saved settings wait for a reinitialise - Apply now takes them. 1 waits for a service restart.", SettingApplies.Banner(3, 1));
         Assert.Equal("1 saved setting waits for a reinitialise - Apply now takes it. 2 wait for a service restart.", SettingApplies.Banner(1, 2));
         Assert.Equal("1 saved setting waits for a reinitialise - Apply now takes it. 1 waits for a service restart.", SettingApplies.Banner(1, 1));
         Assert.Equal("2 saved settings wait for a reinitialise - Apply now takes them. 5 wait for a service restart.", SettingApplies.Banner(2, 5));
      }

      [Fact]
      public void NoBannerPutsAnEnglishPluralInBrackets()
      {
         foreach ((int reinitialise, int restart) in new[] { (1, 0), (2, 0), (0, 1), (0, 2), (1, 1), (1, 2), (2, 1), (2, 2) })
            Assert.DoesNotContain("(s)", SettingApplies.Banner(reinitialise, restart), System.StringComparison.Ordinal);
      }

      [Fact]
      public void TheRestartQuestionSaysHowManyWaitAndWhatARestartDoes()
      {
         string one = SettingApplies.RestartQuestion(1);
         Assert.StartsWith("1 saved setting takes effect only when the hMailServer service restarts.", one, System.StringComparison.Ordinal);
         Assert.Contains("disconnected", one, System.StringComparison.Ordinal);

         string three = SettingApplies.RestartQuestion(3);
         Assert.StartsWith("3 saved settings take effect only when the hMailServer service restarts.", three, System.StringComparison.Ordinal);
         Assert.Contains("Restart the service now?", three, System.StringComparison.Ordinal);
      }

      [Fact]
      public void ThePendingDetailNamesEachSettingUnderWhatItWaitsFor()
      {
         var marks = new System.Collections.Generic.Dictionary<string, string>(System.StringComparer.OrdinalIgnoreCase)
         {
            ["AcmeEnabled"] = "reinitialize",
            ["RestApiPort"] = "restart",
            ["NoSuchKey"] = "restart",
            ["LogLevel"] = "now"
         };
         string detail = SettingApplies.PendingDetail(new[] { "AcmeEnabled", "RestApiPort", "NoSuchKey", "LogLevel", "Unmarked" },
            key => marks.TryGetValue(key, out string mark) ? mark : null);
         string[] lines = detail.Split((char)10);

         // The caption and the page, as the Ctrl+K palette finds the setting.
         Assert.Equal("Waiting for a reinitialise:", lines[0]);
         Assert.Equal("Issue and renew certificates automatically, on the Automatic certificates (ACME) page", lines[1]);
         Assert.Equal("Waiting for a service restart:", lines[2]);
         Assert.StartsWith(SettingApplies.DescribePending("RestApiPort"), lines[3], System.StringComparison.Ordinal);
         Assert.Contains(", on the API & monitoring page", lines[3], System.StringComparison.Ordinal);
         // A key no page shows is named as it is; a key marked now, or not at all, is
         // not one the bar counts.
         Assert.Equal("NoSuchKey", lines[4]);
         Assert.Equal(5, lines.Length);

         Assert.Null(SettingApplies.PendingDetail(new[] { "LogLevel" }, key => "now"));
         Assert.Null(SettingApplies.PendingDetail(null, key => "restart"));
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
         Assert.Equal("1 saved setting waits for a reinitialise - Apply now takes it.", SettingApplies.Banner(1, 0));
         Assert.Equal("2 saved settings wait for a reinitialise - Apply now takes them.", SettingApplies.Banner(2, 0));
         Assert.Equal("1 saved setting waits for a service restart.", SettingApplies.Banner(0, 1));
         Assert.Equal("1 saved setting waits for a reinitialise - Apply now takes it. 3 wait for a service restart.", SettingApplies.Banner(1, 3));
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
