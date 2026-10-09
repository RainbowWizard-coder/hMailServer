// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The access-control entry dialog resolves its account or group before it closes
   /// (review finding F205). Save closed the entry dialog, the folder's dialog then
   /// looked the name up and, finding none, said "No account found with address
   /// '...'" in its own notice with the entry typed gone; the account or group was
   /// free text with nothing to pick from; and the eleven IMAP rights were offered
   /// raw, one column at a 40-pixel pitch, with no way to set the usual three sets.
   /// </summary>
   public class PermissionEntryDialogTests
   {
      [Fact]
      public void TheEntry_ResolvesItsAccountOrGroup_OnItsBox_BeforeItCloses()
      {
         string commit = Member("private void Commit()");
         int resolve = commit.IndexOf("SubjectId = Resolve(SelectedType, Subject);", StringComparison.Ordinal);
         int refused = commit.IndexOf("DialogFields.ShowError(subjectRow_, SelectedType == 1", resolve < 0 ? 0 : resolve, StringComparison.Ordinal);
         int closes = commit.IndexOf("DialogResult = true;", StringComparison.Ordinal);
         Assert.True(resolve >= 0 && refused > resolve && closes > refused, "The entry closes before it resolves its account or group.");
         Assert.Contains("F(\"No group found named '{0}'.\", Subject)", commit, StringComparison.Ordinal);
         Assert.Contains("F(\"No account found with address '{0}'.\", Subject)", commit, StringComparison.Ordinal);
         Assert.Contains("L(\"Enter the group's name.\") : L(\"Enter the account's address.\")", commit, StringComparison.Ordinal);

         // The folder's dialog writes what the entry resolved, and says nothing of it itself.
         string add = Member("private void AddOrEdit(int existingId)");
         Assert.Contains("Resolve = (type, subject) => type == 0 ? ResolveAccountId(subject) : ResolveGroupId(subject),", add, StringComparison.Ordinal);
         Assert.Contains("int subjectId = dlg.SubjectId;", add, StringComparison.Ordinal);
         Assert.DoesNotContain("No account found with address", add, StringComparison.Ordinal);
         Assert.DoesNotContain("No group found named", add, StringComparison.Ordinal);
      }

      [Fact]
      public void TheAccountOrGroup_IsPickedFromTheServers_OrTyped()
      {
         string source = Source();
         Assert.Contains("private readonly ComboBox subject_ = new() { IsEditable = true, IsTextSearchEnabled = true };", source, StringComparison.Ordinal);
         Assert.Contains("Suggest = SubjectsOf_", source, StringComparison.Ordinal);
         string offer = Member("private void Offer_()");
         Assert.Contains("string typed = subject_.Text;", offer, StringComparison.Ordinal);
         Assert.Contains("subject_.Text = typed;", offer, StringComparison.Ordinal);
      }

      [Fact]
      public void ThePresets_TickTheirRights_AndTheRightsAreInTwoColumns()
      {
         string source = Source();
         Assert.Contains("Preset_(new Wpf.Ui.Controls.Button { Content = L(\"_Read only\") }, FolderRightPresets.ReadOnly,", source, StringComparison.Ordinal);
         Assert.Contains("Preset_(new Wpf.Ui.Controls.Button { Content = L(\"Read and _write\") }, FolderRightPresets.ReadAndWrite,", source, StringComparison.Ordinal);
         Assert.Contains("Preset_(new Wpf.Ui.Controls.Button { Content = L(\"_Full control\") }, FolderRightPresets.FullControl,", source, StringComparison.Ordinal);
         Assert.Contains("box.IsChecked = (rights & bit) != 0;", source, StringComparison.Ordinal);
         Assert.Contains("Grid.SetColumn(cell, i % 2);", source, StringComparison.Ordinal);
         Assert.Contains("Grid.SetRow(cell, i / 2);", source, StringComparison.Ordinal);
      }

      [Fact]
      public void ThePresets_AreTheRightsTheirNamesSay()
      {
         Assert.Equal(1 | 2 | 4, FolderRightPresets.ReadOnly);
         Assert.Equal(1 | 2 | 4 | 8 | 16 | 256 | 512, FolderRightPresets.ReadAndWrite);
         Assert.Equal(0, FolderRightPresets.ReadAndWrite & (FolderRightPresets.Post | FolderRightPresets.Administer | FolderRightPresets.CreateFolders | FolderRightPresets.DeleteFolder));

         // Full control is every right the dialog offers, and the bits are the dialog's own.
         string source = Source();
         int all = Regex.Matches(source, @"\(L\(""[^""]+""\), (\d+), ").Cast<Match>().Select(m => int.Parse(m.Groups[1].Value, System.Globalization.CultureInfo.InvariantCulture)).Aggregate(0, (a, b) => a | b);
         Assert.Equal(2047, all);
         Assert.Equal(FolderRightPresets.FullControl, all);
      }

      private static string Source() => FormLineAssert.ReadControlPanelFile(Path.Join("Views", "FolderPermissionsDialog.cs"));

      private static string Member(string signature)
      {
         string member = WithheldRowTests.MemberText(Source(), signature);
         Assert.True(member != null, "FolderPermissionsDialog.cs: " + signature + " is gone.");
         return member;
      }
   }
}
