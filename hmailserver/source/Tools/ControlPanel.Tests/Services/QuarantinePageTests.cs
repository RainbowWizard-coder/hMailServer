// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The quarantine page's empty state said "Quarantining is switched off" of
   /// every empty list, and never read the switch: a server whose quarantine is
   /// on and merely empty was told it was off, and sent to turn on what was on.
   /// The empty state now reads QuarantineEnabled, as the Anti-spam settings page
   /// does, says one thing while quarantining is on and another while it is off,
   /// and offers the page that turns it on only while it is off.
   ///
   /// Scanned rather than rendered, as UnreadRowTests is: this assembly has no
   /// WPF. The test fails against the tree before the fix.
   /// </summary>
   public class QuarantinePageTests
   {
      [Fact]
      public void TheEmptyState_ReadsTheSwitch_AndSaysOffOnlyWhenItIsOff()
      {
         string source = ReadView("QuarantineView.cs");
         string reload = WithheldRowTests.MemberText(source, "private void Reload()");
         Assert.True(reload != null, "QuarantineView.Reload is gone.");

         Assert.True(reload.Contains("ReadBool(\"QuarantineEnabled\"", StringComparison.Ordinal),
            "The quarantine page's empty state does not read QuarantineEnabled, so it says the same thing whether quarantining is on or off.");

         // Two sentences, one per state, and "switched off" only in the off one.
         Assert.True(reload.Contains("Nothing is held right now.", StringComparison.Ordinal),
            "The empty state has no sentence for a quarantine that is on and merely empty.");
         Assert.True(reload.Contains("Quarantining is switched off", StringComparison.Ordinal),
            "The empty state has no sentence for a quarantine that is off.");
         Assert.True(reload.IndexOf("Nothing is held right now.", StringComparison.Ordinal) < reload.IndexOf("Quarantining is switched off", StringComparison.Ordinal),
            "The sentence for a quarantine that is on comes after the one for a quarantine that is off, so the switch reads the wrong way round.");

         // The action that turns it on, offered only while it is off.
         Assert.True(reload.Contains("empty_.Action = quarantining ? null : OpenAntiSpamSettings_()", StringComparison.Ordinal),
            "The empty state does not offer the Anti-spam settings page while quarantining is off, or offers it while it is on.");
         Assert.True(source.Contains("NavigateTo(\"antispam\")", StringComparison.Ordinal),
            "The empty state's action does not open the Anti-spam settings page.");

         // A neutral glyph: an empty quarantine is not a warning.
         Assert.False(source.Contains("MailWarning24", StringComparison.Ordinal), "The empty state still draws the warning glyph.");
      }

      /// <summary>
      /// Review finding F260: each row's allow and block were one list glyph, told
      /// apart by colour and fill alone; the header's four icons were full colour and
      /// pressable with nothing checked, a click answering "Check the messages first";
      /// and a group's row showed neither the pointer nor the keyboard inside it, its
      /// box and icons 8 to 10 pixels left of the rows'.
      /// </summary>
      [Fact]
      public void TheActions_HaveShapesOfTheirOwn_AndTheCheckedMessagesABarInWords()
      {
         string source = ReadView("QuarantineView.cs");

         // Allow and block a person each, both outlined.
         Assert.Contains("IconButton_(\"allow\", SymbolRegular.PersonAvailable24, KeepColourKey, false,", source, StringComparison.Ordinal);
         Assert.Contains("IconButton_(\"block\", SymbolRegular.PersonProhibited24, RemoveColourKey, false,", source, StringComparison.Ordinal);
         Assert.DoesNotContain("TextBulletListSquare24", source, StringComparison.Ordinal);

         // The bar: how many are checked, the four acts in words, pressable only while one is.
         string bar = WithheldRowTests.MemberText(source, "private WrapPanel BuildBulkBar_()");
         Assert.True(bar != null, "QuarantineView.BuildBulkBar_ is gone.");
         foreach (string caption in new[] { "Re_lease", "_Allow sender…", "_Block sender…", "_Delete permanently" })
            Assert.Contains("MakeButton(L(\"" + caption + "\"), ControlAppearance.Secondary", bar, StringComparison.Ordinal);
         Assert.Contains("WrapPanel bulkBar = BuildBulkBar_();", source, StringComparison.Ordinal);
         string update = WithheldRowTests.MemberText(source, "private void UpdateBulkBar_()");
         Assert.True(update != null, "QuarantineView.UpdateBulkBar_ is gone.");
         Assert.Contains("button.IsEnabled = count > 0;", update, StringComparison.Ordinal);
         Assert.Contains("Announce.Request(checkedCount_);", update, StringComparison.Ordinal);
         Assert.Contains("TemplateColumn_(L(\"Actions\"), FourActions_(),", source, StringComparison.Ordinal);

         // A group's row under the pointer and with the keyboard inside it, lined up with the rows.
         Assert.Contains("var pointed = new Trigger { Property = UIElement.IsMouseOverProperty, Value = true };", source, StringComparison.Ordinal);
         Assert.Contains("var focusedWithin = new Trigger { Property = UIElement.IsKeyboardFocusWithinProperty, Value = true };", source, StringComparison.Ordinal);
         Assert.Contains("line.AppendChild(CheckBox_(BoxWidth, new Thickness(10, 0, CheckWidth - BoxWidth, 0)));", source, StringComparison.Ordinal);
      }

      [Fact]
      public void TheBarsButtons_HandTheKeyboardToTheList_BeforeTheyAreDisabled()
      {
         string source = ReadView("QuarantineView.cs").Replace("\r\n", "\n");

         // An act pressed from the keyboard reads the page again, which clears the
         // checks; the button disabled under the keyboard dropped it to the window.
         string update = WithheldRowTests.MemberText(source, "private void UpdateBulkBar_()");
         Assert.True(update != null, "QuarantineView.UpdateBulkBar_ is gone.");
         Assert.True(update.IndexOf("KeepBulkKeyboard_();", StringComparison.Ordinal) >= 0 &&
            update.IndexOf("KeepBulkKeyboard_();", StringComparison.Ordinal) < update.IndexOf("button.IsEnabled = count > 0;", StringComparison.Ordinal),
            "The keyboard is not moved off the bar before its buttons are disabled.");

         string keep = WithheldRowTests.MemberText(source, "private void KeepBulkKeyboard_()");
         Assert.True(keep != null, "QuarantineView.KeepBulkKeyboard_ is gone.");
         // The window's own focus too: the act's question has the keyboard while it is open.
         Assert.Contains("button.IsKeyboardFocused || ReferenceEquals(windowFocus, button)", keep, StringComparison.Ordinal);
         Assert.Contains("list_.Focus();\n            FocusRowAfter_();", keep, StringComparison.Ordinal);
         Assert.Contains("search_.FocusSearch();", keep, StringComparison.Ordinal);
      }

      private static string ReadView(string file)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", file);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/" + file + " could not be found from the test binaries.");
         return null;
      }
   }
}
