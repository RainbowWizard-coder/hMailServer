// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The account dialog's Folders tab and Directory entry tab, and the distribution
   /// list members dialog, in their empty and nothing-to-do states (review findings
   /// F103, F110 and F111): the folder list was a blank 220 pixels over "0 folders.",
   /// Remove the photo was live under "This account has no photo.", and the members
   /// dialog's heading never said how many members the list has. Each test fails
   /// against the dialogs as they were.
   /// </summary>
   public class AccountDialogStatesTests
   {
      [Fact]
      public void TheFoldersList_HasAnEmptyStateInItsPlace_ShownAfterAReadThatWorked()
      {
         string dialog = Read("AccountDialog.cs");
         Assert.Contains("private readonly EmptyState folderEmpty_", dialog, StringComparison.Ordinal);

         string build = WithheldRowTests.MemberText(dialog, "private FrameworkElement BuildFolders()");
         Assert.Contains("folderHost.Children.Add(folderList_);", build, StringComparison.Ordinal);
         Assert.Contains("folderHost.Children.Add(folderEmpty_);", build, StringComparison.Ordinal);
         Assert.Contains("Label(L(\"_IMAP folders in this mailbox\"), folderHost)", build, StringComparison.Ordinal);

         string load = WithheldRowTests.MemberText(dialog, "private void LoadFolders()");
         Assert.Contains("StatusText.Show(folderEmpty_, null, count, null,", load, StringComparison.Ordinal);
         Assert.Contains("L(\"No folders yet. The server creates INBOX when the mailbox is first opened; Add folder creates one now.\")", load, StringComparison.Ordinal);

         // A read that failed claims no emptiness.
         string failed = load[load.IndexOf("catch (Exception ex)", StringComparison.Ordinal)..];
         Assert.Contains("folderEmpty_.Visibility = Visibility.Collapsed;", failed, StringComparison.Ordinal);

         // The count goes through the catalogue's plural forms, never English text.
         Assert.Contains("Plural(count, \"{0} folder.\", \"{0} folders.\")", load, StringComparison.Ordinal);
         Assert.DoesNotContain("\" folders.\"", dialog, StringComparison.Ordinal);
      }

      [Fact]
      public void RemoveThePhoto_IsEnabledOnlyWhileThereIsAPhotoToRemove()
      {
         string dialog = Read("AccountDialog.cs");
         string refresh = WithheldRowTests.MemberText(dialog, "private void RefreshDirectoryPhotoState_()");
         Assert.True(refresh != null, "AccountDialog.RefreshDirectoryPhotoState_ is gone.");
         Assert.Contains("dirPhotoRemove_.IsEnabled = has;", refresh, StringComparison.Ordinal);

         string build = WithheldRowTests.MemberText(dialog, "private ScrollViewer BuildDirectoryEntry()");
         Assert.Contains("dirPhotoRemove_ = remove;", build, StringComparison.Ordinal);
         Assert.True(build.IndexOf("dirPhotoRemove_ = remove;", StringComparison.Ordinal) < build.IndexOf("RefreshDirectoryPhotoState_();", StringComparison.Ordinal),
            "The tab sets the photo state before it knows the Remove button, so the button starts enabled.");
      }

      [Fact]
      public void TheMembersDialog_CountsTheMembersUnderItsHeading_AfterAReadThatWorked()
      {
         string dialog = Read("RecipientsDialog.cs");
         Assert.Contains("frame_ = UseFrame(", dialog, StringComparison.Ordinal);

         string reload = WithheldRowTests.MemberText(dialog, "private void Reload()");
         Assert.Contains("frame_.Description = read ? Plural(rows.Count, \"{0} member\", \"{0} members\") : null;", reload, StringComparison.Ordinal);
      }

      private static string Read(string file) => HostedAddressTests.ReadControlPanelFile(Path.Join("Views", file));
   }
}
