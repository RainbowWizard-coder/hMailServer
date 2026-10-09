// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F240: the Administrative access page's Two-factor tab never
   /// said whether either factor was on - both cards offered "Set up or turn off …",
   /// and the state appeared only after the dialog closed, as green or red text, so
   /// a factor legitimately off read as a failure. Turning the server's factor off
   /// asked for "the current 6-digit code" and accepted any six characters, since
   /// nothing can check one; the recovery text left out the [Security] section and
   /// the service restart the server's own text names; and after a failed enrolment
   /// the code box kept the keyboard beside a Confirm that could not be pressed.
   ///
   /// Scanned rather than rendered, as QuarantinePageTests is: this assembly has no
   /// WPF. Each test fails against the tree before the fix.
   /// </summary>
   public class TwoFactorStateTests
   {
      [Fact]
      public void EachCard_SaysWhetherItsFactorIsOn_WithTheButtonWordedByTheState()
      {
         string view = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         Assert.DoesNotContain("Set up or turn off", view, StringComparison.Ordinal);

         int server = view.IndexOf("serverTwoFactor.Settings.Add(new TwoFactorStateRow", StringComparison.Ordinal);
         int local = view.IndexOf("twoFactor.Settings.Add(new TwoFactorStateRow", server + 1, StringComparison.Ordinal);
         Assert.True(server > 0, "The server-enforced factor's card does not say whether the factor is on.");
         Assert.True(local > server, "The Control-Panel-only factor's card does not say whether the factor is on.");

         string serverRow = view.Substring(server, view.IndexOf("});", server, StringComparison.Ordinal) - server);
         Assert.Contains("ReadOn = () => (bool)ServerSession.Current.Application.AdministratorTOTPEnabled", serverRow, StringComparison.Ordinal);
         Assert.Contains("OffLevel = StatusLevel.Warning", serverRow, StringComparison.Ordinal);
         Assert.Contains("SetUpCaption = L(\"Set up the server-enforced second factor…\")", serverRow, StringComparison.Ordinal);
         Assert.Contains("TurnOffCaption = L(\"Turn off the server-enforced second factor…\")", serverRow, StringComparison.Ordinal);

         string localRow = view.Substring(local, view.IndexOf("});", local, StringComparison.Ordinal) - local);
         Assert.Contains("ReadOn = TotpManager.IsConfigured", localRow, StringComparison.Ordinal);
         Assert.Contains("OffLevel = StatusLevel.Normal", localRow, StringComparison.Ordinal);

         // The recovery text names the section and the restart, as the server's does.
         Assert.Contains("deletes AdministratorTotpSecret from the [Security] section of hMailServer.ini", view, StringComparison.Ordinal);
         Assert.Contains("restarts the hMailServer service", view, StringComparison.Ordinal);
      }

      [Fact]
      public void TheStateRow_DrawsAPillPerState_AndReadsTheStateAgainAfterTheDialog()
      {
         string row = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.TwoFactorState.cs"));

         Assert.Contains("on == true ? StatusLevel.Good : on == false ? OffLevel : StatusLevel.Normal", row, StringComparison.Ordinal);
         Assert.Contains("on == true ? L(\"On\") : on == false ? L(\"Off\") : L(\"Not read\")", row, StringComparison.Ordinal);
         Assert.Contains("button_.Content = on == true ? TurnOffCaption : SetUpCaption", row, StringComparison.Ordinal);
         Assert.Contains("button_.IsEnabled = on != null;", row, StringComparison.Ordinal);

         // Read after the dialog closes, not only when the tab is built.
         string click = row.Substring(row.IndexOf("button_.Click +=", StringComparison.Ordinal));
         Assert.True(click.IndexOf("Open?.Invoke();", StringComparison.Ordinal) < click.IndexOf("Show_();", StringComparison.Ordinal),
            "The row does not read the state again after the dialog closes.");
      }

      [Fact]
      public void TurningTheServerFactorOff_AsksForNoCodeNothingChecks_AndEnterIsClose()
      {
         string dialog = ReadControlPanelFile(Path.Join("Views", "AdministratorTwoFactorDialog.cs"));
         Assert.DoesNotContain("Enter the current 6-digit code to turn the second factor off.", dialog, StringComparison.Ordinal);
         Assert.DoesNotContain("Enter a current code to turn it off.", dialog, StringComparison.Ordinal);

         string refresh = WithheldRowTests.MemberText(dialog, "private void RefreshState()");
         Assert.True(refresh != null, "AdministratorTwoFactorDialog.RefreshState is gone.");
         int enrolled = refresh.IndexOf("if (IsEnrolled() && pendingSecret_ == null)", StringComparison.Ordinal);
         int otherwise = refresh.IndexOf("else", enrolled, StringComparison.Ordinal);
         string on = refresh.Substring(enrolled, otherwise - enrolled);
         Assert.Contains("Turning it off removes that requirement", on, StringComparison.Ordinal);
         Assert.Contains("codeRow_.Visibility = Visibility.Collapsed;", on, StringComparison.Ordinal);
         Assert.Contains("action_.IsDefault = false;", on, StringComparison.Ordinal);
         Assert.Contains("close_.IsDefault = true;", on, StringComparison.Ordinal);

         // The enrolment's recovery text names the section and the restart.
         string off = refresh.Substring(otherwise);
         Assert.Contains("from the [Security] section of hMailServer.ini", off, StringComparison.Ordinal);
         Assert.Contains("restart the hMailServer service", off, StringComparison.Ordinal);
      }

      [Fact]
      public void AFailedEnrolment_LeavesNoCodeBox_AndTheBoxTakesDigitsOnly()
      {
         string dialog = ReadControlPanelFile(Path.Join("Views", "AdministratorTwoFactorDialog.cs"));
         string refresh = WithheldRowTests.MemberText(dialog, "private void RefreshState()");
         Assert.True(refresh != null, "AdministratorTwoFactorDialog.RefreshState is gone.");

         int enrol = refresh.IndexOf("EnsureEnrolled();", StringComparison.Ordinal);
         int hide = refresh.IndexOf("codeRow_.Visibility = pendingSecret_ != null ? Visibility.Visible : Visibility.Collapsed;", StringComparison.Ordinal);
         Assert.True(enrol > 0 && hide > enrol, "A failed enrolment still shows the code box.");
         Assert.Contains("FocusForState_();", refresh, StringComparison.Ordinal);

         Assert.Contains("code_.PreviewTextInput += (s, e) => e.Handled = !IsDigits_(e.Text);", dialog, StringComparison.Ordinal);
         Assert.Contains("DataObject.AddPastingHandler(code_,", dialog, StringComparison.Ordinal);
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
