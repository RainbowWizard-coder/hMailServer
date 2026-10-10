// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The sign-in page and the window's title (review findings F029, F030, F031 and
   /// F033): a refused password was reported as Critical, as a server that could not
   /// be reached was; Enter was caught by a KeyDown handler on the whole page rather
   /// than pressing a default button; the busy line was no live region; every launch
   /// opened on localhost and Administrator; and the window never said which server
   /// it managed.
   ///
   /// The rules are SignInMemory's and are held here as functions; the page and the
   /// window are held to using them by their source, since this assembly has no WPF.
   /// The registry itself is not written: a test that did would change the sign-in
   /// page of the computer it ran on.
   /// </summary>
   public class SignInMemoryTests
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";
      private static readonly XNamespace Ui = "http://schemas.lepo.co/wpfui/2022/xaml";

      [Fact]
      public void NothingKept_OpensOnLocalhostAndAdministrator()
      {
         Assert.Equal(("localhost", "Administrator"), SignInMemory.Prefill(null, null));
         Assert.Equal(("localhost", "Administrator"), SignInMemory.Prefill("", "   "));
         // A value of another type is somebody else's, and is not a server's name.
         Assert.Equal(("localhost", "Administrator"), SignInMemory.Prefill(42, new byte[] { 1 }));
      }

      [Fact]
      public void WhatWasKept_IsWhatThePageOpensWith()
      {
         Assert.Equal(("mail.example.com", "admin@example.com"), SignInMemory.Prefill(" mail.example.com ", "admin@example.com"));
         // Each falls back on its own.
         Assert.Equal(("mail.example.com", "Administrator"), SignInMemory.Prefill("mail.example.com", null));
         Assert.Equal(("localhost", "admin@example.com"), SignInMemory.Prefill(null, "admin@example.com"));
      }

      /// <summary>The server and the user name are kept, and nothing else can be: Remember is given no password.</summary>
      [Fact]
      public void Remember_IsGivenTheServerAndTheUserName_AndNoPassword()
      {
         MethodInfo remember = typeof(SignInMemory).GetMethod(nameof(SignInMemory.Remember));
         Assert.NotNull(remember);
         Assert.Equal(new[] { "host", "user" }, remember.GetParameters().Select(p => p.Name).ToArray());
         Assert.Equal("LastHost", SignInMemory.HostValue);
         Assert.Equal("LastUser", SignInMemory.UserValue);
         Assert.Equal(@"Software\hMailServer\ControlPanel", SignInMemory.RegistryPath);
      }

      [Fact]
      public void TheTitle_NamesTheServer_AndTheProductAloneBeforeSignIn()
      {
         Assert.Equal("mail.example.com - hMailServer Control Panel", SignInMemory.WindowTitle("mail.example.com"));
         Assert.Equal("hMailServer Control Panel", SignInMemory.WindowTitle(null));
         Assert.Equal("hMailServer Control Panel", SignInMemory.WindowTitle(" "));
      }

      [Fact]
      public void ARefusedCredentialOrACancelledPrompt_IsAWarning_AndAServerNotAsked_IsCritical()
      {
         Assert.Equal(StatusLevel.Warning, SignInMemory.FailureLevel(refused: true, cancelled: false));
         Assert.Equal(StatusLevel.Warning, SignInMemory.FailureLevel(refused: false, cancelled: true));
         Assert.Equal(StatusLevel.Critical, SignInMemory.FailureLevel(refused: false, cancelled: false));
      }

      /// <summary>
      /// The page opens on what was kept, fails at the level of the cause, and
      /// announces its busy line; Enter is the default button's.
      /// </summary>
      [Fact]
      public void ThePage_PrefillsFailsAtTheCausesLevel_AndAnnouncesItsBusyLine()
      {
         string code = ConnectPageTestsSource("ConnectView.xaml.cs");

         Assert.Contains("(string host, string user) = SignInMemory.Read();", code, StringComparison.Ordinal);
         Assert.Contains("HostBox.Text = host;", code, StringComparison.Ordinal);
         Assert.Contains("UserBox.Text = user;", code, StringComparison.Ordinal);

         // No page-wide Enter: the default button takes it.
         Assert.DoesNotMatch(new Regex(@"(?m)^\s*(this\.)?KeyDown \+="), code);
         Assert.Contains("ConnectButton.IsDefault = true;", code, StringComparison.Ordinal);

         string fail = WithheldRowTests.MemberText(code, "private void Fail_(string message, StatusLevel level)");
         Assert.True(fail != null, "ConnectView.Fail_ takes no level.");
         Assert.Contains("ErrorNotice.Level = level;", fail, StringComparison.Ordinal);
         Assert.Contains("SignInMemory.FailureLevel(session.CredentialRefused, cancelled)", code, StringComparison.Ordinal);
         Assert.Contains("Fail_(reach.Error, StatusLevel.Critical);", code, StringComparison.Ordinal);

         string busy = WithheldRowTests.MemberText(code, "private void SetBusy_(bool busy, string message)");
         Assert.True(busy != null, "ConnectView.SetBusy_ is gone.");
         Assert.Contains("Announce.Request(BusyText);", busy, StringComparison.Ordinal);
         // And as the line changes to Signing in, after the password and after the code.
         Assert.Equal(2, CountOf(code, "BusyText.Text = L(\"Signing in…\");\n            Announce.Request(BusyText);")
            + CountOf(code, "BusyText.Text = L(\"Signing in…\");\n         Announce.Request(BusyText);"));

         string session = ReadControlPanelFile(Path.Join("Services", "ServerSession.cs"));
         Assert.Contains("CredentialRefused = error == AuthenticationFailed_;", session, StringComparison.Ordinal);
         Assert.Contains("CredentialRefused = false;", session, StringComparison.Ordinal);
      }

      [Fact]
      public void EnterOnTheClosedLanguageBox_OpensItsList_AndSignsNothingIn()
      {
         // The default button takes every Enter that the control with the keyboard
         // leaves unhandled, and a closed ComboBox leaves it: Enter on the Language box
         // still started a sign-in, as the page-wide handler did (F030). The box opens
         // its list on Enter instead, and no other control's Enter is caught.
         string code = ConnectPageTestsSource("ConnectView.xaml.cs");
         Assert.Contains("LanguageBox.KeyDown += Language_KeyDown;", code, StringComparison.Ordinal);

         string enter = WithheldRowTests.MemberText(code, "private void Language_KeyDown(object sender, System.Windows.Input.KeyEventArgs e)");
         Assert.True(enter != null, "ConnectView.Language_KeyDown is gone.");
         // Only on the closed box: in the open list Enter chooses the item, the box's own.
         Assert.Contains("if (e.Key != System.Windows.Input.Key.Enter || LanguageBox.IsDropDownOpen)\n            return;", enter, StringComparison.Ordinal);
         Assert.Contains("LanguageBox.IsDropDownOpen = true;\n         e.Handled = true;", enter, StringComparison.Ordinal);

         // And the page names Enter nowhere else, nor hooks a key on another control.
         Assert.Equal(1, CountOf(code, "Key.Enter"));
         Assert.Equal(1, CountOf(code, "KeyDown +="));
         Assert.DoesNotContain("KeyDown=", ConnectPageTestsSource("ConnectView.xaml"), StringComparison.Ordinal);
      }

      [Fact]
      public void TheBusyLine_IsAPoliteLiveRegion_AndTheRingIsNamed()
      {
         XElement page = XDocument.Parse(ConnectPageTestsSource("ConnectView.xaml")).Root;
         XElement busy = page.Descendants(Wpf + "TextBlock").Single(e => (string)e.Attribute(X + "Name") == "BusyText");
         Assert.Equal("Polite", (string)busy.Attribute("AutomationProperties.LiveSetting"));

         XElement ring = page.Descendants(Ui + "ProgressRing").Single();
         Assert.Equal("{loc:L 'Signing in'}", (string)ring.Attribute("AutomationProperties.Name"));
      }

      /// <summary>
      /// The window's title follows the link - the server's name while signed in,
      /// the product alone on the sign-in page - the title bar shows the window's
      /// title, and a sign-in that worked is kept for the next launch.
      /// </summary>
      [Fact]
      public void TheWindow_NamesItsServer_AndKeepsTheSignIn()
      {
         string window = ReadControlPanelFile("MainWindow.xaml.cs");
         string show = WithheldRowTests.MemberText(window, "private void ShowLinkState_(LinkState state)");
         Assert.True(show != null, "MainWindow.ShowLinkState_ is gone.");
         Assert.Contains("string host = state == LinkState.NotConnected ? null : session?.Host;", show, StringComparison.Ordinal);
         Assert.Contains("Title = SignInMemory.WindowTitle(host);", show, StringComparison.Ordinal);

         string connected = WithheldRowTests.MemberText(window, "private void OnConnected()");
         Assert.True(connected != null, "MainWindow.OnConnected is gone.");
         int verified = connected.IndexOf("connected_ = true;", StringComparison.Ordinal);
         int kept = connected.IndexOf("SignInMemory.Remember(signedIn.Host, signedIn.UserName);", StringComparison.Ordinal);
         Assert.True(verified >= 0 && kept > verified, "The sign-in is not kept once it is verified.");

         XElement xaml = XDocument.Parse(ReadControlPanelFile("MainWindow.xaml")).Root;
         XElement bar = xaml.Descendants(Ui + "TitleBar").Single();
         Assert.Equal("{Binding Title, RelativeSource={RelativeSource AncestorType={x:Type Window}}}", (string)bar.Attribute("Title"));
      }

      /// <summary>
      /// Setup asks for an open Control Panel to be closed before it installs, and
      /// found one by its window's title alone, "hMailServer Control Panel" - which a
      /// signed-in window's title no longer is, and a Japanese, Swedish, Turkish or
      /// Chinese one never was. The Control Panel holds a named mutex from its start,
      /// and setup looks for that mutex, by the same name, as well as for the title.
      /// </summary>
      [Fact]
      public void Setup_FindsAnOpenControlPanel_ByTheMutexItHolds_AsWellAsByItsTitle()
      {
         string mark = ReadControlPanelFile(Path.Join("Services", "ControlPanelRunningMark.cs"));
         Match name = Regex.Match(mark, "public const string MutexName = \"([^\"\\\\]+)\";");
         Assert.True(name.Success, "ControlPanelRunningMark.MutexName is gone.");
         Assert.Contains("held_ = new Mutex(false, MutexName);", mark, StringComparison.Ordinal);

         // Held from the start, before the first window, until the process ends.
         string startup = WithheldRowTests.MemberText(ReadControlPanelFile("App.xaml.cs"), "protected override void OnStartup(StartupEventArgs e)");
         Assert.True(startup != null, "App.OnStartup is gone.");
         Assert.Contains("ControlPanelRunningMark.Hold();", startup, StringComparison.Ordinal);
         Assert.DoesNotContain("held_.Dispose", mark, StringComparison.Ordinal);
         Assert.DoesNotContain("ReleaseMutex", mark, StringComparison.Ordinal);

         // Setup's check is one condition: the mutex, by the same name, or the title.
         string setup = ReadSetupScript_();
         string check = setup.Split('\n').SingleOrDefault(line => line.Contains("FindWindowByWindowName('hMailServer Control Panel')", StringComparison.Ordinal));
         Assert.True(check != null, "Setup no longer looks for the Control Panel's window.");
         Assert.Contains("CheckForMutexes('" + name.Groups[1].Value + "')", check, StringComparison.Ordinal);
         Assert.Contains("(not g_bAdministratorUpdate) and (CheckForMutexes(", check, StringComparison.Ordinal);
      }

      private static string ReadSetupScript_()
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "installation", "hMailServerInnoExtension.iss");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate).Replace("\r\n", "\n");
         }

         Assert.Fail("installation/hMailServerInnoExtension.iss could not be found from the test binaries.");
         return null;
      }

      private static int CountOf(string text, string part)
      {
         int count = 0;
         for (int at = text.IndexOf(part, StringComparison.Ordinal); at >= 0; at = text.IndexOf(part, at + part.Length, StringComparison.Ordinal))
            count++;
         return count;
      }

      private static string ConnectPageTestsSource(string file) => ReadControlPanelFile(Path.Join("Views", file));

      private static string ReadControlPanelFile(string relative)
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate).Replace("\r\n", "\n");
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
