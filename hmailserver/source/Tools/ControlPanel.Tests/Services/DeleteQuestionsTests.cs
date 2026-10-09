// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// What the Control Panel's delete and remove questions say (Services/Confirmations):
   /// each names the thing acted on and what goes with it, and each page asks its own.
   /// </summary>
   public class DeleteQuestionsTests
   {
      // ---- Domains & accounts, public folders (F065) ------------------------------------

      [Fact]
      public void TheAccountsQuestion_SaysTheMailboxAndItsMessagesGo()
      {
         string question = Confirmations.DeleteAccount("user00@example.com");
         Assert.Contains("user00@example.com", question, StringComparison.Ordinal);
         Assert.Contains("mailbox and every message", question, StringComparison.Ordinal);
         Assert.Contains("no undo", question, StringComparison.Ordinal);
      }

      [Fact]
      public void TheAliasAndListQuestions_SayMailToThemStops_AndTheMembersAreKept()
      {
         Assert.Contains("no longer delivered", Confirmations.DeleteAlias("sales@example.com"), StringComparison.Ordinal);
         string list = Confirmations.DeleteDistributionList("everyone@example.com");
         Assert.Contains("everyone@example.com", list, StringComparison.Ordinal);
         Assert.Contains("members' mailboxes are not affected", list, StringComparison.Ordinal);
      }

      [Fact]
      public void TheDomainsQuestion_IsOneWholeSentencePerCount()
      {
         string none = Confirmations.DeleteDomain("example.com", 0);
         string one = Confirmations.DeleteDomain("example.com", 1);
         string many = Confirmations.DeleteDomain("example.com", 12);

         Assert.Equal(3, new[] { none, one, many }.Distinct().Count());
         Assert.All(new[] { none, one, many }, text => Assert.Contains("example.com", text, StringComparison.Ordinal));
         Assert.Contains("no accounts", none, StringComparison.Ordinal);
         Assert.Contains("one account", one, StringComparison.Ordinal);
         Assert.Contains("12 accounts", many, StringComparison.Ordinal);
      }

      [Fact]
      public void ThePublicFoldersQuestion_NamesTheFolderUnquoted()
      {
         string question = Confirmations.DeletePublicFolder("Sales");
         Assert.Contains("public folder Sales and every message", question, StringComparison.Ordinal);
         Assert.DoesNotContain("'", question, StringComparison.Ordinal);
      }

      // ---- SSL certificates, IP ranges, directory synchronisation (F128) ---------------

      [Fact]
      public void TheCertificatesQuestion_NamesThePortsThatPresentIt()
      {
         string unread = Confirmations.DeleteCertificate("legacy.example.com", null);
         string none = Confirmations.DeleteCertificate("legacy.example.com", Array.Empty<string>());
         string one = Confirmations.DeleteCertificate("legacy.example.com", new[] { "SMTP 465" });
         string two = Confirmations.DeleteCertificate("legacy.example.com", new[] { "SMTP 465", "IMAP 993" });

         Assert.Equal(4, new[] { unread, none, one, two }.Distinct().Count());
         Assert.All(new[] { unread, none, one, two }, text => Assert.Contains("legacy.example.com", text, StringComparison.Ordinal));
         Assert.Contains("No TCP/IP port presents it", none, StringComparison.Ordinal);
         Assert.Contains("SMTP 465", one, StringComparison.Ordinal);
         Assert.Contains("does not start again", one, StringComparison.Ordinal);
         Assert.Contains("SMTP 465, IMAP 993", two, StringComparison.Ordinal);
      }

      [Fact]
      public void TheCertificatesPage_CountsTheTlsPortsThatNameTheCertificate()
      {
         string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "SslCertificatesView.xaml.cs"));
         string delete = WithheldRowTests.MemberText(page, "private void Delete_Click(object sender, RoutedEventArgs e)");
         Assert.True(delete != null, "SslCertificatesView.Delete_Click is gone.");
         Assert.Contains("Confirmations.DeleteCertificate(row.Name, PortsPresenting_(row.Id))", delete, StringComparison.Ordinal);
         Assert.Contains("L(\"_Delete certificate\")", delete, StringComparison.Ordinal);

         string ports = WithheldRowTests.MemberText(page, "private static List<string> PortsPresenting_(int certificateId)");
         Assert.True(ports != null, "SslCertificatesView.PortsPresenting_ is gone.");
         Assert.Contains("(int)port.ConnectionSecurity > 0", ports, StringComparison.Ordinal);
         Assert.Contains("(int)port.SSLCertificateID == certificateId", ports, StringComparison.Ordinal);
         Assert.Contains("return null;", ports, StringComparison.Ordinal);
      }

      [Fact]
      public void TheRangesQuestion_NamesItsAddresses_AndWhereTheyGo()
      {
         string question = Confirmations.DeleteIpRange("Branch office", "10.1.0.0", "10.1.0.255");
         Assert.Contains("Branch office (10.1.0.0 - 10.1.0.255)", question, StringComparison.Ordinal);
         Assert.Contains("next range that covers them", question, StringComparison.Ordinal);
      }

      [Fact]
      public void TheDirectorySyncQuestion_RepeatsThePreviewsCounts()
      {
         string all = Confirmations.DirectorySyncApply("", 4, 2, 0, 7, false);
         Assert.StartsWith("Apply the preview to every domain linked to a directory?", all, StringComparison.Ordinal);
         Assert.Contains("Mailboxes created: 4. Mailboxes updated: 2. Directory entries skipped: 7.", all, StringComparison.Ordinal);
         Assert.DoesNotContain("inactive", all, StringComparison.Ordinal);
         Assert.EndsWith("No message is deleted and no account is removed.", all, StringComparison.Ordinal);

         string one = Confirmations.DirectorySyncApply("example.com", 4, 2, 3, 7, true);
         Assert.StartsWith("Apply the preview to the domain example.com?", one, StringComparison.Ordinal);
         Assert.Contains("Accounts marked inactive: 3.", one, StringComparison.Ordinal);

         string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "DirectorySyncView.cs"));
         string apply = WithheldRowTests.MemberText(page, "private async Task ApplyAsync_()");
         Assert.True(apply != null, "DirectorySyncView.ApplyAsync_ is gone.");
         Assert.Contains("Confirmations.DirectorySyncApply(", apply, StringComparison.Ordinal);
         Assert.Contains("previewReport_", apply, StringComparison.Ordinal);
         Assert.Contains("L(\"_Apply\")", apply, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("DomainsView.xaml.cs", "private void DeleteAccount_(string address)", "Confirmations.DeleteAccount(address)", "L(\"_Delete account\")")]
      [InlineData("DomainsView.xaml.cs", "private void DeleteAlias_(string name)", "Confirmations.DeleteAlias(name)", "L(\"_Delete alias\")")]
      [InlineData("DomainsView.xaml.cs", "private void DeleteDistList_(string address)", "Confirmations.DeleteDistributionList(address)", "L(\"_Delete list\")")]
      [InlineData("DomainsView.xaml.cs", "private void DeleteDomain_(string name)", "Confirmations.DeleteDomain(name, accountCount)", "L(\"_Delete domain\")")]
      [InlineData("PublicFoldersView.cs", "private void DeleteFolder()", "Confirmations.DeletePublicFolder(name)", "L(\"_Delete folder\")")]
      [InlineData("IPRangesView.xaml.cs", "private void Delete_Click(object sender, RoutedEventArgs e)", "Confirmations.DeleteIpRange(row.Name, row.LowerIP, row.UpperIP)", "L(\"_Delete range\")")]
      public void EachDelete_AsksItsOwnQuestion_WithTheActOnItsButton(string file, string member, string question, string action)
      {
         string text = WithheldRowTests.MemberText(ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", file)), member);
         Assert.True(text != null, file + ": " + member + " is gone.");
         Assert.Contains(question, text, StringComparison.Ordinal);
         Assert.Contains(action, text, StringComparison.Ordinal);
         Assert.Contains("destructive: true", text, StringComparison.Ordinal);
      }
   }
}
