// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The TCP/IP port dialog shows only the rows that apply and says which port it
   /// edits (review finding F223): it was 880 pixels tall at 520, taller than the
   /// smallest window, with SMTP's five-line hint on a POP3 or IMAP port where its
   /// box was disabled, the CA bundle and the policy's hint under a policy of Off,
   /// and a title, "TCP/IP port", and a heading, "Port binding", that named no port.
   /// The dialog is WPF, which this assembly does not run, so its lines are read.
   /// </summary>
   public class TcpIpPortDialogTests
   {
      [Fact]
      public void SmtpAuthentication_IsShownForAnSmtpPortAlone()
      {
         string update = Member("private void UpdateSmtpAuthentication()");
         Assert.Contains("smtpAuthRow_.Visibility = smtp ? Visibility.Visible : Visibility.Collapsed;", update, StringComparison.Ordinal);
         Assert.Contains("DialogFields.SelectCombo(smtpAuth_, 0, orFirst: true);", update, StringComparison.Ordinal);
      }

      /// <summary>The hint is one sentence; the rest is docs/SmtpAuthOnDeliveryPorts.md's.</summary>
      [Fact]
      public void TheAuthHint_IsOneSentence()
      {
         string dialog = Dialog();
         Assert.Contains("L(\"Off and Honeypot refuse AUTH and ban the address that sends it, Honeypot going on offering it as bait; mail clients sign in on 587, not on port 25.\")", dialog, StringComparison.Ordinal);
         Assert.DoesNotContain("Port 25 is where other servers deliver mail", dialog, StringComparison.Ordinal);
         Assert.Contains("587", File.ReadAllText(Doc("SmtpAuthOnDeliveryPorts.md")), StringComparison.Ordinal);
      }

      /// <summary>The CA bundle and what Request and Require do are shown while the policy is one of them.</summary>
      [Fact]
      public void TheClientCertificateRows_FollowThePolicy()
      {
         string rows = Member("private void UpdateClientCertificateRows()");
         Assert.Contains("bool asked = DialogFields.ComboValue(clientCertPolicy_) != 0;", rows, StringComparison.Ordinal);
         Assert.Contains("clientCertPolicyRow_.Hint = null;", rows, StringComparison.Ordinal);
         Assert.Contains("caFieldRow_.Visibility = asked ? Visibility.Visible : Visibility.Collapsed;", rows, StringComparison.Ordinal);
         Assert.Contains("clientCertPolicy_.SelectionChanged += (s, e) => { UpdateClientCertificateRows(); UpdateClientCertificateValidation(); };", Dialog(), StringComparison.Ordinal);

         string load = Member("private void Load()");
         Assert.Contains("UpdateClientCertificateRows();", load, StringComparison.Ordinal);
      }

      /// <summary>The window's title and the frame's heading name the port as it was read: "SMTP port 587".</summary>
      [Fact]
      public void TheTitleAndTheHeading_NameThePort()
      {
         string name = Member("internal static string PortName(int protocol, int number)");
         Assert.Contains("return F(\"{0} port {1}\", name, number.ToString(System.Globalization.CultureInfo.InvariantCulture));", name, StringComparison.Ordinal);

         string load = Member("private void Load()");
         Assert.Contains("Title = PortName(DialogFields.ComboValue(protocol_), (int)p.PortNumber);", load, StringComparison.Ordinal);
         Assert.Contains("frame_.Heading = Title;", load, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Port binding\")", Dialog(), StringComparison.Ordinal);
      }

      private static string Dialog() => ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "TcpIpPortDialog.cs"));

      private static string Member(string signature)
      {
         string member = WithheldRowTests.MemberText(Dialog(), signature);
         Assert.True(member != null, "TcpIpPortDialog: " + signature + " is gone.");
         return member;
      }

      private static string Doc(string name)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "docs", name);
            if (File.Exists(candidate))
               return candidate;
            candidate = Path.Join(directory.FullName, "hmailserver", "docs", name);
            if (File.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("docs/" + name + " could not be found from the test binaries.");
         return null;
      }
   }
}
