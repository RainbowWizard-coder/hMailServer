// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The administrator password was one masked box on the Administrative access
   /// page, written by the page's Save with no confirmation and no current password
   /// (review finding F168): a typo locked the administrator out of every client.
   /// These hold the dialog that replaced it - each box's own problem, in the order
   /// the boxes are on screen - and that the page writes the password nowhere else.
   /// </summary>
   public class AdministratorPasswordChangeTests
   {
      [Fact]
      public void TheCurrentPasswordIsAskedFor()
      {
         AdministratorPasswordChange.Problem problem = AdministratorPasswordChange.Check("", "new one", "new one");
         Assert.NotNull(problem);
         Assert.Equal(AdministratorPasswordChange.Box.Current, problem.Box);
      }

      [Fact]
      public void AnEmptyNewPassword_IsRefused_RatherThanKeepingTheOldOne()
      {
         AdministratorPasswordChange.Problem problem = AdministratorPasswordChange.Check("old", "", "");
         Assert.NotNull(problem);
         Assert.Equal(AdministratorPasswordChange.Box.New, problem.Box);
      }

      [Theory]
      [InlineData("Correct horse", "Correct hors")]
      [InlineData("Correct horse", "correct horse")]
      [InlineData("Correct horse", "Correct horse ")]
      [InlineData("Correct horse", "")]
      [InlineData("Correct horse", null)]
      public void ATypoInTheConfirmation_IsSaidOnTheConfirmation(string newPassword, string confirmation)
      {
         AdministratorPasswordChange.Problem problem = AdministratorPasswordChange.Check("old", newPassword, confirmation);
         Assert.NotNull(problem);
         Assert.Equal(AdministratorPasswordChange.Box.Confirm, problem.Box);
         Assert.False(string.IsNullOrWhiteSpace(problem.Sentence));
      }

      [Fact]
      public void ThreeBoxesThatAgree_AreReadyForTheServer()
      {
         Assert.Null(AdministratorPasswordChange.Check("old", " spaces count ", " spaces count "));
      }

      /// <summary>
      /// The Administrative access page writes the password only through the dialog:
      /// no box on the page that its Save writes, and a dialog that puts the current
      /// password to the server before it writes the new one, and carries a session
      /// signed in as the administrator on to it.
      /// </summary>
      [Fact]
      public void ThePasswordIsChangedOnlyInTheDialog_AfterTheCurrentOneIsChecked()
      {
         string page = ReadControlPanelFile("Views/ServerSettingsView.xaml.cs");
         string build = WithheldRowTests.MemberText(page, "private void BuildAdminAccess()");
         Assert.True(build != null, "BuildAdminAccess is gone.");
         Assert.DoesNotContain("new ComPassword", build, StringComparison.Ordinal);
         Assert.DoesNotContain("MethodName = \"SetAdministratorPassword\"", build, StringComparison.Ordinal);
         Assert.Contains("new AdministratorPasswordDialog(", build, StringComparison.Ordinal);

         string dialog = ReadControlPanelFile("Views/AdministratorPasswordDialog.cs");
         int checks = dialog.IndexOf("AdministratorPasswordChange.Check(", StringComparison.Ordinal);
         int asks = dialog.IndexOf("CheckAdministratorPassword(", StringComparison.Ordinal);
         int writes = dialog.IndexOf("SetAdministratorPassword(", StringComparison.Ordinal);
         int carries = dialog.IndexOf("AdministratorPasswordChanged(", StringComparison.Ordinal);
         Assert.True(checks >= 0 && asks > checks && writes > asks && carries > writes,
            "The dialog writes the new password before it has checked the boxes and the current password, or does not carry the session on to it.");
      }

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative + " could not be found from the test binaries.");
         return null;
      }
   }
}
