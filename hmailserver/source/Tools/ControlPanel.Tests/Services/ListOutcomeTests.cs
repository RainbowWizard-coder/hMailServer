// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review findings F208 and F113: an Add, an Edit or a Delete on a list the
   /// generic editor draws gave no visible outcome, and the list lost its place.
   /// The editor set "Added at…", "Saved at…" or "Deleted." and then read the list
   /// again, and the read hid the notice on success; the read rebuilt the rows, so
   /// the edited row lost its selection and the list jumped to the top, and a new
   /// entry was neither selected nor in view. These hold the words that name the
   /// entry, the row kept after a delete, and the order the editor now does things
   /// in: the read first, then the outcome, then the row selected, in view and
   /// with the keyboard.
   /// </summary>
   public class ListOutcomeTests
   {
      [Fact]
      public void TheEntryName_IsTheFirstShownValueThatSaysSomething_OnOneLine()
      {
         Assert.Equal("multi.surbl.org", ListOutcome.EntryName(new[] { "", "  multi.surbl.org ", "Listed in SURBL" }));
         Assert.Equal("Greetings from the server", ListOutcome.EntryName(new[] { "Greetings from\r\n  the server" }));
         Assert.Null(ListOutcome.EntryName(new[] { "", "   ", null }));
         Assert.Null(ListOutcome.EntryName(null));

         string longName = ListOutcome.EntryName(new[] { new string('a', 200) });
         Assert.Equal(80, longName.Length);
         Assert.EndsWith("…", longName, StringComparison.Ordinal);
      }

      [Fact]
      public void TheOutcome_NamesTheEntry_OrSaysWhenWhereItHasNoName()
      {
         var at = new DateTime(2026, 10, 9, 14, 5, 0);
         Assert.Equal("multi.surbl.org added.", ListOutcome.Added("multi.surbl.org", at));
         Assert.Equal("multi.surbl.org saved.", ListOutcome.Saved("multi.surbl.org", at));
         Assert.Equal("multi.surbl.org deleted.", ListOutcome.Deleted("multi.surbl.org"));

         Assert.StartsWith("Added at ", ListOutcome.Added(null, at), StringComparison.Ordinal);
         Assert.StartsWith("Saved at ", ListOutcome.Saved("", at), StringComparison.Ordinal);
         Assert.Equal("Deleted.", ListOutcome.Deleted(null));
      }

      [Theory]
      [InlineData(3, 1, 1)]    // the row that took the deleted one's place
      [InlineData(2, 2, 1)]    // the last row was deleted: the new last row
      [InlineData(5, 9, 4)]    // a list that shrank by more than one
      [InlineData(0, 0, -1)]   // the list is empty now
      [InlineData(4, -1, -1)]  // nothing was selected
      public void AfterADelete_TheRowThatTookItsPlaceIsKept(int countAfter, int deletedIndex, int kept)
         => Assert.Equal(kept, ListOutcome.IndexAfterDelete(countAfter, deletedIndex));

      /// <summary>
      /// The editor reads the list before it says the outcome, so that the read does
      /// not hide it, and neither an Add nor a Delete says anything before the read.
      /// </summary>
      [Theory]
      [InlineData("private void OpenDialog(Row existing)", "Load_(savedId, -1, focus: saved);", "ListOutcome.Added(name, DateTime.Now)")]
      [InlineData("private void DeleteSelected()", "Load_(0, index, focus: true);", "ListOutcome.Deleted(name)")]
      public void TheEditor_ReadsTheListFirst_ThenSaysTheOutcome(string member, string read, string outcome)
      {
         string body = WithheldRowTests.MemberText(ReadEditor(), member);
         Assert.True(body != null, "CollectionEditorView: " + member + " is gone.");

         int readAt = body.IndexOf(read, StringComparison.Ordinal);
         int sayAt = body.IndexOf(outcome, StringComparison.Ordinal);
         Assert.True(readAt >= 0, member + " does not read the list with " + read);
         Assert.True(sayAt > readAt, member + " says the outcome before it reads the list, and the read hides it.");
         Assert.True(body.IndexOf("Say_(StatusLevel.Good", StringComparison.Ordinal) > readAt, member + " says its outcome before it reads the list.");
         Assert.DoesNotContain("Reload();", body, StringComparison.Ordinal);
      }

      [Fact]
      public void TheEditor_KeepsTheSelectedEntry_InViewAndWithTheKeyboard()
      {
         string code = ReadEditor();
         Assert.Contains("private void Reload()\n         => Load_(grid_.SelectedItem is Row selected ? selected.Id : 0, -1, focus: false);",
            code.Replace("\r\n", "\n"), StringComparison.Ordinal);

         string select = WithheldRowTests.MemberText(code, "private void Select_(Row row, bool focus)");
         Assert.True(select != null, "CollectionEditorView.Select_ is gone.");
         Assert.Contains("grid_.SelectedItem = row;", select, StringComparison.Ordinal);
         Assert.Contains("grid_.ScrollIntoView(row);", select, StringComparison.Ordinal);
         Assert.Contains("container.Focus();", select, StringComparison.Ordinal);

         // Said again when the words are the same as the last time.
         string say = WithheldRowTests.MemberText(code, "private void Say_(StatusLevel level, string text)");
         Assert.True(say != null, "CollectionEditorView.Say_ is gone.");
         Assert.Contains("Announce.Request(status_);", say, StringComparison.Ordinal);
      }

      private static string ReadEditor()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "CollectionEditorView.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/CollectionEditorView.cs could not be found from the test binaries.");
         return null;
      }
   }
}
