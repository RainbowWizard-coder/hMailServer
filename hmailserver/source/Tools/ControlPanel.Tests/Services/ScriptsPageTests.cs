// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Event scripts page says what it checks and whether the server runs the
   /// script at all. Check syntax asked the server, which compiles the file it has,
   /// so after an edit it reported "No syntax errors reported." of the old code;
   /// Insert template appended VBScript to a JScript file; Save said "Saved and
   /// reloaded" with the engine switched off; an ordinary load put "Loaded from
   /// disk." in an Information box at the foot of the page; and in German and
   /// Japanese the header's template list and three buttons did not fit the
   /// smallest window.
   ///
   /// Scanned rather than rendered: this assembly has no WPF.
   /// </summary>
   public class ScriptsPageTests
   {
      [Fact]
      public void CheckSyntax_SaysWhatItChecks_WhileTheEditorHoldsChanges()
      {
         string check = Member("private void CheckSyntax()");
         int unsaved = check.IndexOf("if (HasUnsavedChanges)", StringComparison.Ordinal);
         int asks = check.IndexOf("scripting.CheckSyntax()", StringComparison.Ordinal);
         Assert.True(unsaved >= 0 && asks > unsaved, "Check syntax asks the server before it looks at unsaved changes, and reports on the saved script as if it were the one on screen.");
         Assert.Contains("Save & reload to check what is on screen.", check, StringComparison.Ordinal);
      }

      [Fact]
      public void ThePage_ReadsWhetherScriptingIsOn_AndOffersTemplatesOnlyInVBScript()
      {
         string state = Member("private void ReadScriptingState_()");
         Assert.Contains("(bool)scripting.Enabled", state, StringComparison.Ordinal);
         Assert.Contains("(string)scripting.Language", state, StringComparison.Ordinal);
         Assert.Contains("templates_.Visibility = vbscript_ ? Visibility.Visible : Visibility.Collapsed;", state, StringComparison.Ordinal);

         string code = Source();
         Assert.Contains("scriptingOff_.Content = NoticeActions.OpenPage(\"advanced\", \"scripts-open-advanced\");", code, StringComparison.Ordinal);
         Assert.Contains("if (!editor_.IsEnabled || !vbscript_)", Member("private void InsertTemplate(string body)"), StringComparison.Ordinal);
         Assert.Contains("ReadScriptingState_();", Member("private void LoadScript()"), StringComparison.Ordinal);
      }

      [Fact]
      public void Save_SaysReloaded_OnlyWhileScriptingIsOn()
      {
         string save = Member("private bool SaveScript()");
         int on = save.IndexOf("else if (scriptingOn_)", StringComparison.Ordinal);
         int reloaded = save.IndexOf("Saved and reloaded at {0}", StringComparison.Ordinal);
         Assert.True(on >= 0 && reloaded > on, "Save says \"Saved and reloaded\" whether or not scripting is on.");
         Assert.Contains("Scripting is off, so the server does not run these handlers.", save, StringComparison.Ordinal);
      }

      [Fact]
      public void AnOrdinaryLoad_SaysNothing_AndTheNoticesAreUnderTheHeader()
      {
         string code = Source();
         Assert.DoesNotContain("L(\"Loaded from disk.\")", code, StringComparison.Ordinal);
         Assert.DoesNotContain("Grid.SetRow(status_", code, StringComparison.Ordinal);
         Assert.Contains("Grid.SetRow(notices, 1);", code, StringComparison.Ordinal);

         // The header's actions wrap, and the template list is on the editor's card.
         Assert.Contains("var actions = new WrapPanel", code, StringComparison.Ordinal);
         Assert.DoesNotContain("actions.Children.Add(templates_)", code, StringComparison.Ordinal);
         Assert.Contains("editorArea.Children.Add(templates_);", code, StringComparison.Ordinal);
      }

      private static string Member(string signature)
      {
         string text = WithheldRowTests.MemberText(Source(), signature);
         Assert.True(text != null, "ScriptsView has no " + signature + ".");
         return text;
      }

      private static string Source()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "ScriptsView.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/ScriptsView.cs could not be found from the test binaries.");
         return null;
      }
   }
}
