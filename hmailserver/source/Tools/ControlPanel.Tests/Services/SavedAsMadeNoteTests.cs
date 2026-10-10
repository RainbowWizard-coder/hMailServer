// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A list on a dialog's tab whose Add, Edit and Delete write to the server as they
   /// are pressed - the domain's aliases and its allow and block list, the account's
   /// external accounts and its rules - says so, so that the dialog's Cancel is not
   /// read as undoing them (review findings F196 and F108). The Levels tab said it of
   /// its own Save; these said nothing. Scanned rather than rendered: this assembly
   /// has no WPF.
   /// </summary>
   public class SavedAsMadeNoteTests
   {
      private const string Sentence = "Changes here are saved as you make them, not with the dialog's Save, so Cancel does not undo them.";

      // How a factory in CollectionSpecs.cs that builds an editor for a dialog's tab ends.
      internal const string DialogTabEnd = "}, embedded: true, onDialogTab: true);";

      [Fact]
      public void EveryListBuiltForADialog_SaysItSavesAsYouGo()
      {
         string specs = SaveStateModelTests.ReadControlPanelFile("Views/CollectionSpecs.cs");

         // Each factory that builds an editor for a dialog's tab ends its spec with
         // "}, embedded: true, onDialogTab: true);"; the page that embeds one of its
         // own (the Groups page) builds it another way and is not a dialog.
         int count = 0;
         int end = specs.IndexOf(DialogTabEnd, StringComparison.Ordinal);
         while (end >= 0)
         {
            int start = specs.LastIndexOf("=> new(new CollectionSpec", end, StringComparison.Ordinal);
            Assert.True(start >= 0, "An embedded editor is built from something other than a CollectionSpec factory.");
            string factory = specs.Substring(start, end - start);
            string name = Regex.Match(specs.Substring(0, start), @"public static CollectionEditorView (\w+)\([^)]*\)\s*$").Groups[1].Value;
            Assert.True(factory.Contains("SavesAsMadeInDialog = true", StringComparison.Ordinal), name + " is built for a dialog's tab and does not say it saves as you go.");
            count++;
            end = specs.IndexOf(DialogTabEnd, end + 1, StringComparison.Ordinal);
         }

         Assert.Equal(4, count);
      }

      [Fact]
      public void TheEditor_PutsTheNoteUnderItsHint_AndTheRulesTabWhereItsHeaderWas()
      {
         string editor = SaveStateModelTests.ReadControlPanelFile("Views/CollectionEditorView.cs");
         Assert.Contains("if (spec_.SavesAsMadeInDialog)", editor, StringComparison.Ordinal);
         string note = WithheldRowTests.MemberText(editor, "public static TextBlock SavedAsMadeNote()");
         Assert.True(note != null && note.Contains("L(\"" + Sentence + "\")", StringComparison.Ordinal), "The note's sentence moved.");

         string rules = SaveStateModelTests.ReadControlPanelFile("Views/RulesView.xaml.cs");
         string configure = WithheldRowTests.MemberText(rules, "public void ConfigureForRules(Func<dynamic> rulesProvider, bool serverLevel, bool embedded)");
         Assert.True(configure != null && configure.Contains("CollectionEditorView.SavedAsMadeNote()", StringComparison.Ordinal),
            "The account dialog's Rules tab does not say its rules are saved as they are made.");
      }
   }
}
