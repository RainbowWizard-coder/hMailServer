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
   /// The Domains &amp; accounts page's flows (review findings F092, F095, F096, F097
   /// and F102). The Address box held "user@" and the domain as text, so an untouched
   /// Create account made a mailbox called user; the other boxes' placeholders read
   /// "newdomain.com", "alias@domain", "target@anywhere" and "list@domain". A new
   /// account was not selected, and a mailbox moved to another domain vanished. The
   /// account search could not find an address of another domain. Create account was
   /// the screen's one primary button on an empty server, and the rows' buttons were
   /// announced with no row.
   /// </summary>
   public class HostedAddressTests
   {
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";
      private static readonly XNamespace Ui = "http://schemas.lepo.co/wpfui/2022/xaml";
      private static readonly XNamespace Scaffold = "clr-namespace:hMailServer.ControlPanel.Views.Scaffold";

      [Theory]
      [InlineData("bob", "example.com", "bob@example.com")]
      [InlineData("  bob  ", "example.com", "bob@example.com")]
      [InlineData("bob@example.com", "example.com", "bob@example.com")]
      [InlineData("bob@other.example", "example.com", "bob@other.example")]
      [InlineData("", "example.com", "")]
      [InlineData("   ", "example.com", "")]
      [InlineData(null, "example.com", "")]
      [InlineData("bob", null, "bob")]
      public void ANameTypedWithoutAnAt_IsCompletedWithTheSelectedDomain(string typed, string domain, string expected)
         => Assert.Equal(expected, HostedAddress.Complete(typed, domain));

      [Fact]
      public void ThePlaceholder_IsAnExampleNameInTheDomain_AndNothingWithNoDomain()
      {
         Assert.Equal("name@example.com", HostedAddress.Placeholder("name", "example.com"));
         Assert.Equal("", HostedAddress.Placeholder("name", null));
      }

      [Theory]
      [InlineData("user01@example.net", "example.com", "example.net")]
      [InlineData("user01@EXAMPLE.NET", "example.com", "example.net")]
      [InlineData("user01@example.com", "example.com", null)]
      [InlineData("user01@example.ne", "example.com", null)]
      [InlineData("user01@", "example.com", null)]
      [InlineData("user01", "example.com", null)]
      [InlineData("user01@unknown.example", "example.com", null)]
      [InlineData("", "example.com", null)]
      public void AWholeAddressOfAnotherHostedDomain_NamesThatDomain(string search, string selected, string expected)
         => Assert.Equal(expected, HostedAddress.DomainToShow(search, new[] { "example.com", "example.net", "example.org" }, selected));

      [Theory]
      [InlineData("bob@example.net", "example.net")]
      [InlineData("bob", null)]
      [InlineData("bob@", null)]
      [InlineData(null, null)]
      public void TheDomainOfAnAddress_IsWhatFollowsItsAt(string address, string expected)
         => Assert.Equal(expected, HostedAddress.DomainOf(address));

      [Fact]
      public void NoBoxHoldsTextItWasNotGiven_AndThePlaceholdersAreReservedExamples()
      {
         XElement page = Page();
         foreach (string box in new[] { "NewDomainBox", "NewAliasValue", "NewAccountBox", "NewAliasName", "NewDistAddress" })
            Assert.Null(Named(page, box).Attribute("PlaceholderText")); // set in code, each example marked

         string code = CodeBehind();
         Assert.Contains("NewDomainBox.PlaceholderText = ExamplePlaceholder.Of(\"example.org\");", code, StringComparison.Ordinal);
         Assert.Contains("NewAliasValue.PlaceholderText = ExamplePlaceholder.Of(\"person@example.net\");", code, StringComparison.Ordinal);
         Assert.DoesNotContain("NewAccountBox.Text = \"user@\"", code, StringComparison.Ordinal);
         string xaml = ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml"));
         foreach (string old in new[] { "newdomain.com", "user@domain", "alias@domain", "target@anywhere", "list@domain" })
            Assert.DoesNotContain("\"" + old + "\"", xaml, StringComparison.Ordinal);

         string placeholders = WithheldRowTests.MemberText(code, "private void ShowAddressPlaceholders_(string domainName)");
         Assert.True(placeholders != null, "DomainsView.ShowAddressPlaceholders_ is gone.");
         Assert.Contains("NewAccountBox.PlaceholderText = ExamplePlaceholder.Shown(HostedAddress.Placeholder(\"name\", domainName));", placeholders, StringComparison.Ordinal);
         Assert.Contains("NewAliasName.PlaceholderText = ExamplePlaceholder.Shown(HostedAddress.Placeholder(\"sales\", domainName));", placeholders, StringComparison.Ordinal);
         Assert.Contains("NewDistAddress.PlaceholderText = ExamplePlaceholder.Shown(HostedAddress.Placeholder(\"team\", domainName));", placeholders, StringComparison.Ordinal);

         string reload = WithheldRowTests.MemberText(code, "private void ReloadAccounts()");
         Assert.Equal(2, CountOf(reload, "ShowAddressPlaceholders_("));
      }

      [Theory]
      [InlineData("private void AddAccount_Click(object sender, RoutedEventArgs e)", "HostedAddress.Complete(NewAccountBox.Text, domainName)")]
      [InlineData("private void AddAlias_Click(object sender, RoutedEventArgs e)", "HostedAddress.Complete(NewAliasName.Text, SelectedDomainName())")]
      [InlineData("private void AddDistList_Click(object sender, RoutedEventArgs e)", "HostedAddress.Complete(NewDistAddress.Text, SelectedDomainName())")]
      public void EachAddressForm_CompletesANameWithTheDomain(string member, string completion)
      {
         string body = WithheldRowTests.MemberText(CodeBehind(), member);
         Assert.True(body != null, "DomainsView." + member + " is gone.");
         Assert.Contains(completion, body, StringComparison.Ordinal);
      }

      [Fact]
      public void ANewAccount_IsSelected_AndAMovedMailboxIsFollowedToItsDomain()
      {
         string code = CodeBehind();
         string add = WithheldRowTests.MemberText(code, "private void AddAccount_Click(object sender, RoutedEventArgs e)");
         int reload = add.LastIndexOf("ReloadAccounts();", StringComparison.Ordinal);
         Assert.True(reload > 0 && add.IndexOf("SelectAccount_(address);", reload, StringComparison.Ordinal) > reload,
            "Create account does not select the new account after reading the list again.");

         string edit = WithheldRowTests.MemberText(code, "private void EditAccount_(string address)");
         Assert.Contains("HostedAddress.DomainOf(dialog.MovedTo) is string movedDomain", edit, StringComparison.Ordinal);
         Assert.Contains("ReloadDomains(select: movedDomain);", edit, StringComparison.Ordinal);
         Assert.Contains("SelectAccount_(dialog.MovedTo);", edit, StringComparison.Ordinal);

         string dialog = ReadControlPanelFile(Path.Join("Views", "AccountDialog.cs"));
         Assert.Contains("public string MovedTo { get; private set; }", dialog, StringComparison.Ordinal);
         string move = WithheldRowTests.MemberText(dialog, "private void MoveMailbox()");
         Assert.True(move.IndexOf("MovedTo = dialog.MovedTo;", StringComparison.Ordinal) < move.IndexOf("Close();", StringComparison.Ordinal),
            "The account dialog closes after a move without saying where the mailbox went.");
      }

      [Fact]
      public void TheAccountSearch_SelectsTheDomainAWholeAddressNames()
      {
         string search = WithheldRowTests.MemberText(CodeBehind(), "private void AccountSearch_TextChanged(object sender, EventArgs e)");
         Assert.Contains("HostedAddress.DomainToShow(AccountSearch.SearchText,", search, StringComparison.Ordinal);
         Assert.Contains("SelectDomain_(other)", search, StringComparison.Ordinal);
         Assert.Contains("ListSearch.Apply(AccountList, AccountSearch.SearchText);", search, StringComparison.Ordinal);
      }

      [Fact]
      public void OnAnEmptyServer_AddADomain_IsTheOnePrimaryButton()
      {
         XElement action = Named(Page(), "DomainStatus").Element(Scaffold + "EmptyState.Action").Elements().Single();
         Assert.Equal("Primary", (string)action.Attribute("Appearance"));

         string placeholders = WithheldRowTests.MemberText(CodeBehind(), "private void ShowAddressPlaceholders_(string domainName)");
         Assert.Contains("AddAccountButton.Appearance = domainName == null ? Wpf.Ui.Controls.ControlAppearance.Secondary : Wpf.Ui.Controls.ControlAppearance.Primary;",
            placeholders, StringComparison.Ordinal);
      }

      [Fact]
      public void EveryRowButton_IsNamedForItsRow()
      {
         XElement page = Page();
         var expected = new (string list, string click, string name)[]
         {
            ("DomainList", "EditDomain_Click", "{Binding EditName}"),
            ("DomainList", "DeleteDomain_Click", "{Binding DeleteName}"),
            ("AccountList", "EditAccount_Click", "{Binding EditName}"),
            ("AccountList", "DeleteAccount_Click", "{Binding DeleteName}"),
            ("AliasList", "DeleteAlias_Click", "{Binding DeleteName}"),
            ("DistList", "EditDistList_Click", "{Binding EditName}"),
            ("DistList", "EditRecipients_Click", "{Binding MembersName}"),
            ("DistList", "DeleteDistList_Click", "{Binding DeleteName}"),
         };

         foreach ((string list, string click, string name) in expected)
         {
            XElement button = Named(page, list).Descendants(Ui + "Button").Single(b => (string)b.Attribute("Click") == click);
            Assert.Equal(name, (string)button.Attribute("AutomationProperties.Name"));
            if (button.Attribute("Icon") != null)
               Assert.Equal(name, (string)button.Attribute("ToolTip"));
         }

         string code = CodeBehind();
         foreach (string property in new[]
         {
            "public string EditName => F(\"Edit domain {0}\", Name);",
            "public string DeleteName => F(\"Delete domain {0}\", Name);",
            "public string EditName => F(\"Edit account {0}\", Address);",
            "public string DeleteName => F(\"Delete account {0}\", Address);",
            "public string DeleteName => F(\"Delete alias {0}\", Name);",
            "public string DeleteName => F(\"Delete distribution list {0}\", Address);",
         })
            Assert.Contains(property, code, StringComparison.Ordinal);
      }

      /// <summary>
      /// No row button on the page carries words: the domain and account rows' Edit
      /// and Delete were captions, whose names had to start with them in every
      /// language (WCAG 2.5.3); they are glyphs named for their row, as the alias and
      /// list rows' are (the owner's decision 15; DomainsRowTests holds their names).
      /// </summary>
      [Fact]
      public void NoRowButton_CarriesACaption()
      {
         XElement page = Page();
         foreach (string list in new[] { "DomainList", "AccountList", "AliasList", "DistList" })
         {
            foreach (XElement button in Named(page, list).Descendants(Ui + "Button"))
               Assert.True(button.Attribute("Content") == null, list + " " + (string)button.Attribute("Click") + " carries a caption.");
         }
      }

      private static int CountOf(string text, string part)
      {
         int count = 0;
         for (int at = text.IndexOf(part, StringComparison.Ordinal); at >= 0; at = text.IndexOf(part, at + part.Length, StringComparison.Ordinal))
            count++;
         return count;
      }

      private static XElement Page() => XDocument.Parse(ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml"))).Root;

      private static XElement Named(XElement page, string name)
      {
         XElement named = page.Descendants().FirstOrDefault(e => (string)e.Attribute(X + "Name") == name);
         Assert.True(named != null, "DomainsView.xaml has no element named " + name + ".");
         return named;
      }

      private static string CodeBehind() => ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml.cs"));

      internal static string ReadControlPanelFile(string relative) => File.ReadAllText(FindControlPanelFile(relative));

      internal static string FindControlPanelFile(string relative)
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return candidate;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
