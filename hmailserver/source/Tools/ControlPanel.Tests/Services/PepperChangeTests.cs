// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The password pepper was written by Save like any other secret, its shouting
   /// caption the only safeguard (review finding F169), though a new pepper stops
   /// every password stored as an Argon2id or scrypt hash from working. These hold
   /// when a save asks first, that the question carries the number of accounts, that
   /// the field has a plain caption and a warning under it, and that the page asks
   /// before it writes anything.
   /// </summary>
   public class PepperChangeTests
   {
      [Theory]
      [InlineData("new pepper", "", 12, true)]           // set for the first time on a server with accounts
      [InlineData("new pepper", "old pepper", 1, true)]  // changed
      [InlineData("new pepper", "old pepper", null, true)] // accounts not counted: asked as if there were some
      [InlineData("new pepper", "old pepper", 0, false)] // no accounts yet: nothing to break
      [InlineData("", "old pepper", 12, false)]          // the box left empty keeps the stored pepper
      [InlineData(null, "old pepper", 12, false)]
      [InlineData("old pepper", "old pepper", 12, false)] // the same pepper typed again changes nothing
      [InlineData("Old pepper", "old pepper", 12, true)] // a pepper is compared exactly
      public void ASave_AsksFirst_OnlyWhenANewPepperMeetsAccounts(string typed, string stored, int? accounts, bool asks)
         => Assert.Equal(asks, PepperChange.AsksFirst(typed, stored, accounts));

      [Fact]
      public void TheQuestion_GivesTheNumberOfAccounts_WhenItIsKnown()
      {
         Assert.Contains("1234", PepperChange.Question(1234), StringComparison.Ordinal);
         Assert.False(string.IsNullOrWhiteSpace(PepperChange.Question(null)));
         Assert.DoesNotContain("{0}", PepperChange.Question(null), StringComparison.Ordinal);
      }

      [Fact]
      public void TheField_HasAPlainCaption_AndAWarningUnderIt_AndASaveAsksBeforeItWrites()
      {
         string page = ReadFeatureSettingsView();

         Assert.DoesNotContain("invalidates ALL existing passwords", page, StringComparison.Ordinal);
         Assert.Contains("Key = \"PasswordPepper\", Label = L(\"Password pepper\")", page, StringComparison.Ordinal);
         Assert.Contains("Warning = PepperChange.Warning", page, StringComparison.Ordinal);

         string save = WithheldRowTests.MemberText(page, "private bool Save_()");
         Assert.True(save != null, "FeatureSettingsView.Save_ is gone.");
         int asks = save.IndexOf("ConfirmPepperChange_()", StringComparison.Ordinal);
         int writes = save.IndexOf("setting.Save(store_)", StringComparison.Ordinal);
         Assert.True(asks >= 0 && writes > asks, "Save writes the page before it asks about a new password pepper.");

         string confirm = WithheldRowTests.MemberText(page, "private bool ConfirmPepperChange_()");
         Assert.True(confirm != null && confirm.Contains("cancelByDefault: true", StringComparison.Ordinal),
            "The pepper's question makes Enter the answer that changes it.");
      }

      private static string ReadFeatureSettingsView()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "FeatureSettingsView.xaml.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/FeatureSettingsView.xaml.cs could not be found from the test binaries.");
         return null;
      }
   }
}
