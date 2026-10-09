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
   /// The Auto-ban page failed on every second visit and every Reload: the
   /// never-ban editor kept its grid and its status line in readonly fields,
   /// built once with the definition, while BuildUi builds every editor again on
   /// every OnEnter. The second build added the same grid to a new panel, WPF
   /// threw "Specified element is already the logical child of another element",
   /// and the shell reported that as "Server unavailable" over a blank page. The
   /// editor makes its grid and status line anew in every BuildEditor now, and
   /// the shell calls a fault of a page's own what it is.
   ///
   /// Scanned rather than rendered, as UnreadRowTests is: this assembly has no
   /// WPF, and the editor needs one. The render harness enters the page twice
   /// for its empty and lost-link states, which is where the fault showed. Each
   /// test fails against the tree before the fix.
   /// </summary>
   public class NeverBanEditorTests
   {
      [Fact]
      public void TheEditor_MakesItsGridAndStatusLine_OnEveryBuild()
      {
         string source = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.NeverBanNames.cs"));
         string rows = source.Substring(source.IndexOf("private sealed class NeverBanNameRows", StringComparison.Ordinal));
         rows = rows.Substring(0, rows.IndexOf("private static class AddNeverBanNameDialog", StringComparison.Ordinal));

         Assert.False(Regex.IsMatch(rows, @"readonly\s+DataGrid\s+grid_"), "The never-ban editor keeps one grid for every build, which the second build cannot add to its panel.");
         Assert.False(Regex.IsMatch(rows, @"readonly\s+[\w.]+\s+status_"), "The never-ban editor keeps one status line for every build.");

         string build = WithheldRowTests.MemberText(rows, "protected override FrameworkElement BuildEditor(object value)");
         Assert.True(build != null, "NeverBanNameRows.BuildEditor is gone.");
         Assert.True(build.Contains("grid_ = new DataGrid", StringComparison.Ordinal), "BuildEditor does not make a new grid.");
         Assert.True(build.Contains("status_ = new Scaffold.InlineNotice", StringComparison.Ordinal), "BuildEditor does not make a new status line.");
      }

      [Fact]
      public void TheShell_CallsAFaultOfThePagesOwn_NotServerUnavailable()
      {
         string source = ReadControlPanelFile("MainWindow.xaml.cs");
         string enter = WithheldRowTests.MemberText(source, "private void EnterPage(object page)");
         Assert.True(enter != null, "MainWindow.EnterPage is gone.");
         Assert.True(enter.Contains("IsTransportFailure(ex)", StringComparison.Ordinal) && enter.Contains("COMException", StringComparison.Ordinal),
            "EnterPage does not ask whether the exception is the server's before it says \"Server unavailable\".");
         Assert.True(enter.Contains("L(\"Could not build this page\")", StringComparison.Ordinal),
            "EnterPage has no title for a fault of the page's own.");
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
