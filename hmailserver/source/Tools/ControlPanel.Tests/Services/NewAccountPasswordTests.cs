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
   /// The account dialog judges a password as it is typed and asks before it saves a
   /// weak one; the Domains page's Create account wrote whatever was typed, so the first
   /// mailbox a new administrator made skipped the check (review finding F072). These
   /// hold the page to the dialog's verdict and its question.
   /// </summary>
   public class NewAccountPasswordTests
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";

      [Theory]
      [InlineData("secret", PasswordStrength.Level.Weak)]
      [InlineData("Summer2026", PasswordStrength.Level.Fair)]
      [InlineData("c0rrect-Horse-battery", PasswordStrength.Level.Strong)]
      [InlineData("", PasswordStrength.Level.Empty)]
      public void TheVerdict_IsTheDialogs(string password, PasswordStrength.Level level)
         => Assert.Equal(level, PasswordStrength.Evaluate(password).Level);

      [Fact]
      public void CreateAccount_AsksBeforeItGivesAWeakPassword_WithCancelForEnter()
      {
         string code = CodeBehind();
         string create = WithheldRowTests.MemberText(code, "private void AddAccount_Click(object sender, RoutedEventArgs e)");
         Assert.True(create != null, "DomainsView.AddAccount_Click is gone.");
         int asks = create.IndexOf("if (!NewAccountPasswordAccepted_(password))", StringComparison.Ordinal);
         int writes = create.IndexOf("accounts.Add()", StringComparison.Ordinal);
         Assert.True(asks > 0 && writes > asks, "Create account no longer asks about a weak password before it creates the account.");

         string accepted = WithheldRowTests.MemberText(code, "private static bool NewAccountPasswordAccepted_(string password)");
         Assert.True(accepted != null, "DomainsView.NewAccountPasswordAccepted_ is gone.");
         Assert.Contains("PasswordStrength.Evaluate(password)", accepted, StringComparison.Ordinal);
         Assert.Contains("level != PasswordStrength.Level.Weak", accepted, StringComparison.Ordinal);
         Assert.Contains("L(\"_Create account\"), cancelByDefault: true, level: StatusLevel.Warning", accepted, StringComparison.Ordinal);
      }

      [Fact]
      public void TheVerdict_IsShownAsItIsTyped_OnALineOfItsOwnUnderThePassword()
      {
         XElement page = XDocument.Parse(ReadControlPanelFile(Path.Join("Views", "DomainsView.xaml"))).Root;
         XElement form = page.Descendants().First(e => (string)e.Attribute(X + "Name") == "AccountForm");
         XElement line = form.Elements().FirstOrDefault(e => (string)e.Attribute(X + "Name") == "NewAccountStrength");
         Assert.True(line != null, "The account form has no strength line.");
         Assert.Equal("2", (string)line.Attribute("Grid.Row"));
         Assert.Equal("2", (string)line.Attribute("Grid.ColumnSpan"));
         Assert.Equal("Polite", (string)line.Attribute("AutomationProperties.LiveSetting"));
         Assert.Equal(4, form.Element(Wpf + "Grid.RowDefinitions").Elements().Count());

         string code = CodeBehind();
         Assert.Contains("NewAccountPassword.PasswordChanged += (s, e) => ShowNewAccountStrength_();", code, StringComparison.Ordinal);
         string show = WithheldRowTests.MemberText(code, "private void ShowNewAccountStrength_()");
         Assert.True(show != null, "DomainsView.ShowNewAccountStrength_ is gone.");
         Assert.Contains("PasswordStrength.Evaluate(NewAccountPassword.Password)", show, StringComparison.Ordinal);
         Assert.Contains("Announce.Request(NewAccountStrength)", show, StringComparison.Ordinal);
         Assert.Contains("SetResourceReference(TextBlock.ForegroundProperty, key)", show, StringComparison.Ordinal);

         // The buttons that wrap under the password go under the strength line, not over it.
         string wrap = WithheldRowTests.MemberText(code, "private void AccountForm_SizeChanged(object sender, SizeChangedEventArgs e)");
         Assert.Contains("Grid.SetRow(AccountButtons, beside ? 1 : 3);", wrap, StringComparison.Ordinal);
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
