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

      [Theory]
      [InlineData("DomainsView.xaml.cs", "private void DeleteAccount_(string address)", "Confirmations.DeleteAccount(address)", "L(\"_Delete account\")")]
      [InlineData("DomainsView.xaml.cs", "private void DeleteAlias_(string name)", "Confirmations.DeleteAlias(name)", "L(\"_Delete alias\")")]
      [InlineData("DomainsView.xaml.cs", "private void DeleteDistList_(string address)", "Confirmations.DeleteDistributionList(address)", "L(\"_Delete list\")")]
      [InlineData("DomainsView.xaml.cs", "private void DeleteDomain_(string name)", "Confirmations.DeleteDomain(name, accountCount)", "L(\"_Delete domain\")")]
      [InlineData("PublicFoldersView.cs", "private void DeleteFolder()", "Confirmations.DeletePublicFolder(name)", "L(\"_Delete folder\")")]
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
