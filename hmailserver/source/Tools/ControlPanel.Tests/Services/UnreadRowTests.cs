// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A settings row whose value could not be read when the page was built is shown
   /// with no editor: BuildUi passes over it. A save wrote every row all the same,
   /// from the editor the row did not have - a number row wrote 0 over the server's
   /// value, the others threw and were counted as not saved - or from the editor of
   /// an earlier build of the page, which holds what the server held then.
   ///
   /// Scanned rather than rendered, as WithheldRowTests is; it fails against the tree
   /// before the fix.
   /// </summary>
   public class UnreadRowTests
   {
      [Fact]
      public void ARowThePageShowedNoEditorFor_IsNeitherWrittenNorCountedBySave()
      {
         string source = WithheldRowTests.ReadServerSettingsView();
         Assert.False(source == null, "ServerSettingsView.xaml.cs could not be found from the test binaries.");

         // BuildUi remembers each row it built an editor for, where it builds it...
         Match remembered = Regex.Match(source, @"lastRow = setting\.CreateRow\(value\);\s*(\w+)\.Add\(setting\);");
         Assert.True(remembered.Success, "BuildUi does not remember the rows it built an editor for.");
         string built = remembered.Groups[1].Value;

         // ... forgets them before it builds the page again ...
         string buildUi = WithheldRowTests.MemberText(source, "private void BuildUi()");
         Assert.True(buildUi != null, "BuildUi is gone from ServerSettingsView.");
         int forgets = buildUi.IndexOf(built + ".Clear()", StringComparison.Ordinal);
         int builds = buildUi.IndexOf("setting.CreateRow(value)", StringComparison.Ordinal);
         Assert.True(forgets >= 0 && forgets < builds, "BuildUi does not forget the rows of its last build before it builds the page again.");

         // ... and a save passes over any other row before it writes or counts it.
         string save = WithheldRowTests.MemberText(source, "private void Save_Click(object sender, RoutedEventArgs e)");
         Assert.True(save != null, "Save_Click is gone from ServerSettingsView.");
         int passes = save.IndexOf("!" + built + ".Contains(setting)", StringComparison.Ordinal);
         Assert.True(passes >= 0, "Save_Click writes rows the page showed no editor for.");
         foreach (string write in new[] { "SaveToIni(", "restRows.Add(", "setting.Write(", "saved++" })
         {
            int at = save.IndexOf(write, StringComparison.Ordinal);
            Assert.True(at > passes, "Save_Click reaches " + write + " before it passes over a row the page showed no editor for.");
         }
      }
   }
}
