// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Destructive actions are drawn by their weight (review findings F183 and F076):
   /// a list's delete, remove or revoke, and a button that deletes or replaces in
   /// bulk, is a DangerTextButton - red words on a secondary button, grey while it
   /// cannot be pressed - and the filled Danger appearance is the act itself, a
   /// confirmation's destructive answer or the backup page's restore. The group
   /// members' Remove, the folder permissions' Delete, the account's Folders tab, two
   /// factor and recovery buttons, the certificates' Delete selected and every API
   /// key's Revoke were filled red, several live with nothing chosen, while the
   /// journaling removal, both Restore defaults, both Remove expired and Clear
   /// greylisting triplets were plain buttons. The domain white and black list's and
   /// the suppression list's Remove were DangerTextButtons already; they are held
   /// here with the rest.
   /// </summary>
   public class DangerWeightTests
   {
      // The filled Danger appearance, in the files allowed to use it and why.
      private static readonly Dictionary<string, string> FilledDanger = new()
      {
         [Path.Join("Views", "Dialogs.cs")] = "a confirmation's destructive answer",
         [Path.Join("Views", "BackupView.xaml")] = "Start restore and Restore the mailbox, the restore itself",
         [Path.Join("Views", "QuarantineListDialog.cs")] = "Blacklist and delete, the dialog's own answer",
      };

      [Fact]
      public void TheFilledDangerAppearance_IsTheActItself_Only()
      {
         string root = Path.GetDirectoryName(ControlPanelFile("App.xaml.cs"));
         var offenders = new List<string>();
         foreach (string path in Directory.EnumerateFiles(root, "*.*", SearchOption.AllDirectories)
                     .Where(p => p.EndsWith(".cs", StringComparison.Ordinal) || p.EndsWith(".xaml", StringComparison.Ordinal)))
         {
            string relative = Path.GetRelativePath(root, path);
            if (relative.Split(Path.DirectorySeparatorChar)[0] is "bin" or "obj")
               continue;

            string text = File.ReadAllText(path);
            bool filled = text.Contains("ControlAppearance.Danger", StringComparison.Ordinal)
                          || text.Contains("Appearance=\"Danger\"", StringComparison.Ordinal);
            if (filled && !FilledDanger.ContainsKey(relative))
               offenders.Add(relative);
         }

         Assert.True(offenders.Count == 0,
            "A button wears the filled Danger appearance outside a confirmation's answer and the restore; draw a list's or a bulk delete as DangerTextButton (ButtonStyles.DangerText):\n" + string.Join("\n", offenders));
      }

      [Theory]
      [InlineData("GroupMembersDialog.cs", "ButtonStyles.DangerText(remove);")]
      [InlineData("FolderPermissionsDialog.cs", "ButtonStyles.DangerText(del);")]
      [InlineData("AccountDialog.cs", "ButtonStyles.DangerText(del);")]
      [InlineData("AccountDialog.cs", "ButtonStyles.DangerText(empty);")]
      [InlineData("AccountDialog.cs", "ButtonStyles.DangerText(erase);")]
      [InlineData("AccountTwoFactorPanel.cs", "ButtonStyles.DangerText(disable_);")]
      [InlineData("AccountRecoveryPanel.cs", "ButtonStyles.DangerText(clear_);")]
      [InlineData("AccountRecoveryPanel.cs", "ButtonStyles.DangerText(clearCodes_);")]
      [InlineData("ApiKeysView.cs", "ButtonStyles.DangerText(revoke);")]
      [InlineData("JournalingView.cs", "ButtonStyles.DangerText(remove);")]
      [InlineData("DomainPolicyListView.cs", "ButtonStyles.DangerText(remove);")]
      [InlineData("SuppressionListView.cs", "ButtonStyles.DangerText(remove);")]
      [InlineData("RouteDialog.cs", "ButtonStyles.DangerText(removeBtn);")]
      [InlineData("RolesView.cs", "ButtonStyles.DangerText(take_);")]
      [InlineData("QuarantineView.cs", "ButtonStyles.DangerText(removeExpired_);")]
      [InlineData("MessageTraceView.cs", "ButtonStyles.DangerText(removeExpired_);")]
      [InlineData("ServerSettingsView.xaml.cs", "ButtonStyles.DangerText(btn);")]
      public void EachListDeleteAndBulkDelete_IsADangerTextButton(string file, string call)
         => Assert.Contains(call, ReadControlPanelFile(Path.Join("Views", file)), StringComparison.Ordinal);

      [Theory]
      [InlineData("SslCertificatesView.xaml", "DeleteButton")]
      [InlineData("TcpIpPortsView.xaml", "RestoreDefaultsButton")]
      [InlineData("IPRangesView.xaml", "RestoreDefaultsButton")]
      public void TheXamlPagesBulkAndListDeletes_AreDangerTextButtons(string file, string name)
      {
         string page = ReadControlPanelFile(Path.Join("Views", file));
         Match button = Regex.Match(page, "<ui:Button x:Name=\"" + name + "\"[^>]*>", RegexOptions.Singleline);
         Assert.True(button.Success, file + ": " + name + " is gone.");
         Assert.Contains("Style=\"{StaticResource DangerTextButton}\"", button.Value, StringComparison.Ordinal);
         Assert.DoesNotContain("Appearance=", button.Value, StringComparison.Ordinal);
      }

      [Fact]
      public void TheSettingsPagesClearingButtons_AreDrawnAsDestructive()
      {
         string page = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         foreach (string caption in new[] { "Clear greylisting triplets", "Remove the token", "Discard the index and rebuild it" })
         {
            int at = page.IndexOf("ButtonText = L(\"" + caption + "\"),", StringComparison.Ordinal);
            Assert.True(at >= 0, caption + " is gone.");
            string next = page.Substring(at, Math.Min(200, page.Length - at));
            Assert.Contains("Destructive = true,", next, StringComparison.Ordinal);
         }
      }

      [Theory]
      [InlineData("GroupMembersDialog.cs", "SelectionGate.Bind(list_, remove);")]
      [InlineData("FolderPermissionsDialog.cs", "SelectionGate.Bind(list_, edit, del);")]
      [InlineData("AccountDialog.cs", "SelectionGate.Bind(folderList_, del);")]
      [InlineData("RouteDialog.cs", "SelectionGate.Bind(addressList_, removeBtn);")]
      public void AButtonThatActsOnTheChosenRow_IsGatedByTheList(string file, string gate)
         => Assert.Contains(gate, ReadControlPanelFile(Path.Join("Views", file)), StringComparison.Ordinal);

      [Fact]
      public void ARemovalFromAListThatOutlivesIt_IsCalledRemove()
      {
         // "Remove" takes an entry out of a list the thing outlives - a folder's
         // access entries, a list's recipients; "Delete" destroys.
         string permissions = ReadControlPanelFile(Path.Join("Views", "FolderPermissionsDialog.cs"));
         Assert.Contains("var del = new Wpf.Ui.Controls.Button { Content = L(\"Re_move\")", permissions, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"_Remove selected\")", ReadControlPanelFile(Path.Join("Views", "RecipientsDialog.cs")), StringComparison.Ordinal);
      }

      [Fact]
      public void AFolderIsDeletedByItsName_NotByWhatItsRowSays()
      {
         // The row says "(not subscribed)" in the current language after the name;
         // taking it off in English left it on in every other language.
         string dialog = ReadControlPanelFile(Path.Join("Views", "AccountDialog.cs"));
         string delete = WithheldRowTests.MemberText(dialog, "private void DeleteFolder()");
         Assert.True(delete != null, "AccountDialog.DeleteFolder is gone.");
         Assert.Contains("folderNames_[index]", delete, StringComparison.Ordinal);
         Assert.DoesNotContain("(not subscribed)", delete, StringComparison.Ordinal);
      }

      private static string ReadControlPanelFile(string relative) => File.ReadAllText(ControlPanelFile(relative));

      private static string ControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
