// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Groups page's Members… is the list's first verb, beside Edit, Delete and
   /// Add, enabled while a group is selected, and Enter and a double-click on a
   /// group open it, F2 renaming it (review finding F201). It was a lone button
   /// under the list, 760 pixels down at 1280 by 800, with a hint beside it that
   /// changed as a group was chosen and was never announced, while Enter and a
   /// double-click on a group - a page whose reason to exist is the members -
   /// opened the rename.
   /// </summary>
   public class GroupsPageVerbsTests
   {
      [Fact]
      public void Members_IsTheListsFirstVerb_AndOpensTheRow()
      {
         string specs = ReadControlPanelFile(Path.Join("Views", "CollectionSpecs.cs"));
         string page = WithheldRowTests.MemberText(specs, "public GroupsPageView(CollectionSpec spec)");
         Assert.True(page != null, "GroupsPageView's constructor is gone.");
         Assert.Contains("editor_.AddRowVerb(membersButton_, opensRow: true);", page, StringComparison.Ordinal);
         Assert.Contains("Content = L(\"_Members…\")", page, StringComparison.Ordinal);
         Assert.Contains("Appearance = Wpf.Ui.Controls.ControlAppearance.Secondary", page, StringComparison.Ordinal);

         // No row of its own under the list, and no hint that changed unannounced.
         Assert.DoesNotContain("AddBelowList", page, StringComparison.Ordinal);
         Assert.DoesNotContain("membersHint_", specs, StringComparison.Ordinal);
         Assert.DoesNotContain("Select a group to edit which accounts are members of it.", specs, StringComparison.Ordinal);

         // The group the dialog opens on is the editor's selected row, by its id.
         string open = WithheldRowTests.MemberText(specs, "private void OpenMembers()");
         Assert.True(open != null, "GroupsPageView.OpenMembers is gone.");
         Assert.Contains("editor_.SelectedEntry()", open, StringComparison.Ordinal);
      }

      [Fact]
      public void ARowVerb_GoesFirst_IsGated_AndTakesEnterAndTheDoubleClick()
      {
         string editor = ReadControlPanelFile(Path.Join("Views", "CollectionEditorView.cs"));
         string add = WithheldRowTests.MemberText(editor, "internal void AddRowVerb(Button verb, bool opensRow)");
         Assert.True(add != null, "CollectionEditorView.AddRowVerb is gone.");
         Assert.Contains("actions_.Children.Insert(0, verb);", add, StringComparison.Ordinal);
         Assert.Contains("Services.SelectionGate.Bind(grid_, verb);", add, StringComparison.Ordinal);
         Assert.Contains("openVerb_ = verb;", add, StringComparison.Ordinal);

         // F2 renames through Edit, wired ahead of the grid's other keys; Enter and a
         // double-click press the row's verb where there is one, and Edit where not.
         int rename = editor.IndexOf("GridKeys.WireRename(grid_, () => openVerb_ != null && GridKeys.Press(edit));", StringComparison.Ordinal);
         int keys = editor.IndexOf("GridKeys.Wire(grid_, () => GridKeys.Press(openVerb_ ?? edit), GridKeys.Pressing(del), GridKeys.Pressing(addButton_));", StringComparison.Ordinal);
         Assert.True(rename > 0, "F2 is not wired to Edit on a list whose rows open another verb.");
         Assert.True(keys > rename, "Enter and the double-click do not press the row's verb, or are wired before F2's rename, whose key they would take.");
      }

      /// <summary>
      /// Members… shares its toolbar with Edit, Delete and Add, and the header with
      /// Refresh, in one access-key scope, so no two of them may have one key in any
      /// language: WPF moves the keyboard between two captions of one key rather than
      /// pressing either, and in French and Italian Alt+M reached Members… and Edit,
      /// in Russian Alt+У Members… and Delete, and opened neither. build/check-mnemonics.py
      /// reads a file at a time, and Members… is GroupsPageView's caption while the
      /// others are CollectionEditorView's.
      /// </summary>
      [Fact]
      public void TheToolbarsAccessKeys_AreDistinct_InEveryLanguage()
      {
         string[] captions = { "_Members…", "_Edit", "_Delete", "_Add", "_Refresh" };
         string specs = ReadControlPanelFile(Path.Join("Views", "CollectionSpecs.cs"));
         string editor = ReadControlPanelFile(Path.Join("Views", "CollectionEditorView.cs"));
         Assert.Contains("L(\"_Members…\")", specs, StringComparison.Ordinal);
         foreach (string caption in captions.Skip(1))
            Assert.True(editor.Contains("MakeButton(L(\"" + caption + "\")", StringComparison.Ordinal),
               "CollectionEditorView no longer makes the " + caption + " button; update this test with the toolbar.");

         AssertDistinct("en", captions);
         string resources = Path.GetDirectoryName(ControlPanelFile(Path.Join("Resources", "Strings.resx")));
         int catalogues = 0;
         foreach (string path in Directory.EnumerateFiles(resources, "Strings.*.resx"))
         {
            string tag = Path.GetFileName(path)["Strings.".Length..^".resx".Length];
            Dictionary<string, string> catalogue = XDocument.Load(path).Root.Elements("data")
               .ToDictionary(d => (string)d.Attribute("name"), d => (string)d.Element("value") ?? "", StringComparer.Ordinal);
            var translated = new List<string>();
            foreach (string caption in captions)
            {
               Assert.True(catalogue.TryGetValue(caption, out string value) && value.Length > 0, tag + " does not translate " + caption + ".");
               translated.Add(value);
            }

            AssertDistinct(tag, translated);
            catalogues++;
         }

         Assert.Equal(17, catalogues);
      }

      private static void AssertDistinct(string tag, IEnumerable<string> captions)
      {
         var seen = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (string caption in captions)
         {
            string key = MnemonicText.Key(caption);
            Assert.True(key != null, tag + ": \"" + caption + "\" has no Alt key.");
            Assert.True(!seen.ContainsKey(key), tag + ": Alt+" + key + " in \"" + caption + "\" collides with \"" + seen.GetValueOrDefault(key) + "\" in the Groups page's toolbar.");
            seen[key] = caption;
         }
      }

      [Fact]
      public void F2_IsHandledBeforeTheGridsOtherKeys_AndOnlyFromTheRows()
      {
         string keys = ReadControlPanelFile(Path.Join("Views", "GridKeys.cs"));
         string rename = WithheldRowTests.MemberText(keys, "public static void WireRename(Selector list, Func<bool> rename)");
         Assert.True(rename != null, "GridKeys.WireRename is gone.");
         Assert.Contains("e.Key != Key.F2", rename, StringComparison.Ordinal);
         Assert.Contains("FromTheRows_(list, e.OriginalSource as DependencyObject)", rename, StringComparison.Ordinal);
         Assert.Contains("e.Handled = true;", rename, StringComparison.Ordinal);
      }

      private static string ReadControlPanelFile(string relative) => File.ReadAllText(ControlPanelFile(relative));

      private static string ControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
