// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The generated Add and Edit dialog behind every collection page stays open on
   /// a refused save (review finding F209). Every committer returned true, the
   /// dialog closed, and the page wrote over COM after it had gone, so that a
   /// refusal came as a message box, "Could not save: ...", with everything typed
   /// lost. The fields' captions had no Alt keys (none at all in Japanese), a number
   /// box stretched to 412 pixels for a one-to-three digit score, and Blocked
   /// senders' Score was a text field whose default was the string "100".
   /// </summary>
   public class FieldDialogSaveTests
   {
      // ---- the Alt keys ------------------------------------------------------------------

      [Fact]
      public void EveryCaption_GetsAKeyOfItsOwn_AWordsFirstLetterFirst()
      {
         List<string> keyed = FieldDialogRules.WithAccessKeys(
            new[] { "DNS host", "Expected result", "Score", "Refusal text", "Active" },
            new[] { "DNSHost", "ExpectedResult", "Score", "RejectMessage", "Active" },
            new[] { 'A' }, markAfter: false);

         Assert.Equal(new[] { "_DNS host", "_Expected result", "_Score", "_Refusal text", "A_ctive" }, keyed);
         Assert.Equal(keyed.Count, keyed.Select(c => char.ToUpperInvariant(c[c.IndexOf('_') + 1])).Distinct().Count());
      }

      [Fact]
      public void AKeyTaken_IsNotGivenAgain_AndACaptionThatHasOneKeepsIt()
      {
         List<string> keyed = FieldDialogRules.WithAccessKeys(
            new[] { "Name", "_Note", "Number" },
            new[] { "Name", "Note", "Number" },
            new[] { 'S' }, markAfter: false);

         // N is the note's, so the name and the number take their next letters.
         Assert.Equal(new[] { "N_ame", "_Note", "N_umber" }, keyed);
      }

      [Fact]
      public void AlphabetsAKeyboardTypes_AreKeys_InTheirOwnLetters()
      {
         List<string> keyed = FieldDialogRules.WithAccessKeys(
            new[] { "Имя", "Описание" }, new[] { "Name", "Description" }, new[] { 'Д' }, markAfter: false);
         Assert.Equal(new[] { "_Имя", "_Описание" }, keyed);
      }

      [Fact]
      public void JapaneseAndChinese_MarkTheKeyAfterTheCaption_WithTheEnglishPropertysLetter()
      {
         Assert.True(FieldDialogRules.MarksKeyAfter("追加(_A)"));
         Assert.False(FieldDialogRules.MarksKeyAfter("_Add"));

         List<string> keyed = FieldDialogRules.WithAccessKeys(
            new[] { "DNS ホスト", "期待される結果", "スコア…" },
            new[] { "DNSHost", "ExpectedResult", "Score" },
            FieldDialogRules.KeysOf(new[] { "追加(_A)", "キャンセル" }), markAfter: true);

         // The mark goes before a closing ellipsis, as the catalogues put it.
         Assert.Equal(new[] { "DNS ホスト(_D)", "期待される結果(_E)", "スコア(_S)…" }, keyed);
      }

      [Fact]
      public void ACaptionWithNoLetterAKeyboardTypes_IsMarkedAfter_EvenWhereTheLanguageMarksInside()
      {
         List<string> keyed = FieldDialogRules.WithAccessKeys(new[] { "名前" }, new[] { "Name" }, Array.Empty<char>(), markAfter: false);
         Assert.Equal(new[] { "名前(_N)" }, keyed);
      }

      [Fact]
      public void TheButtonsKeys_AreReadAsTheCatalogueWordsThem()
      {
         Assert.Equal(new[] { 'A', 'S' }, FieldDialogRules.KeysOf(new[] { "_Add", "Cancel", "_Save", "Save__as" }));
         Assert.Equal(new[] { 'A' }, FieldDialogRules.KeysOf(new[] { "追加(_A)" }));
      }

      // ---- the field a refusal names ---------------------------------------------------

      [Fact]
      public void ARefusal_IsSaidOnTheFieldItNames_TheLongestCaptionFirst()
      {
         string[] captions = { "_Name", "Name of the list", "Score" };
         string[] props = { "Name", "ListName", "Score" };
         Assert.Equal(1, FieldDialogRules.FieldNamedIn("Could not save: Name of the list is already in use.", captions, props));
         Assert.Equal(2, FieldDialogRules.FieldNamedIn("Could not save: the score must be between 0 and 1000.", captions, props));
         Assert.Equal(0, FieldDialogRules.FieldNamedIn("Could not save: name is empty.", captions, props));
      }

      [Fact]
      public void ARefusalThatNamesNoField_NamesNone()
      {
         Assert.Equal(-1, FieldDialogRules.FieldNamedIn("Could not save: the connection was lost.", new[] { "DNS host" }, new[] { "DNSHost" }));
         Assert.Equal(-1, FieldDialogRules.FieldNamedIn(null, new[] { "DNS host" }, new[] { "DNSHost" }));
      }

      [Fact]
      public void ARefusalThatNamesAProperty_IsSaidOnItsField()
      {
         Assert.Equal(0, FieldDialogRules.FieldNamedIn("Could not save: DNSHost is not a host name.", new[] { "DNS host", "Score" }, new[] { "DNSHost", "Score" }));
      }

      // ---- the dialog and its pages ----------------------------------------------------

      [Fact]
      public void TheDialog_WritesWithItselfOpen_AndSaysARefusalOnItsField()
      {
         string dialog = Read("FieldDialog.cs");
         Assert.Contains("Func<Dictionary<string, object>, Refusal> save)", dialog, StringComparison.Ordinal);
         Assert.Contains("public sealed record Refusal(string Text, string Prop = null);", dialog, StringComparison.Ordinal);

         // The checks first, then the write; a refusal stays, on its field.
         int checks = dialog.IndexOf("if (!FieldDialogRules.AllAccept(committers_))", StringComparison.Ordinal);
         int write = dialog.IndexOf("Refusal refusal = save_?.Invoke(Result);", StringComparison.Ordinal);
         int said = dialog.IndexOf("DialogFields.ShowError(RowFor_(refusal), refusal.Text);", StringComparison.Ordinal);
         int closes = dialog.IndexOf("DialogResult = true;", StringComparison.Ordinal);
         Assert.True(checks >= 0 && write > checks && said > write && closes > said, "The dialog closes before the write, or does not say a refusal.");
         Assert.Contains("Result.Clear();", dialog, StringComparison.Ordinal);
         Assert.Contains("FieldDialogRules.FieldNamedIn(refusal.Text,", dialog, StringComparison.Ordinal);

         // An Alt key on every caption, none the buttons'; a number box a number's width.
         Assert.Contains("FieldDialogRules.WithAccessKeys(", dialog, StringComparison.Ordinal);
         Assert.Contains("FieldDialogRules.KeysOf(new[] { okCaption, L(\"Cancel\") })", dialog, StringComparison.Ordinal);
         Assert.Contains("Width = 160,", dialog, StringComparison.Ordinal);
         Assert.Contains("HorizontalAlignment = HorizontalAlignment.Left", dialog, StringComparison.Ordinal);
         Assert.Contains("KeepRoomForFieldErrors(Math.Max(1, fields.Count(f => f.Required)));", dialog, StringComparison.Ordinal);
      }

      [Fact]
      public void TheListPage_PassesItsWriteIn_WhereItWroteAfterTheDialogHadGone()
      {
         string page = Read("CollectionEditorView.cs");
         string open = WithheldRowTests.MemberText(page, "private void OpenDialog(Row existing)");
         Assert.True(open != null, "CollectionEditorView.OpenDialog is gone.");
         Assert.Contains("new FieldDialog(spec_, existing, Window.GetWindow(this), values =>", open, StringComparison.Ordinal);
         Assert.Contains("return new FieldDialog.Refusal(F(\"Could not save: {0}\", ServerSession.DescribeComError(ex)));", open, StringComparison.Ordinal);
         Assert.DoesNotContain("MessageBox.Show(F(\"Could not save: {0}\"", open, StringComparison.Ordinal);
         Assert.DoesNotContain("dlg.Result", open, StringComparison.Ordinal);

         string quarantine = Read("QuarantineView.cs");
         string digest = WithheldRowTests.MemberText(quarantine, "private void EditDigestSettings()");
         Assert.True(digest != null, "QuarantineView.EditDigestSettings is gone.");
         Assert.Contains("new FieldDialog(spec, current, Window.GetWindow(this), values =>", digest, StringComparison.Ordinal);
         Assert.Contains("_ => new FieldDialog.Refusal(F(\"Could not save: {0}\", ServerSession.DescribeComError(error)), \"QuarantineDigestLinkHours\")", digest, StringComparison.Ordinal);
      }

      [Fact]
      public void BlockedSendersScore_IsANumber_WithTheNumber100()
      {
         string specs = Read("CollectionSpecs.cs");
         int at = specs.IndexOf("public static CollectionEditorView BlockedSenders()", StringComparison.Ordinal);
         Assert.True(at >= 0, "No BlockedSenders spec.");
         string spec = specs.Substring(at, specs.IndexOf("\n      }", at, StringComparison.Ordinal) - at);
         Assert.Contains("new FieldSpec { Prop = \"Score\", Label = L(\"Score\"), Kind = FieldKind.Number, GridWidth = 80, Default = 100,", spec, StringComparison.Ordinal);
         Assert.DoesNotContain("Default = \"100\"", spec, StringComparison.Ordinal);
         Assert.Contains("Prop = \"Address\", Label = L(\"Address or domain\"), GridWidth = 260, Required = true, Hint = L(", spec, StringComparison.Ordinal);
      }

      private static string Read(string file) => FormLineAssert.ReadControlPanelFile(Path.Join("Views", file));
   }
}
