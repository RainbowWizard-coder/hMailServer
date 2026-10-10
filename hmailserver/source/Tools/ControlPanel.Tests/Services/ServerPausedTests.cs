// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// "Pause" means one thing on each page that has it, and a paused server is said
   /// under every page (review finding F019). Server status's Pause, which stops the
   /// server's engine - no connection accepted, nothing delivered - and Live logs'
   /// Pause, which freezes the view, were the same word with the same glyph on
   /// neighbouring pages; and once the engine was paused the shell's pill still said
   /// Connected in green on every page, since only the link fed it and nothing outside
   /// Server status read the server's state, so a forgotten pause was a server that
   /// took no mail with no sign of it anywhere. Scanned rather than rendered: this
   /// assembly has no WPF. Each test but the pill's words fails against the tree
   /// before the change.
   /// </summary>
   public class ServerPausedTests
   {
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";

      [Fact]
      public void ServerStatusSaysWhatItPauses_AndLiveLogsThatItPausesTheView()
      {
         string status = Read("Views", "StatusView.xaml.cs");
         Assert.Contains("L(\"_Pause the mail server\")", status, StringComparison.Ordinal);
         Assert.Contains("L(\"R_esume the mail server\")", status, StringComparison.Ordinal);
         Assert.DoesNotContain("the mail server engine\", MnemonicText.Strip", status, StringComparison.Ordinal);
         Assert.Contains("Content=\"{loc:L '_Pause the mail server'}\"", Read("Views", "StatusView.xaml"), StringComparison.Ordinal);

         string logs = Read("Views", "LogsView.xaml.cs");
         Assert.Contains("L(\"R_esume view\") : L(\"_Pause view\")", logs, StringComparison.Ordinal);
         Assert.Contains("SymbolRegular.PlayCircle24 : Wpf.Ui.Controls.SymbolRegular.PauseCircle24", logs, StringComparison.Ordinal);
         string logsXaml = Read("Views", "LogsView.xaml");
         Assert.Contains("Content=\"{loc:L '_Pause view'}\"", logsXaml, StringComparison.Ordinal);
         Assert.Contains("Symbol=\"PauseCircle24\"", logsXaml, StringComparison.Ordinal);
      }

      /// <summary>
      /// Server status's State names the paused engine as the pill, the bar and the
      /// buttons on the same screen do: it read "State: Stopped" under a "Paused" pill
      /// and beside "The mail server is paused", in German "Angehalten" under
      /// "Pausiert".
      /// </summary>
      [Fact]
      public void ServerStatusesState_IsPaused_AsThePillSaysIt()
      {
         string state = WithheldRowTests.MemberText(Read("Views", "StatusView.xaml.cs"), "private static string ServerStateName(int state)");
         Assert.True(state != null, "StatusView.ServerStateName is gone.");
         Assert.Contains("1 => L(\"Paused\"),", state, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Stopped\")", state, StringComparison.Ordinal);
         Assert.Equal("Paused", ConnectionStatus.PausedWord);
      }

      [Fact]
      public void APausedServer_IsABarAtTheFootOfTheWindow_WithResume()
      {
         XDocument window = XDocument.Parse(Read("MainWindow.xaml"));
         XElement bar = window.Descendants().SingleOrDefault(e => (string)e.Attribute(X + "Name") == "PausedBar");
         Assert.True(bar != null, "The window has no paused server's bar.");
         Assert.Equal("Collapsed", (string)bar.Attribute("Visibility"));
         XElement resume = bar.Descendants().Single(e => (string)e.Attribute(X + "Name") == "ResumeServerButton");
         Assert.Equal("ResumePausedServer_Click", (string)resume.Attribute("Click"));
         XElement notice = bar.Descendants().Single(e => (string)e.Attribute(X + "Name") == "PausedNotice");
         Assert.Equal("Warning", (string)notice.Attribute("Level"));
         Assert.Equal("{loc:L 'The mail server is paused - nothing is accepted or delivered.'}", (string)notice.Attribute("Text"));

         string paused = Read("MainWindow.ServerPaused.cs");
         Assert.Contains("public void ShowServerState(int state) => ShowServerPaused_(connected_ && state == ServerStateStopped_);", paused, StringComparison.Ordinal);
         Assert.Contains("private const int ServerStateStopped_ = 1;", paused, StringComparison.Ordinal);
         Assert.Contains("ShowServerState((int)session.Application.ServerState);", paused, StringComparison.Ordinal);
         Assert.Contains("session.Application.Start();", paused, StringComparison.Ordinal);
         Assert.Contains("PausedBar.Visibility = paused ? Visibility.Visible : Visibility.Collapsed;", paused, StringComparison.Ordinal);
      }

      [Fact]
      public void ThePillSaysPaused_AndTheStateIsAskedOnEveryPageAndTheDashboardsRefresh()
      {
         string window = Read("MainWindow.xaml.cs");
         string show = WithheldRowTests.MemberText(window, "private void ShowLinkState_(LinkState state)");
         Assert.Contains("bool paused = serverPaused_ && state == LinkState.Connected;", show, StringComparison.Ordinal);
         Assert.Contains("ConnectionStatus.PausedWord", show, StringComparison.Ordinal);
         Assert.Contains("ConnectionStatus.PausedLevel", show, StringComparison.Ordinal);

         string enter = WithheldRowTests.MemberText(window, "private void EnterPage(object page)");
         Assert.Contains("RefreshServerPaused();", enter, StringComparison.Ordinal);
         string signIn = WithheldRowTests.MemberText(window, "private void ShowSignIn_(string host, string userName, bool focusServer)");
         Assert.Contains("ShowServerPaused_(false);", signIn, StringComparison.Ordinal);

         Assert.Contains("(Application.Current?.MainWindow as MainWindow)?.RefreshServerPaused();", Read("Views", "DashboardView.xaml.cs"), StringComparison.Ordinal);
         Assert.Contains("(Application.Current?.MainWindow as MainWindow)?.ShowServerState(state);", Read("Views", "StatusView.xaml.cs"), StringComparison.Ordinal);

         // F6 reaches the bar as a part of the foot of the window.
         string keys = Read("MainWindow.Keys.cs");
         Assert.Contains("IsWithin_(focused, PausedBar)", keys, StringComparison.Ordinal);
         Assert.Contains("FirstFocusable_(PausedBar)", keys, StringComparison.Ordinal);
      }

      /// <summary>
      /// The bar is on screen with every page, Server status's own R_esume the mail
      /// server among them, in the window's one access-key scope, so its button
      /// carries no key in any language, as the pending bar's do not. It does what
      /// that Resume does, and says it in the same words, less the key: on one
      /// screen the two read "Resume the mail server" and "Resume the server", in
      /// German "Mailserver fortsetzen" and "Server fortsetzen". A key written after
      /// the words, "(_E)" in Japanese and Chinese, goes with its brackets.
      /// </summary>
      [Fact]
      public void TheBarsButton_CarriesNoAltKey_InAnyLanguage()
      {
         const string Caption = "Resume the mail server";
         const string Header = "R_esume the mail server";
         Assert.Contains("Content=\"{loc:L '" + Caption + "'}\"", Read("MainWindow.xaml"), StringComparison.Ordinal);
         Assert.True(MnemonicText.Parse(Caption).KeyIndex < 0);
         Assert.Equal(Caption, MnemonicText.Strip(Header));
         Assert.Contains("L(\"" + Header + "\")", Read("Views", "StatusView.xaml.cs"), StringComparison.Ordinal);

         string[] catalogues = Directory.GetFiles(Path.Join(SaveStateModelTests.ControlPanelDirectory(), "Resources"), "Strings*.resx");
         Assert.True(catalogues.Length >= 18, "The catalogues were not found.");
         foreach (string catalogue in catalogues)
         {
            XElement data = XDocument.Load(catalogue).Root.Elements("data").FirstOrDefault(d => (string)d.Attribute("name") == Caption);
            Assert.True(data != null, Path.GetFileName(catalogue) + " has no entry for the bar's button.");
            string value = (string)data.Element("value") ?? "";
            Assert.True(value.Length > 0 && MnemonicText.Parse(value).KeyIndex < 0,
               Path.GetFileName(catalogue) + " gives the bar's button an Alt key, or no text: \"" + value + "\".");

            XElement header = XDocument.Load(catalogue).Root.Elements("data").FirstOrDefault(d => (string)d.Attribute("name") == Header);
            Assert.True(header != null, Path.GetFileName(catalogue) + " has no entry for Server status's Resume.");
            string words = MnemonicText.Strip(System.Text.RegularExpressions.Regex.Replace((string)header.Element("value") ?? "", @"\s*\(_.\)$", ""));
            Assert.True(value == words,
               Path.GetFileName(catalogue) + " says the bar's Resume as \"" + value + "\" and Server status's as \"" + words + "\".");
         }
      }

      [Fact]
      public void ThePausedPill_IsAWarning_InAWordNoLinkStateHas()
      {
         Assert.Equal(StatusLevel.Warning, ConnectionStatus.PausedLevel);
         foreach (LinkState state in ConnectionStatus.AllStates)
            Assert.NotEqual(ConnectionStatus.WordFor(state), ConnectionStatus.PausedWord);

         string sentence = ConnectionStatus.DescribePaused("mail.example.test", "Administrator");
         Assert.Contains("mail.example.test", sentence, StringComparison.Ordinal);
         Assert.Contains("paused", sentence, StringComparison.Ordinal);
         Assert.Equal(ConnectionStatus.PausedWord, ConnectionStatus.DescribePaused(null, null));
      }

      private static string Read(params string[] parts)
      {
         string relative = Path.Join(parts);
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
