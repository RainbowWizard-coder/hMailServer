// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The dialog that creates a REST API key was the one dialog off the standard
   /// frame: no heading, notes at 65 % opacity, a 200-pixel date picker in the
   /// long date format that cut the year off ("Wednesday, January 6, 2"), and a
   /// refused value reported in a message box after Create key was pressed. The
   /// test fails against that dialog: it is on the frame, its fields are captioned
   /// rows with hints, the expiry is the short date beside quick picks, and a
   /// refused value is said on the field at fault.
   /// </summary>
   public class ApiKeyDialogTests
   {
      [Fact]
      public void TheDialog_IsOnTheFrame_WithFieldRows_AndReportsOnTheFieldAtFault()
      {
         string code = Source();
         string ask = WithheldRowTests.MemberText(code, "public static ApiKeyRequest Ask(Window owner)");
         Assert.True(ask != null, "ApiKeyDialog.Ask is gone.");

         Assert.Contains("new FramedDialog { Owner = owner, Title = L(\"Create a REST API key\") }", ask, StringComparison.Ordinal);
         Assert.Contains("dlg.Frame(L(\"Create a REST API key\"), panel, ok, cancel,", ask, StringComparison.Ordinal);
         Assert.DoesNotContain("new FluentDialogWindow", ask, StringComparison.Ordinal);
         Assert.DoesNotContain("Opacity = 0.65", code, StringComparison.Ordinal);

         foreach (string row in new[] { "labelRow", "expiresRow", "domainsRow", "sourceRow" })
            Assert.Matches(new Regex(@"FieldRow " + row + @" = DialogFields\.Field\("), ask);

         // The short date, which fits, and three quick picks.
         Assert.Contains("SelectedDateFormat = DatePickerFormat.Short", ask, StringComparison.Ordinal);
         foreach (string pick in new[] { "QuickPick_(L(\"_30 days\"), 30, expires)", "QuickPick_(L(\"_90 days\"), 90, expires)", "QuickPick_(L(\"_1 year\"), 365, expires)" })
            Assert.Contains(pick, ask, StringComparison.Ordinal);

         // Every refusal on its field; none in a message box.
         Assert.DoesNotContain("MessageBox.Show", ask, StringComparison.Ordinal);
         Assert.Equal(4, Regex.Matches(ask, @"DialogFields\.ShowError\((labelRow|expiresRow|domainsRow|sourceRow),").Count);
         Assert.Contains("DialogFields.ClearErrors(labelRow, expiresRow, domainsRow, sourceRow);", ask, StringComparison.Ordinal);
      }

      private static string Source() => ReadControlPanelFile(Path.Join("Views", "ApiKeysView.cs"));

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
