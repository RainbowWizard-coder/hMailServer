// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The three notes that took over a caption's meaning when the caption was cut
   /// short: the setting profiles page's consent box, whose caption was the whole
   /// consequence of confirming; the account's auto-reply box, whose caption said
   /// what outside senders get; and the folder permission entry's Post, whose caption
   /// carried the Send-As rule. A check box names itself to a screen reader by its
   /// caption, so the note under the box is told to the box as its help text, and a
   /// screen reader reading the box still hears the consequence. WPF is not run here -
   /// this assembly is WPF-free - so each test holds the line of the view that does
   /// it, as the Control Panel's own rule for a sentence under a box has it
   /// (DirectorySyncView, TcpIpPortDialog, LdapSettingsView).
   /// </summary>
   public class ConsequenceNoteTests
   {
      [Theory]
      [InlineData("ProfilesView.cs", "confirm_", "confirmNote_.Text")]
      [InlineData("AccountDialog.cs", "vacationInternalOnly_", "outsideNote")]
      [InlineData("FolderPermissionsDialog.cs", "cb", "note")]
      public void TheNoteUnderTheBox_IsTheBoxsHelpText(string view, string box, string note)
      {
         string source = ReadControlPanelFile(Path.Join("Views", view));
         Assert.True(source.Contains("AutomationProperties.SetHelpText(" + box + ", " + note + ");", StringComparison.Ordinal),
            "Views/" + view + " no longer tells " + box + " its note as help text, so a screen reader reading the box hears its caption alone.");
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
