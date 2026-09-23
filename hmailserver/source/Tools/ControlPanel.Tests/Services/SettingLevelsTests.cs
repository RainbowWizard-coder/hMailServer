// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// One value, three levels (docs/SettingLevels.md): the words the Levels tab of
   /// the domain and account dialogs says beside each setting - what is in effect
   /// and where it came from - and the list of settings it offers. The server
   /// decides the value and the source; these hold the tab to saying what the
   /// server said, and nothing for a word it does not know.
   /// </summary>
   public class SettingLevelsTests
   {
      [Fact]
      public void WhatIsInEffectIsSaidWithWhereItCameFrom()
      {
         Assert.Equal("In effect: 200, from the account", SettingLevels.InEffect("200", " Account ", SettingLevelType.Integer));
         Assert.Equal("In effect: On, from the domain", SettingLevels.InEffect("1", "domain", SettingLevelType.Boolean));
         Assert.Equal("In effect: Off, from the server", SettingLevels.InEffect("0", "server", SettingLevelType.Boolean));
         Assert.Equal("In effect: (empty), from the server", SettingLevels.InEffect("", "server", SettingLevelType.Text));
      }

      [Fact]
      public void ASourceTheTabDoesNotKnowSaysNothing()
      {
         // The negative control for the one above: an older or a newer server's
         // word is never shown as one of the three.
         Assert.Null(SettingLevels.InEffect("5", null, SettingLevelType.Integer));
         Assert.Null(SettingLevels.InEffect("5", "", SettingLevelType.Integer));
         Assert.Null(SettingLevels.InEffect("5", "mailbox", SettingLevelType.Integer));
      }

      [Fact]
      public void ABooleanIsSentAsTheServerTakesIt()
      {
         Assert.Equal("1", SettingLevels.ForServer(" On ", SettingLevelType.Boolean));
         Assert.Equal("0", SettingLevels.ForServer("false", SettingLevelType.Boolean));
         // Anything else goes as it was typed, for the server to refuse in its own words.
         Assert.Equal("maybe", SettingLevels.ForServer("maybe", SettingLevelType.Boolean));
         Assert.Equal("30", SettingLevels.ForServer(" 30 ", SettingLevelType.Integer));
      }

      [Fact]
      public void TheListIsOneEntryANameAndEveryNameFitsTheOverlay()
      {
         string[] names = SettingLevels.All.Select(s => s.Name).ToArray();
         Assert.Equal(names.Length, names.Distinct().Count());
         Assert.All(names, n => Assert.InRange(n.Length, 1, 30));
         Assert.Contains("recall_days", names);
         Assert.Contains("max_message_size_kb", names);
      }
   }
}
