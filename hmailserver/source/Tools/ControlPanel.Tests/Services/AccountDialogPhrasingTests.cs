// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F292: the account and domain dialogs phrased the same things
   /// differently. The account dialog said "Last logon", as the raw server string with
   /// no time zone, and "can never log in" where the shell says Sign-in & access; its
   /// zero-access row was two sentences where the value would be; Alt keys sat on its
   /// two read-only rows; the domain's signature choices dropped their articles ("Use
   /// only if account has no signature"); ranges were "(days, 0 = for ever)" against
   /// "(-1 = keep forever)" and "(MB, 0 = unlimited)", a kept password "leave blank to
   /// keep the stored one" against "leave empty to keep current"; and DKIM's two rows
   /// were the dialog's only US spelling, canonicalization.
   /// </summary>
   public class AccountDialogPhrasingTests
   {
      private static readonly string[] Dialogs = { "AccountDialog.cs", "DomainDialog.cs", "DistributionListDialog.cs" };

      [Fact]
      public void TheLastSignIn_IsTheReadersShortDateAndTime_MarkedAsTheServersTime()
      {
         var signedIn = new DateTime(2026, 10, 8, 14, 30, 0);
         string expected = signedIn.ToString("g", CultureInfo.CurrentCulture) + " (server time)";

         Assert.Equal(expected, LastSignInText.Format(signedIn));
         // As the server writes it, should a date come as text.
         Assert.Equal(expected, LastSignInText.Format("2026-10-08 14:30:00"));
      }

      [Fact]
      public void ADateTheServerKeepsForNoSignIn_IsNever()
      {
         // An OLE date of 0, which is what the server gives for an empty or unreadable date.
         Assert.Equal("Never", LastSignInText.Format(new DateTime(1899, 12, 30)));
         Assert.Equal("Never", LastSignInText.Format(null));
         Assert.Equal("Never", LastSignInText.Format(""));
         Assert.Equal("Never", LastSignInText.Format(DBNull.Value));
         // Text in a form of its own is the server's words, shown as they are.
         Assert.Equal("yesterday", LastSignInText.Format(" yesterday "));
      }

      [Fact]
      public void TheAccountDialog_ShowsItsTwoReadOnlyRowsWithoutAnAltKey_AndZeroAccessAsOffOrOn()
      {
         string code = ReadDialog("AccountDialog.cs");

         Assert.Contains("DialogFields.ReadOnlyField(L(\"Last sign-in\"), lastLogon_)", code, StringComparison.Ordinal);
         Assert.Contains("DialogFields.ReadOnlyField(L(\"Zero-access mailbox\"), zeroAccess_)", code, StringComparison.Ordinal);
         Assert.Contains("lastLogon_.Text = LastSignInText.Format(lastSignIn);", code, StringComparison.Ordinal);
         Assert.Contains("zeroAccess_.Text = zeroAccess ? L(\"On\") : L(\"Off\");", code, StringComparison.Ordinal);
         Assert.Contains("zeroAccessRow_.Hint = zeroAccess", code, StringComparison.Ordinal);
         Assert.DoesNotContain("Last l_ogon", code, StringComparison.Ordinal);
         Assert.DoesNotContain("_Zero-access mailbox", code, StringComparison.Ordinal);
         Assert.DoesNotContain("Convert.ToString(a.LastLogonTime)", code, StringComparison.Ordinal);
      }

      [Fact]
      public void TheDomainsSignatureChoices_SayTheAccountsSignature()
      {
         string code = ReadDialog("DomainDialog.cs");
         Assert.Contains("L(\"Use only when the account has no signature\"), 1", code, StringComparison.Ordinal);
         Assert.Contains("L(\"Replace the account's signature\"), 2", code, StringComparison.Ordinal);
         Assert.Contains("L(\"Append to the account's signature\"), 3", code, StringComparison.Ordinal);
      }

      [Theory]
      // what 0 means is "(unit; 0 = ...)", never after a comma
      [InlineData(@"\((?:MB|KB|days|_days|da_ys), 0 = ")]
      [InlineData(@"0 = unlimited")]
      [InlineData(@"keep forever")]
      [InlineData(@"leave blank to keep")]
      [InlineData(@"canonicalization")]
      [InlineData(@"\blog in\b")]
      [InlineData(@"\blogged in\b")]
      [InlineData(@"\bLast l_?ogon\b")]
      public void NoDialog_PhrasesItTheOtherWay(string pattern)
      {
         var form = new Regex(pattern);
         var found = Dialogs
            .SelectMany(dialog => Regex.Matches(ReadDialog(dialog), @"\bL\(""((?:[^""\\]|\\.)*)""\)").Select(m => dialog + ": " + m.Groups[1].Value))
            .Where(text => form.IsMatch(text))
            .ToList();

         Assert.True(found.Count == 0, "Phrased the other way:\n" + string.Join("\n", found));
      }

      private static string ReadDialog(string name) => ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", name));
   }
}
