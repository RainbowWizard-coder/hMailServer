// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The window's pending bar had one button, Apply now, which takes only the
   /// settings a reinitialise takes: the ones waiting for a service restart had
   /// nothing to press, though the feature settings pages had a restart of their
   /// own, and the bar never named the settings it counted (review finding F010).
   /// The restart is now one shared flow (Services/ServiceRestart), which the bar
   /// offers for the service on this machine and asks about first. Scanned rather
   /// than rendered: this assembly has no WPF.
   /// </summary>
   public class PendingBarRestartTests
   {
      [Fact]
      public void TheBar_OffersTheRestart_ForTheServiceOnThisMachine_AndNamesTheSettings()
      {
         string markup = SaveStateModelTests.ReadControlPanelFile("MainWindow.xaml");
         Assert.Contains("x:Name=\"RestartServiceButton\"", markup, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.AutomationId=\"restart-service-pending\"", markup, StringComparison.Ordinal);
         Assert.Contains("Click=\"RestartPending_Click\"", markup, StringComparison.Ordinal);

         string shell = SaveStateModelTests.ReadControlPanelFile("MainWindow.xaml.cs");
         string refresh = WithheldRowTests.MemberText(shell, "private void RefreshPendingBar_(bool fresh)");
         Assert.True(refresh != null, "RefreshPendingBar_ is gone.");
         Assert.Contains("waitingForRestart > 0 && ServiceRestart.Available", refresh, StringComparison.Ordinal);
         Assert.Contains("SettingApplies.PendingDetail(pending, IniFeatureStore.AppliesFor)", refresh, StringComparison.Ordinal);
         Assert.Contains("PendingNotice.ToolTip = detail", refresh, StringComparison.Ordinal);
         Assert.Contains("SetHelpText(PendingNotice, detail", refresh, StringComparison.Ordinal);
      }

      /// <summary>
      /// The bar's two buttons are met in the order they are drawn. Each was docked
      /// right on its own, so Apply now, declared first, was drawn rightmost and
      /// Restart the service… to its left, while Tab and UI Automation, which follow
      /// declaration order, reached Apply now first: right to left.
      /// </summary>
      [Fact]
      public void TheBarsButtons_AreReached_InTheOrderTheyAreDrawn()
      {
         string markup = SaveStateModelTests.ReadControlPanelFile("MainWindow.xaml");
         int bar = markup.IndexOf("x:Name=\"PendingBar\"", StringComparison.Ordinal);
         Assert.True(bar >= 0, "The pending bar is gone.");
         int end = markup.IndexOf("</Border>", bar, StringComparison.Ordinal);
         string body = markup.Substring(bar, end - bar);

         int panel = body.IndexOf("<StackPanel DockPanel.Dock=\"Right\" Orientation=\"Horizontal\">", StringComparison.Ordinal);
         int restart = body.IndexOf("x:Name=\"RestartServiceButton\"", StringComparison.Ordinal);
         int apply = body.IndexOf("x:Name=\"ApplyPendingButton\"", StringComparison.Ordinal);
         int panelEnd = body.IndexOf("</StackPanel>", StringComparison.Ordinal);
         int notice = body.IndexOf("x:Name=\"PendingNotice\"", StringComparison.Ordinal);

         Assert.True(panel >= 0, "The bar's buttons are not in one horizontal panel docked right.");
         Assert.True(panel < restart && restart < apply && apply < panelEnd,
            "The bar's buttons are not declared in the panel in the order they are drawn: Restart the service…, then Apply now.");
         Assert.True(panelEnd < notice, "The notice is not the bar's fill, after its buttons.");

         // Neither button is docked on its own, which is what reversed them.
         string buttons = body.Substring(restart, panelEnd - restart);
         Assert.DoesNotContain("DockPanel.Dock", buttons, StringComparison.Ordinal);
      }

      /// <summary>
      /// The bar is in the window's access-key scope, which every page shares, and
      /// stays up until what it counts is applied - a restart's until the service
      /// restarts. Restart the ser_vice… took Alt+V, which IP ranges' Save
      /// permissions, LDAP's certificate check, the backup's verification, the queue's
      /// View source and the audit trail's chain check carry; _Apply now took Alt+A,
      /// on eighteen pages; and in German, French and Russian the keys met Delete,
      /// Refresh and their neighbours on a score of pages. WPF, given two enabled
      /// targets for one key, moves the focus between them and presses neither. No
      /// letter is free on every page, so neither caption carries a key in any
      /// language, as ReadFailure's Try again does not, and check-mnemonics.py puts
      /// the bar's captions into the scope of every page the window shows, so that a
      /// key given back to one is reported where it collides.
      /// </summary>
      [Fact]
      public void TheBarsCaptions_CarryNoAltKey_InAnyLanguage()
      {
         string markup = SaveStateModelTests.ReadControlPanelFile("MainWindow.xaml");
         int bar = markup.IndexOf("x:Name=\"PendingBar\"", StringComparison.Ordinal);
         Assert.True(bar >= 0, "The pending bar is gone.");
         string body = markup.Substring(bar, markup.IndexOf("</Border>", bar, StringComparison.Ordinal) - bar);

         string[] captions = Regex.Matches(body, @"Content=""\{loc:L '((?:[^'\\]|\\.)*)'\}""").Select(m => m.Groups[1].Value).ToArray();
         Assert.Equal(new[] { "Restart the service…", "Apply now" }, captions);

         // The directory pages' notice offers the same restart under the same caption.
         string shared = SaveStateModelTests.ReadControlPanelFile("Views/IniPageSaveState.cs");
         string action = WithheldRowTests.MemberText(shared, "public Wpf.Ui.Controls.Button RestartAction(string automationId, string question)");
         Assert.True(action != null && action.Contains("Content = L(\"Restart the service…\")", StringComparison.Ordinal),
            "The directory pages' restart is not captioned as the bar's is.");

         string[] catalogues = Directory.GetFiles(Path.Join(SaveStateModelTests.ControlPanelDirectory(), "Resources"), "Strings*.resx");
         Assert.True(catalogues.Length >= 18, "The catalogues were not found.");
         foreach (string key in captions)
         {
            Assert.True(MnemonicText.Parse(key).KeyIndex < 0, "The bar's caption \"" + key + "\" carries an Alt key.");
            foreach (string catalogue in catalogues)
            {
               XElement data = XDocument.Load(catalogue).Root.Elements("data").FirstOrDefault(d => (string)d.Attribute("name") == key);
               Assert.True(data != null, Path.GetFileName(catalogue) + " has no entry for \"" + key + "\".");
               string value = (string)data.Element("value") ?? "";
               Assert.True(value.Length > 0 && MnemonicText.Parse(value).KeyIndex < 0,
                  Path.GetFileName(catalogue) + " gives the bar's caption \"" + key + "\" an Alt key, or no text: \"" + value + "\".");
            }
         }

         // The checker's two halves: both captions exempt from coverage, and the bar's
         // captions in the scope of every page the window shows.
         string checker = ReadRepositoryFile_(Path.Join("build", "check-mnemonics.py"));
         Assert.Contains("\"Apply now\", \"Restart the service…\"", checker, StringComparison.Ordinal);
         // The paused server's bar is one of the window's bars too (review finding F019).
         Assert.Contains("WINDOW_BARS = {\"MainWindow.xaml\": [\"PendingBar\", \"PausedBar\"]}", checker, StringComparison.Ordinal);
         Assert.Contains("placed += bar_captions(files)", checker, StringComparison.Ordinal);
      }

      [Fact]
      public void TheBarsRestart_AsksBeforeItStopsTheService()
      {
         string shell = SaveStateModelTests.ReadControlPanelFile("MainWindow.xaml.cs");
         string click = WithheldRowTests.MemberText(shell, "private async void RestartPending_Click(object sender, RoutedEventArgs e)");
         Assert.True(click != null, "RestartPending_Click is gone.");

         int asks = click.IndexOf("MessageBox.Confirm(SettingApplies.RestartQuestion(", StringComparison.Ordinal);
         int restarts = click.IndexOf("ServiceRestart.RunAsync()", StringComparison.Ordinal);
         Assert.True(asks >= 0 && restarts > asks, "The bar restarts the service without asking first.");
         Assert.Contains("cancelByDefault: true", click, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("Views/FeatureSettingsView.xaml.cs")]
      [InlineData("Views/IniPageSaveState.cs")]
      public void EveryRestart_GoesThroughTheSharedFlow(string file)
      {
         string code = SaveStateModelTests.ReadControlPanelFile(file);
         Assert.Contains("ServiceRestart.RunAsync()", code, StringComparison.Ordinal);
         Assert.DoesNotContain("new ServiceController(", code, StringComparison.Ordinal);
         Assert.DoesNotContain("TryRestartService", code, StringComparison.Ordinal);
      }

      /// <summary>
      /// The two directory pages offer the restart beside a saved schedule that waits
      /// for one - Directory synchronisation for its schedule, Directory authentication
      /// for the Enabled that is half of it - through one flow, which asks first and is
      /// offered only for the service on this machine.
      /// </summary>
      [Theory]
      [InlineData("Views/DirectorySyncView.cs", "dirsync-restart-service")]
      [InlineData("Views/LdapSettingsView.cs", "ldap-restart-service")]
      public void TheDirectoryPages_OfferTheRestart_WhenTheScheduleWaitsForIt(string file, string automationId)
      {
         string code = SaveStateModelTests.ReadControlPanelFile(file);
         string save = WithheldRowTests.MemberText(code, "private bool Save_()");
         Assert.True(save != null && save.Contains("action: RestartAction_()", StringComparison.Ordinal),
            file + ": a schedule that waits for a restart is not offered one.");

         string action = WithheldRowTests.MemberText(code, "private Wpf.Ui.Controls.Button RestartAction_()");
         Assert.True(action != null && action.Contains("saveState_.RestartAction(\"" + automationId + "\"", StringComparison.Ordinal),
            file + ": the restart is not the shared one.");
         Assert.DoesNotContain("ServiceRestart.RunAsync()", code, StringComparison.Ordinal);

         string shared = SaveStateModelTests.ReadControlPanelFile("Views/IniPageSaveState.cs");
         string offer = WithheldRowTests.MemberText(shared, "public Wpf.Ui.Controls.Button RestartAction(string automationId, string question)");
         Assert.True(offer != null && offer.Contains("if (!ServiceRestart.Available)", StringComparison.Ordinal),
            "The directory pages offer to restart a service they cannot reach.");

         string restart = WithheldRowTests.MemberText(shared, "private async Task RestartAsync_(string automationId, string question)");
         Assert.True(restart != null, "IniPageSaveState.RestartAsync_ is gone.");
         int asks = restart.IndexOf("MessageBox.Confirm(question,", StringComparison.Ordinal);
         int restarts = restart.IndexOf("ServiceRestart.RunAsync()", StringComparison.Ordinal);
         Assert.True(asks >= 0 && restarts > asks, "The directory pages restart the service without asking first.");
         Assert.Contains("cancelByDefault: true", restart, StringComparison.Ordinal);
      }

      /// <summary>
      /// The keyboard on a directory page's Restart the service…, pressed, went to the
      /// window as the restart began, when the notice dropped the button for the
      /// Restarting sentence: Tab started again from the window's first control, and a
      /// screen reader said nothing of where the keyboard was. It goes to the page's
      /// Save changes, the header's control above the notice - or to the action that
      /// replaces the one dropped - and counts as on the button while the window, not
      /// yet active again after the question, will give it back to it.
      /// </summary>
      [Fact]
      public void TheNoticesRestart_LeavesTheKeyboardOnThePage()
      {
         string shared = SaveStateModelTests.ReadControlPanelFile("Views/IniPageSaveState.cs");
         string say = WithheldRowTests.MemberText(shared, "private void Say_()");
         Assert.True(say != null, "IniPageSaveState.Say_ is gone.");
         int holds = say.IndexOf("HoldsKeyboard_(before)", StringComparison.Ordinal);
         int replaced = say.IndexOf("notice_.Content = action;", StringComparison.Ordinal);
         int keeps = say.IndexOf("KeepKeyboard_(", StringComparison.Ordinal);
         Assert.True(holds >= 0 && replaced > holds && keeps > replaced,
            "The notice does not look for the keyboard on its action before it drops it, and keep it after.");

         string remembered = WithheldRowTests.MemberText(shared, "private static bool HoldsKeyboard_(UIElement element)");
         Assert.True(remembered != null && remembered.Contains("FocusManager.GetFocusedElement(scope)", StringComparison.Ordinal),
            "The keyboard the window gives back after the question is not counted.");
         string keep = WithheldRowTests.MemberText(shared, "private void KeepKeyboard_(UIElement next)");
         Assert.True(keep != null && keep.Contains("home?.Focus();", StringComparison.Ordinal) && keep.Contains("next.Focus()", StringComparison.Ordinal),
            "The keyboard is not put on the next action or the page's home.");

         foreach (string file in new[] { "Views/LdapSettingsView.cs", "Views/DirectorySyncView.cs" })
            Assert.Contains("keyboardHome: () => saveButton_", SaveStateModelTests.ReadControlPanelFile(file), StringComparison.Ordinal);
      }

      /// <summary>
      /// The pending bar's two buttons are disabled while the work they start runs, and
      /// the bar is collapsed when nothing is left: either gave the keyboard on them to
      /// the window. The keyboard goes to the page first - where it last was, as F6
      /// takes it there - and comes back to the page once the page is entered again, if
      /// the element it went to was built away.
      /// </summary>
      [Theory]
      [InlineData("private async void RestartPending_Click(object sender, RoutedEventArgs e)")]
      [InlineData("private void ApplyPending_Click(object sender, RoutedEventArgs e)")]
      public void TheBarsButtons_SendTheKeyboardToThePage_BeforeTheyAreDisabled(string member)
      {
         string shell = SaveStateModelTests.ReadControlPanelFile("MainWindow.xaml.cs");
         string click = WithheldRowTests.MemberText(shell, member);
         Assert.True(click != null, member + " is gone.");

         int asks = click.IndexOf("MessageBox.Confirm(", StringComparison.Ordinal);
         int off = click.IndexOf("KeyboardOffPendingBar_();", StringComparison.Ordinal);
         int disabled = click.IndexOf("ApplyPendingButton.IsEnabled = false;", StringComparison.Ordinal);
         Assert.True(asks >= 0 && off > asks && disabled > off, member + " disables the bar's buttons with the keyboard still on them.");
      }

      [Fact]
      public void TheBar_IsCollapsedWithTheKeyboardOffIt()
      {
         string shell = SaveStateModelTests.ReadControlPanelFile("MainWindow.xaml.cs");
         string refresh = WithheldRowTests.MemberText(shell, "private void RefreshPendingBar_(bool fresh)");
         Assert.True(refresh != null, "RefreshPendingBar_ is gone.");
         Assert.DoesNotContain("PendingBar.Visibility = Visibility.Collapsed;", refresh, StringComparison.Ordinal);
         Assert.Equal(2, Regex.Matches(refresh, @"HidePendingBar_\(\);").Count);
         Assert.Contains("KeyboardBackToPage_();", refresh, StringComparison.Ordinal);

         string hide = WithheldRowTests.MemberText(shell, "private void HidePendingBar_()");
         Assert.True(hide != null, "HidePendingBar_ is gone.");
         int off = hide.IndexOf("KeyboardOffPendingBar_();", StringComparison.Ordinal);
         int collapsed = hide.IndexOf("PendingBar.Visibility = Visibility.Collapsed;", StringComparison.Ordinal);
         int back = hide.IndexOf("KeyboardBackToPage_();", StringComparison.Ordinal);
         Assert.True(off >= 0 && collapsed > off && back > collapsed, "The bar is collapsed with the keyboard still on it.");

         string send = WithheldRowTests.MemberText(shell, "private void KeyboardOffPendingBar_()");
         Assert.True(send != null && send.Contains("FocusManager.GetFocusedElement(this)", StringComparison.Ordinal) && send.Contains("KeyboardToPage_();", StringComparison.Ordinal),
            "The keyboard the window gives back to the bar after a question is not sent to the page.");
         string toPage = WithheldRowTests.MemberText(shell, "private void KeyboardToPage_()");
         Assert.True(toPage != null && toPage.Contains("RememberedPageFocus_()", StringComparison.Ordinal)
               && toPage.Contains("(remembered ?? FirstFocusable_(ContentHost))?.Focus();", StringComparison.Ordinal)
               && toPage.Contains("if (ReferenceEquals(remembered, ContentHost))", StringComparison.Ordinal),
            "The keyboard does not go to the page's control that last had it, or to its first.");
      }

      /// <summary>A file under the repository root, found from the test binaries.</summary>
      private static string ReadRepositoryFile_(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, relative);
            if (File.Exists(candidate) && Directory.Exists(Path.Join(directory.FullName, "build")))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail(relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
