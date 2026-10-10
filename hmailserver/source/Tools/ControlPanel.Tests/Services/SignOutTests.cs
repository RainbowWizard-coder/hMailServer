// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Control Panel signs out, connects to another server and signs in again
   /// without being closed (review finding F012). The administrator's menu held who
   /// was signed in, the version, Language, Theme and About, and nothing ended the
   /// session; the sign-in page came back only after a cancelled second factor -
   /// which left the session the current one, signed in on its password - and with
   /// the link lost the pill's tool tip and name said "Connection lost" again and
   /// nothing offered a way back. Scanned rather than rendered: this assembly has no
   /// WPF. Each test fails against the tree before the change.
   /// </summary>
   public class SignOutTests
   {
      [Fact]
      public void TheAdministratorsMenu_SignsOut_AndConnectsToAnotherServer()
      {
         string menu = Member(Read("MainWindow.xaml.cs"), "private void Admin_Click(object sender, RoutedEventArgs e)");
         Assert.Contains("new MenuItem { Header = L(\"Sign out\") }", menu, StringComparison.Ordinal);
         Assert.Contains("new MenuItem { Header = L(\"Connect to another server…\") }", menu, StringComparison.Ordinal);
         Assert.Contains("SignOut_(anotherServer: false)", menu, StringComparison.Ordinal);
         Assert.Contains("SignOut_(anotherServer: true)", menu, StringComparison.Ordinal);
      }

      [Fact]
      public void WhileTheLinkIsLost_SignInAgain_IsAtTheTopOfTheMenu()
      {
         string menu = Member(Read("MainWindow.xaml.cs"), "private void Admin_Click(object sender, RoutedEventArgs e)");
         int lost = menu.IndexOf("if (linkState_ == LinkState.Lost)", StringComparison.Ordinal);
         int again = menu.IndexOf("L(\"Sign in again\")", StringComparison.Ordinal);
         int who = menu.IndexOf("F(\"Connected to {0} as {1}\"", StringComparison.Ordinal);
         Assert.True(lost > 0 && again > lost && who > again, "Sign in again is not the first item of the menu while the link is lost.");

         string show = Member(Read("MainWindow.xaml.cs"), "private void ShowLinkState_(LinkState state)");
         Assert.Contains("linkState_ = state;", show, StringComparison.Ordinal);
      }

      [Fact]
      public void SigningOut_AsksAboutUnsavedChanges_ThenEndsTheSession()
      {
         string window = Read("MainWindow.xaml.cs");
         string signOut = Member(window, "private void SignOut_(bool anotherServer)");
         Assert.Contains("if (!MayLeaveCurrentPage_())", signOut, StringComparison.Ordinal);

         string show = Member(window, "private void ShowSignIn_(string host, string userName, bool focusServer)");
         Assert.True(show != null, "MainWindow.ShowSignIn_ is gone.");
         foreach (string step in new[]
         {
            "connected_ = false;", "EndTour(record: false);", "page.OnLeave();", "pageCache_.Clear();", "history_.Clear();",
            "NavTree.IsEnabled = false;", "SearchButton.IsEnabled = false;", "HelpButton.IsEnabled = false;",
            "BreadcrumbBar.Visibility = Visibility.Collapsed;", "HidePendingBar_();",
            "ServerSession.Current?.SignOut();", "ServerSession.SetCurrent(null);", "ShowLinkState_(LinkState.NotConnected);",
            "new ConnectView(OnConnected, host, userName, focusServer)"
         })
         {
            Assert.Contains(step, show, StringComparison.Ordinal);
         }

         // The session ends before the sign-in page is shown.
         Assert.True(show.IndexOf("ServerSession.SetCurrent(null);", StringComparison.Ordinal) < show.IndexOf("new ConnectView(", StringComparison.Ordinal),
            "The sign-in page is shown before the session has ended.");
      }

      [Fact]
      public void ARefusedSecondFactor_EndsTheSession_TheSameWay()
      {
         string connected = Member(Read("MainWindow.xaml.cs"), "private void OnConnected()");
         int verify = connected.IndexOf("if (!VerifyTwoFactor())", StringComparison.Ordinal);
         int signIn = connected.IndexOf("ShowSignIn_(", StringComparison.Ordinal);
         Assert.True(verify >= 0 && signIn > verify, "A refused second factor does not go back to the sign-in page through ShowSignIn_.");
         Assert.DoesNotContain("ContentHost.Content = new ConnectView(OnConnected);", connected, StringComparison.Ordinal);
      }

      [Fact]
      public void TheSession_LetsGoOfTheServer_AndForgetsThePassword()
      {
         string signOut = Member(Read(Path.Join("Services", "ServerSession.cs")), "public void SignOut()");
         Assert.True(signOut != null, "ServerSession.SignOut is gone.");
         Assert.Contains("app_ = null;", signOut, StringComparison.Ordinal);
         Assert.Contains("password_?.Dispose();", signOut, StringComparison.Ordinal);
         Assert.Contains("password_ = null;", signOut, StringComparison.Ordinal);
         Assert.Contains("Release((object)app);", signOut, StringComparison.Ordinal);
      }

      [Fact]
      public void TheSignInPage_ComesBackOnTheServerAndTheAccount_ReadyForWhatComesNext()
      {
         string connect = Read(Path.Join("Views", "ConnectView.xaml.cs"));
         string again = Member(connect, "public ConnectView(Action onConnected, string host, string userName, bool focusServer)");
         Assert.True(again != null, "ConnectView has no sign-in page for a session that ended.");
         Assert.Contains("HostBox.Text = host;", again, StringComparison.Ordinal);
         Assert.Contains("UserBox.Text = userName;", again, StringComparison.Ordinal);
         Assert.Contains("HostBox.SelectAll();", connect, StringComparison.Ordinal);
      }

      private static string Member(string body, string signature) => WithheldRowTests.MemberText(body, signature);

      private static string Read(string relative)
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
