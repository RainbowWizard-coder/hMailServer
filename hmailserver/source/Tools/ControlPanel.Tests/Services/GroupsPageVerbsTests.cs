// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
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

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
