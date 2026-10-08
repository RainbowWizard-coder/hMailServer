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
