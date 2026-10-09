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
   /// The pill in an hMailServer.INI settings page's header is what says whether the
   /// values on screen are the ones the service runs. These hold each state that shows
   /// a pill to a distinct word and to the severity an administrator would give it, so
   /// a failed restart is never drawn in the colour of an applied one; and they hold
   /// the INI pages and the server settings page to one save-state model (review
   /// findings F120 and F218): no pill at rest, Unsaved changes on the first edit, and
   /// Saved, Reinitialise to apply or Restart to apply after a save, in the same words.
   /// </summary>
   public class SettingsPageStatusTests
   {
      [Fact]
      public void EveryStateIsListedAndEveryShownStateHasADistinctWord()
      {
         Assert.Equal(Enum.GetValues<SettingsPageState>().Length, SettingsPageStatus.AllStates.Count);

         foreach (SettingsPageState state in Enum.GetValues<SettingsPageState>())
            Assert.Contains(state, SettingsPageStatus.AllStates);

         var shown = SettingsPageStatus.AllStates.Where(SettingsPageStatus.ShowsPill).ToList();
         foreach (SettingsPageState state in shown)
            Assert.False(string.IsNullOrWhiteSpace(SettingsPageStatus.WordFor(state)), state + " has no word.");

         Assert.Equal(shown.Count, shown.Select(SettingsPageStatus.WordFor).Distinct(StringComparer.Ordinal).Count());
      }

      [Fact]
      public void APageThatShowsWhatItReadHasNoPill()
      {
         // The grey "Editing" pill an INI page wore as it opened, with nothing changed,
         // is gone: the line under the header names the file.
         Assert.False(SettingsPageStatus.ShowsPill(SettingsPageState.Clean));
         Assert.Equal("", SettingsPageStatus.WordFor(SettingsPageState.Clean));
         Assert.DoesNotContain(SettingsPageStatus.AllStates, state => SettingsPageStatus.WordFor(state) == "Editing");

         foreach (SettingsPageState state in SettingsPageStatus.AllStates.Where(s => s != SettingsPageState.Clean))
            Assert.True(SettingsPageStatus.ShowsPill(state), state + " shows no pill.");
      }

      [Fact]
      public void TheLevelsMatchWhatEachStateMeansToAnAdministrator()
      {
         Assert.Equal(StatusLevel.Normal, SettingsPageStatus.LevelFor(SettingsPageState.Clean));
         // Leaving the page or reloading it loses the change.
         Assert.Equal(StatusLevel.Warning, SettingsPageStatus.LevelFor(SettingsPageState.Unsaved));
         Assert.Equal(StatusLevel.Good, SettingsPageStatus.LevelFor(SettingsPageState.Saved));
         Assert.Equal(StatusLevel.Good, SettingsPageStatus.LevelFor(SettingsPageState.Applied));
         // Written, with a step left to take when it suits: never Good, never a fault.
         Assert.Equal(StatusLevel.Information, SettingsPageStatus.LevelFor(SettingsPageState.WaitsForReinitialise));
         Assert.Equal(StatusLevel.Information, SettingsPageStatus.LevelFor(SettingsPageState.WaitsForRestart));
         // The values on screen are not yet the server's.
         Assert.Equal(StatusLevel.Warning, SettingsPageStatus.LevelFor(SettingsPageState.Restarting));
         // A server that answered could not give the values, and the file's are shown.
         Assert.Equal(StatusLevel.Warning, SettingsPageStatus.LevelFor(SettingsPageState.ReadFromFile));
         // The link to the service has gone: Critical, as the shell's pill says it.
         Assert.Equal(StatusLevel.Critical, SettingsPageStatus.LevelFor(SettingsPageState.NotConnected));
         // The file and the running service disagree, or there is no file at all.
         Assert.Equal(StatusLevel.Critical, SettingsPageStatus.LevelFor(SettingsPageState.RestartFailed));
         Assert.Equal(StatusLevel.Critical, SettingsPageStatus.LevelFor(SettingsPageState.Unavailable));
      }

      [Fact]
      public void OnlyASaveInEffectIsDrawnAsGood()
      {
         // "Saved" turned green after a save whose settings waited for a restart: the
         // service had not taken them, and the pill said it had.
         foreach (SettingsPageState state in SettingsPageStatus.AllStates
                     .Where(s => s != SettingsPageState.Saved && s != SettingsPageState.Applied))
            Assert.NotEqual(StatusLevel.Good, SettingsPageStatus.LevelFor(state));
      }

      [Theory]
      [InlineData(SettingsPageState.Clean)]
      [InlineData(SettingsPageState.Saved)]
      [InlineData(SettingsPageState.WaitsForReinitialise)]
      [InlineData(SettingsPageState.WaitsForRestart)]
      [InlineData(SettingsPageState.Applied)]
      [InlineData(SettingsPageState.RestartFailed)]
      public void ThePillFollowsTheEditors(SettingsPageState settled)
      {
         // The first edit replaces whatever the last read or save said, and an edit
         // undone by hand takes the pill back to it: the pill is whether the editors
         // differ, as the question before leaving the page is.
         Assert.Equal(SettingsPageState.Unsaved, SettingsPageStatus.Follow(settled, hasUnsavedChanges: true));
         Assert.Equal(settled, SettingsPageStatus.Follow(settled, hasUnsavedChanges: false));
      }

      [Fact]
      public void ASaveSettlesInWhatStillWaits()
      {
         Assert.Equal(SettingsPageState.Saved, SettingsPageStatus.AfterSave(0, 0));
         Assert.Equal(SettingsPageState.WaitsForReinitialise, SettingsPageStatus.AfterSave(2, 0));
         Assert.Equal(SettingsPageState.WaitsForRestart, SettingsPageStatus.AfterSave(3, 1));
         // A server that cannot say applies its settings at its next start.
         Assert.Equal(SettingsPageState.WaitsForRestart, SettingsPageStatus.AfterSave(null, 0));
      }

      [Fact]
      public void TheServerSettingsPageSettlesTheSameWay()
      {
         Assert.Equal(ServerSettingsPageState.Saved, ServerSettingsPageStatus.AfterSave(0, 0, 0));
         Assert.Equal(ServerSettingsPageState.WaitsForReinitialise, ServerSettingsPageStatus.AfterSave(0, 2, 0));
         Assert.Equal(ServerSettingsPageState.WaitsForRestart, ServerSettingsPageStatus.AfterSave(0, 3, 1));
         Assert.Equal(ServerSettingsPageState.WaitsForRestart, ServerSettingsPageStatus.AfterSave(0, null, 0));
         // A failed write outranks what waits: the reader has rows to find.
         Assert.Equal(ServerSettingsPageState.PartlySaved, ServerSettingsPageStatus.AfterSave(1, 0, 0));
         Assert.Equal(ServerSettingsPageState.PartlySaved, ServerSettingsPageStatus.AfterSave(1, null, 2));
      }

      [Theory]
      [InlineData(SettingsPageState.Clean, ServerSettingsPageState.Clean)]
      [InlineData(SettingsPageState.Unsaved, ServerSettingsPageState.Unsaved)]
      [InlineData(SettingsPageState.Saved, ServerSettingsPageState.Saved)]
      [InlineData(SettingsPageState.WaitsForReinitialise, ServerSettingsPageState.WaitsForReinitialise)]
      [InlineData(SettingsPageState.WaitsForRestart, ServerSettingsPageState.WaitsForRestart)]
      public void EverySettingsPageSaysAStateTheyShareInOneWord(SettingsPageState ini, ServerSettingsPageState server)
      {
         Assert.Equal(ServerSettingsPageStatus.WordFor(server), SettingsPageStatus.WordFor(ini));
         Assert.Equal(ServerSettingsPageStatus.LevelFor(server), SettingsPageStatus.LevelFor(ini));
         Assert.Equal(ServerSettingsPageStatus.ShowsPill(server), SettingsPageStatus.ShowsPill(ini));
      }

      /// <summary>
      /// A page whose read fell back to hMailServer.ini says so in its pill at the
      /// level of the read's cause, the level of the notice beside it: a lost link is
      /// the shell's Connection lost at Critical, and a server that answered - one
      /// without GetIniSetting - is Read from file at Warning, and is never said to be
      /// not connected. Before, the pill was Not connected at Warning whatever the
      /// cause, beside a Critical notice about the same lost link, and said Not
      /// connected of a server the shell said was Connected.
      /// </summary>
      [Fact]
      public void AFallBack_IsSaidAtTheLevelOfItsCause_InAWordTrueOfIt()
      {
         var lost = new System.Runtime.InteropServices.COMException("The RPC server is unavailable.", unchecked((int)0x800706BA));
         var older = new System.Runtime.InteropServices.COMException("Unknown name.", unchecked((int)0x80020006));

         foreach (Exception cause in new Exception[] { lost, older })
            Assert.Equal(ComErrorText.LevelFor(cause), SettingsPageStatus.LevelFor(SettingsPageStatus.ForFallback(cause)));

         Assert.Equal(SettingsPageState.NotConnected, SettingsPageStatus.ForFallback(lost));
         Assert.Equal(StatusLevel.Critical, SettingsPageStatus.LevelFor(SettingsPageStatus.ForFallback(lost)));
         Assert.Equal(ConnectionStatus.WordFor(LinkState.Lost), SettingsPageStatus.WordFor(SettingsPageStatus.ForFallback(lost)));

         Assert.Equal(SettingsPageState.ReadFromFile, SettingsPageStatus.ForFallback(older));
         Assert.Equal(StatusLevel.Warning, SettingsPageStatus.LevelFor(SettingsPageStatus.ForFallback(older)));
         string word = SettingsPageStatus.WordFor(SettingsPageStatus.ForFallback(older));
         Assert.NotEqual(ConnectionStatus.WordFor(LinkState.Lost), word);
         Assert.NotEqual(ConnectionStatus.WordFor(LinkState.NotConnected), word);
      }
   }
}
