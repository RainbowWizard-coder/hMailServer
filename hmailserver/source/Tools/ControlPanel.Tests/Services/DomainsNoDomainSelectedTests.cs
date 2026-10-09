// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// With no domain selected - an empty server, a domain just deleted - the accounts
   /// card kept the last domain's user@ address in its box, said nothing under
   /// "Select a domain", and the account, alias and list forms stayed live, Create
   /// account answering "Address and password are required." where the missing thing
   /// was a domain (review finding F063). These hold the no-domain state the page now
   /// puts the three cards in, and that it does so when the selection did not change.
   /// </summary>
   public class DomainsNoDomainSelectedTests
   {
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";

      [Fact]
      public void WithNoDomain_TheAccountFormIsEmptyAndWaits_AndTheCardSaysWhatToDo()
      {
         string reload = WithheldRowTests.MemberText(CodeBehind(), "private void ReloadAccounts()");
         Assert.True(reload != null, "DomainsView.ReloadAccounts is gone.");
         string none = reload[..reload.IndexOf("AccountsCard.Title = F(", StringComparison.Ordinal)];

         foreach (string step in new[]
         {
            "NewAccountBox.Text = \"\";",
            "NewAccountPassword.Password = \"\";",
            "StatusText.Show(AccountStatus, null, 0, null, L(\"Select a domain to see its accounts.\"));",
            "AccountForm.IsEnabled = false;"
         })
            Assert.True(none.Contains(step, StringComparison.Ordinal), "With no domain selected, ReloadAccounts no longer does " + step);

         Assert.DoesNotContain("AccountStatus.Visibility = Visibility.Collapsed;", none, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("private void ReloadAliases()", "AddAliasForm.IsEnabled = DomainList.SelectedItem != null;")]
      [InlineData("private void ReloadDistLists()", "AddListForm.IsEnabled = DomainList.SelectedItem != null;")]
      public void TheAliasAndListForms_WaitForADomain(string member, string step)
      {
         string body = WithheldRowTests.MemberText(CodeBehind(), member);
         Assert.True(body != null, "DomainsView." + member + " is gone.");
         Assert.Contains(step, body, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("AddAliasForm", "AddAliasButton")]
      [InlineData("AddListForm", "AddListButton")]
      public void EachFormIsNamed_AndHoldsItsButton(string form, string button)
      {
         XElement page = XDocument.Parse(ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml"))).Root;
         XElement named = page.Descendants().FirstOrDefault(e => (string)e.Attribute(X + "Name") == form);
         Assert.True(named != null, "DomainsView.xaml has no element named " + form + ".");
         Assert.Contains(named.Descendants(), e => (string)e.Attribute(X + "Name") == button);
      }

      [Fact]
      public void AReadWithNoDomainToSelect_PutsTheCardsInTheirNoDomainState_AndAFailedOneSaysNothingOfSelecting()
      {
         string reload = WithheldRowTests.MemberText(CodeBehind(), "private void ReloadDomains(string select = null)");
         Assert.True(reload != null, "DomainsView.ReloadDomains is gone.");

         // An empty server: nothing to select, so no selection change runs the three.
         int none = reload.IndexOf("if (DomainList.SelectedIndex < 0)", StringComparison.Ordinal);
         Assert.True(none > 0, "ReloadDomains no longer puts the cards in their no-domain state when nothing is selected.");
         string after = reload[none..];
         Assert.True(after.Contains("ReloadAccounts();", StringComparison.Ordinal) && after.Contains("ReloadAliases();", StringComparison.Ordinal)
            && after.Contains("ReloadDistLists();", StringComparison.Ordinal));

         // A read that failed is not a domain to select: the accounts card says so no more than the other two do.
         string failed = reload[..reload.IndexOf("ReadFailure.Clear(DomainNotice", StringComparison.Ordinal)];
         Assert.Contains("AccountStatus.Visibility = Visibility.Collapsed;", failed, StringComparison.Ordinal);
      }

      private static string CodeBehind() => ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml.cs"));

      private static string ReadControlPanelFile(string relative)
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
