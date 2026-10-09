// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The pill in a settings page's header is the one thing on the page that
   /// says whether what is on screen has reached the server. These hold each
   /// state that shows a pill to a distinct word and to the severity an
   /// administrator would give it, and the two sentences the notice carries to
   /// the facts they have to state.
   /// </summary>
   public class ServerSettingsPageStatusTests
   {
      [Fact]
      public void EveryStateIsListedAndEveryShownStateHasADistinctWord()
      {
         Assert.Equal(Enum.GetValues<ServerSettingsPageState>().Length, ServerSettingsPageStatus.AllStates.Count);

         foreach (ServerSettingsPageState state in Enum.GetValues<ServerSettingsPageState>())
            Assert.Contains(state, ServerSettingsPageStatus.AllStates);

         var shown = ServerSettingsPageStatus.AllStates.Where(ServerSettingsPageStatus.ShowsPill).ToList();
         foreach (ServerSettingsPageState state in shown)
            Assert.False(string.IsNullOrWhiteSpace(ServerSettingsPageStatus.WordFor(state)), state + " has no word.");

         Assert.Equal(shown.Count, shown.Select(ServerSettingsPageStatus.WordFor).Distinct(StringComparer.Ordinal).Count());
      }

      [Fact]
      public void OnlyACleanPageHidesThePill()
      {
         Assert.False(ServerSettingsPageStatus.ShowsPill(ServerSettingsPageState.Clean));
         Assert.Equal("", ServerSettingsPageStatus.WordFor(ServerSettingsPageState.Clean));

         foreach (ServerSettingsPageState state in ServerSettingsPageStatus.AllStates.Where(s => s != ServerSettingsPageState.Clean))
            Assert.True(ServerSettingsPageStatus.ShowsPill(state), state + " shows no pill.");
      }

      [Fact]
      public void TheLevelsMatchWhatEachStateMeansToAnAdministrator()
      {
         // Nothing to say.
         Assert.Equal(StatusLevel.Normal, ServerSettingsPageStatus.LevelFor(ServerSettingsPageState.Clean));
         // Leaving the page loses the change.
         Assert.Equal(StatusLevel.Warning, ServerSettingsPageStatus.LevelFor(ServerSettingsPageState.Unsaved));
         // Nothing to do but wait.
         Assert.Equal(StatusLevel.Information, ServerSettingsPageStatus.LevelFor(ServerSettingsPageState.Saving));
         Assert.Equal(StatusLevel.Good, ServerSettingsPageStatus.LevelFor(ServerSettingsPageState.Saved));
         // What is on screen is not what the server holds.
         Assert.Equal(StatusLevel.Critical, ServerSettingsPageStatus.LevelFor(ServerSettingsPageState.PartlySaved));
      }

      [Fact]
      public void OnlyAWhollySavedPageIsDrawnAsGood()
      {
         foreach (ServerSettingsPageState state in ServerSettingsPageStatus.AllStates.Where(s => s != ServerSettingsPageState.Saved))
            Assert.NotEqual(StatusLevel.Good, ServerSettingsPageStatus.LevelFor(state));
      }

      [Fact]
      public void AReadWithNoFailureHasNothingToSay()
      {
         Assert.Null(ServerSettingsPageStatus.ReadSentence(0, null));
         Assert.Null(ServerSettingsPageStatus.ReadSentence(0, "irrelevant"));

         string sentence = ServerSettingsPageStatus.ReadSentence(3, "The RPC server is unavailable.");
         Assert.Contains("3", sentence);
         Assert.Contains("The RPC server is unavailable.", sentence);
      }

      [Fact]
      public void TheSaveSentenceCountsTheWritesAndPromisesOnlyWhatApplied()
      {
         // Nothing the server lists as pending: everything applied, and no
         // restart is promised or asked for - a save publishes.
         string all = ServerSettingsPageStatus.SaveSentence(18, 0, "14:20:00", waiting: 0);
         Assert.Contains("18", all);
         Assert.Contains("14:20:00", all);
         Assert.DoesNotContain("restart", all);
         Assert.DoesNotContain("reinitialis", all);

         // Keys the server marks reinitialize or restart and lists as pending: the
         // sentence says how many wait, and must not claim they applied.
         string waiting = ServerSettingsPageStatus.SaveSentence(18, 0, "14:20:00", waiting: 2);
         Assert.Contains("2 of them wait", waiting);
         Assert.Contains("reinitialis", waiting);

         string partial = ServerSettingsPageStatus.SaveSentence(16, 2, "14:20:00", waiting: 1);
         Assert.Contains("16", partial);
         Assert.Contains("2", partial);
         Assert.DoesNotContain("14:20:00", partial);

         // A server that cannot say which keys wait - one from before a save could
         // publish - is not told they applied: the sentence it was told then.
         string older = ServerSettingsPageStatus.SaveSentence(18, 0, "14:20:00", waiting: null);
         Assert.Contains("hMailServer.ini settings apply after a service restart", older);
         Assert.DoesNotContain(" - applied immediately.", older);
      }
   }
}
