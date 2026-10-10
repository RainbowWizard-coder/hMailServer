// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The sign-in card offers no second factor of its own, and the two code prompts
   /// say whose code each asks for (review finding F241). "Set up two-factor
   /// authentication…" on the sign-in card set up the Control-Panel-only factor,
   /// which Administrative access calls the older, weaker option; the server's code
   /// prompt and this computer's were the same dialog, titled "Two-factor
   /// authentication", so that with both on the administrator was asked twice by
   /// identical dialogs for codes from two entries of the authenticator; and the
   /// setup dialog was titled "Two-factor authentication" too and paired "Enable
   /// two-factor authentication" with Close. Scanned rather than rendered: this
   /// assembly has no WPF. Each test fails against the tree before the change.
   /// </summary>
   public class TwoFactorPromptsTests
   {
      [Fact]
      public void TheSignInCard_HasNoTwoFactorLink_AndAdministrativeAccessSetsBothUp()
      {
         string card = Read("Views", "ConnectView.xaml");
         Assert.DoesNotContain("HyperlinkButton", card, StringComparison.Ordinal);
         Assert.DoesNotContain("Totp_Click", card, StringComparison.Ordinal);
         Assert.DoesNotContain("TotpSetupDialog", Read("Views", "ConnectView.xaml.cs"), StringComparison.Ordinal);

         // Where the choice is: the Two-factor tab offers this computer's factor beside the server's.
         string access = Read("Views", "ServerSettingsView.xaml.cs");
         Assert.Contains("Open = () => new TotpSetupDialog(Application.Current?.MainWindow).ShowDialog()", access, StringComparison.Ordinal);
         Assert.Contains("Open = () => new AdministratorTwoFactorDialog(Application.Current?.MainWindow).ShowDialog()", access, StringComparison.Ordinal);
      }

      [Fact]
      public void ThePrompts_SayWhoseCodeTheyAskFor()
      {
         string prompt = Read("Views", "TotpPromptDialog.cs");
         Assert.Contains("L(\"This computer's Control Panel code\")", prompt, StringComparison.Ordinal);
         Assert.Contains("L(\"The server's administrator code\")", prompt, StringComparison.Ordinal);
         Assert.Contains("public TotpPromptDialog(Window owner) : this(owner, Factor.Server)", prompt, StringComparison.Ordinal);
         Assert.Contains("UseFrame(heading,", prompt, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Two-factor authentication\")", prompt, StringComparison.Ordinal);

         // The Control Panel's own check asks for this computer's code; the server's
         // sign-in asks for the server's, the one-argument prompt.
         string window = Read("MainWindow.xaml.cs");
         string verify = WithheldRowTests.MemberText(window, "private bool VerifyTwoFactor()");
         Assert.Contains("new Views.TotpPromptDialog(this, Views.TotpPromptDialog.Factor.ThisComputer)", verify, StringComparison.Ordinal);
         string connect = Read("Views", "ConnectView.xaml.cs");
         Assert.Contains("new TotpPromptDialog(Window.GetWindow(this));", connect, StringComparison.Ordinal);
      }

      [Fact]
      public void TheSetupDialog_IsTitledByItsAct_WithCancelUntilItIsDone()
      {
         string setup = Read("Views", "TotpSetupDialog.cs");
         Assert.DoesNotContain("L(\"Two-factor authentication setup\")", setup, StringComparison.Ordinal);
         Assert.DoesNotContain("UseFrame(L(\"Two-factor authentication\")", setup, StringComparison.Ordinal);
         Assert.Contains("L(\"Set up Control-Panel-only two-factor authentication\")", setup, StringComparison.Ordinal);
         Assert.Contains("L(\"Turn off Control-Panel-only two-factor authentication\")", setup, StringComparison.Ordinal);
         Assert.Contains("frame_.Heading = heading;", setup, StringComparison.Ordinal);

         Assert.Contains("private readonly Wpf.Ui.Controls.Button close_ = new() { Content = L(\"Cancel\"), MinWidth = 88 };", setup, StringComparison.Ordinal);
         string apply = WithheldRowTests.MemberText(setup, "private void Apply()");
         Assert.Equal(2, CountOf(apply, "close_.Content = L(\"Close\");"));
      }

      private static int CountOf(string text, string part)
      {
         int count = 0;
         for (int at = text.IndexOf(part, StringComparison.Ordinal); at >= 0; at = text.IndexOf(part, at + part.Length, StringComparison.Ordinal))
            count++;
         return count;
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
