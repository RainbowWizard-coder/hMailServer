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
   /// One value, three levels (docs/SettingLevels.md): the words the Levels tab of
   /// the domain and account dialogs says beside each setting - what is in effect
   /// and where it came from - and the list of settings it offers. The server
   /// decides the value and the source; these hold the tab to saying what the
   /// server said, and nothing for a word it does not know.
   /// </summary>
   public class SettingLevelsTests
   {
      /// <summary>
      /// A setting is headed by its name as a person reads it, and its key - the server's
      /// word, which the REST API sets a level by - is a small caption beside it: the
      /// tab's headings were the keys themselves, "max_message_size_kb", in mono type in
      /// every language, against the owner's decision 11 (a key name only as a small
      /// caption; the final check of Tier 2). The box's caption is still escaped: a
      /// CheckBox reads the first underscore of a caption as its access key, which made
      /// "max_message_size_kb" "maxmessage_size_kb" with Alt+M on it when the key was the
      /// caption, and a translation could carry one.
      /// </summary>
      [Fact]
      public void ASettingIsHeadedByItsName_AndItsKeyIsASmallCaptionBesideIt()
      {
         string panel = SaveStateModelTests.ReadControlPanelFile("Views/SettingLevelsPanel.cs");
         Assert.Contains("string caption = SettingLevels.Caption(level.Name);", panel, StringComparison.Ordinal);
         Assert.Contains("new CheckBox { Content = MnemonicText.Escape(caption),", panel, StringComparison.Ordinal);
         Assert.Contains("panel.Children.Add(Named_(check, level.Name));", panel, StringComparison.Ordinal);
         Assert.Contains("panel.Children.Add(Named_(name, level.Name));", panel, StringComparison.Ordinal);
         Assert.DoesNotContain("Content = level.Name", panel, StringComparison.Ordinal);
         Assert.DoesNotContain("Text = level.Name", panel, StringComparison.Ordinal);
         Assert.DoesNotContain("MnemonicText.Escape(level.Name)", panel, StringComparison.Ordinal);

         foreach (SettingLevel level in SettingLevels.All)
         {
            Assert.True(SettingLevels.HasCaption(level.Name), level.Name + " has no name of its own on the Levels tab.");
            string caption = SettingLevels.Caption(level.Name);
            Assert.NotEqual(level.Name, caption);
            Assert.DoesNotContain("_", caption, StringComparison.Ordinal);
            Assert.Equal(caption, MnemonicText.AccessTextShown(MnemonicText.Escape(caption)));
            Assert.Null(MnemonicText.Key(MnemonicText.Escape(caption)));
         }

         Assert.Equal(SettingLevels.All.Count, SettingLevels.All.Select(level => SettingLevels.Caption(level.Name)).Distinct().Count());
         // A key a newer server adds is shown as it is spelt, rather than not at all.
         Assert.Equal("a_newer_key", SettingLevels.Caption("a_newer_key"));
      }

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
