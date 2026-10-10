// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Authentication page and the palette's way to it (review finding F238).
   /// The page opened on fifteen OAuth2 fields, every one live while OAuth2 was off,
   /// with password storage and SMTP AUTH below the fold; and Ctrl+K sent
   /// "Two-factor" to it, which has no second factor, and "SASL", "CRAM-MD5" and
   /// "APOP" to it, whose switches - for the mechanisms this server offers - are on
   /// Protocols, while "2FA", "TOTP" and the mailbox password policy on
   /// Administrative access found nothing.
   /// </summary>
   public class AuthenticationPagesTests
   {
      /// <summary>
      /// The cards in the order a server needs them, OAuth2 last (the owner's
      /// decision of 9 October 2026).
      /// </summary>
      [Fact]
      public void TheAuthenticationPage_PutsOAuth2Last()
      {
         string section = AuthenticationSection_();

         int storage = section.IndexOf("Title = L(\"Password storage\")", StringComparison.Ordinal);
         int smtp = section.IndexOf("Title = L(\"SMTP authentication\")", StringComparison.Ordinal);
         int signIn = section.IndexOf("Title = L(\"Sign-in to the webmail and the Control Deck\")", StringComparison.Ordinal);
         int oauthAdded = section.IndexOf("cards_.Add(oauth2);", StringComparison.Ordinal);

         Assert.True(storage >= 0 && smtp > storage && signIn > smtp, "Password storage, SMTP authentication and the sign-in card are not in that order.");
         Assert.True(oauthAdded > signIn, "The OAuth2 card is not added after the others.");
         Assert.Equal(1, Count_(section, "cards_.Add(oauth2);"));
         Assert.Equal(1, Count_(section, "Title = L(\"OAuth2 / external identity provider\")"));

         // Nothing is added after it: it is the page's last card.
         string afterOauth = section.Substring(oauthAdded + "cards_.Add(oauth2);".Length);
         Assert.DoesNotContain("cards_.Add(", afterOauth, StringComparison.Ordinal);
      }

      /// <summary>
      /// The OAuth2 card's fields follow its switch: greyed while tokens are not
      /// accepted, as the switch's note under it says, and back once it is ticked.
      /// </summary>
      [Fact]
      public void TheOAuth2Fields_FollowTheirSwitch()
      {
         string section = AuthenticationSection_();
         Assert.Contains("EnabledByKey = \"OAuth2Enabled\"", section, StringComparison.Ordinal);
         Assert.Contains("Label = L(\"Accept OAuth2 bearer tokens (XOAUTH2)\"), Blurb = L(\"While this is off the server uses none of the settings below, so they cannot be changed until it is on.\")", section, StringComparison.Ordinal);

         string code = ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml.cs"));
         string build = WithheldRowTests.MemberText(code, "private void BuildUi()");
         Assert.True(build != null, "FeatureSettingsView.BuildUi is gone.");
         Assert.Contains("WireEnabledBy_(card);", build, StringComparison.Ordinal);
         Assert.Contains("editors_[setting] = editor;", build, StringComparison.Ordinal);

         string wire = WithheldRowTests.MemberText(code, "private void WireEnabledBy_(CardDef card)");
         Assert.True(wire != null, "FeatureSettingsView.WireEnabledBy_ is gone.");
         // The editor, not the row, so the caption still says what the row holds; and
         // a row the environment holds stays disabled whatever the switch says.
         Assert.Contains("built is FieldRow row && row.Content is UIElement content ? content : built", wire, StringComparison.Ordinal);
         Assert.Contains("editor.IsEnabled = allowed && on;", wire, StringComparison.Ordinal);
         Assert.Contains("toggle.OnEditorChanged(Follow);", wire, StringComparison.Ordinal);
      }

      /// <summary>
      /// The aliases are where their subjects are: the second factor on
      /// Administrative access, the mechanisms on Protocols.
      /// </summary>
      [Fact]
      public void TheAliases_AreOnThePagesThatHoldTheirSubjects()
      {
         NavNode authentication = NavigationMap.Find("authentication");
         NavNode adminAccess = NavigationMap.Find("adminaccess");
         NavNode protocols = NavigationMap.Find("protocols");

         foreach (string gone in new[] { "Two-factor", "SASL", "CRAM-MD5", "APOP" })
            Assert.DoesNotContain(gone, authentication.Aliases, StringComparer.OrdinalIgnoreCase);

         foreach (string alias in new[] { "Two-factor", "2FA", "TOTP", "Password policy", "Password expiry", "Password history", "Breached passwords" })
            Assert.Contains(alias, adminAccess.Aliases, StringComparer.OrdinalIgnoreCase);

         foreach (string alias in new[] { "SASL", "AUTH PLAIN", "AUTH LOGIN", "CRAM-MD5", "APOP" })
            Assert.Contains(alias, protocols.Aliases, StringComparer.OrdinalIgnoreCase);

         // The pages a reader of Authentication needs next, the mechanisms first.
         Assert.Equal(new[] { "protocols", "ldap", "adminaccess" }, authentication.SeeAlso.Take(3).ToArray());
      }

      /// <summary>What each search opens: the page that holds its subject, first.</summary>
      [Theory]
      [InlineData("Two-factor", "adminaccess")]
      [InlineData("2FA", "adminaccess")]
      [InlineData("TOTP", "adminaccess")]
      [InlineData("Password policy", "adminaccess")]
      [InlineData("Password expiry", "adminaccess")]
      [InlineData("Breached passwords", "adminaccess")]
      [InlineData("SASL", "protocols")]
      [InlineData("CRAM-MD5", "protocols")]
      [InlineData("APOP", "protocols")]
      [InlineData("Password hashing", "authentication")]
      [InlineData("XOAUTH2", "authentication")]
      public void ASearch_OpensThePageThatHoldsItsSubject(string query, string page)
      {
         IReadOnlyList<PaletteRow> rows = PaletteSearch.Query(query, null, null);
         int first = PaletteSearch.FirstSelectable(rows);
         Assert.True(first >= 0, "'" + query + "' matched nothing at all.");
         Assert.Equal(page, rows[first].Page);
      }

      /// <summary>
      /// Administrative access says what it holds: the subtitle and the description
      /// name the mailbox password rules, which "The credentials used to administer
      /// this server." left out.
      /// </summary>
      [Fact]
      public void AdministrativeAccess_SaysItHoldsTheMailboxPasswordRules()
      {
         const string sentence = "The administrator's password and second factor, and the rules every mailbox password is held to: length and characters, common and breached passwords, reuse and expiry.";
         Assert.Equal(sentence, NavigationMap.Find("adminaccess").Purpose);

         string build = WithheldRowTests.MemberText(ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs")), "private void BuildAdminAccess()");
         Assert.True(build != null, "ServerSettingsView.BuildAdminAccess is gone.");
         Assert.Contains("Header.Subtitle = L(\"" + sentence + "\");", build, StringComparison.Ordinal);

         // Each rule its cards hold, so that the list after the colon is all of them.
         foreach ((string path, string word) in new[]
         {
            ("PasswordPolicyMinimumLength", "length"), ("PasswordPolicyRequireMixedCase", "characters"),
            ("PasswordPolicyRequireDigit", "characters"), ("PasswordPolicyRequireNonAlphanumeric", "characters"),
            ("PasswordPolicyRejectCommon", "common"), ("BreachCheckEnabled", "breached"),
            ("PasswordPolicyHistoryCount", "reuse"), ("PasswordPolicyMaximumAgeDays", "expiry")
         })
         {
            Assert.Contains("Path = \"" + path + "\"", build, StringComparison.Ordinal);
            Assert.Contains(word, sentence, StringComparison.Ordinal);
         }
      }

      private static string AuthenticationSection_()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml.cs"));
         int start = code.IndexOf("case Section.Authentication:", StringComparison.Ordinal);
         Assert.True(start >= 0, "FeatureSettingsView has no Authentication section.");
         int end = code.IndexOf("case Section.Dns:", start, StringComparison.Ordinal);
         Assert.True(end > start, "The Authentication section's end was not found.");
         return code.Substring(start, end - start);
      }

      private static int Count_(string text, string what)
      {
         int count = 0;
         for (int at = text.IndexOf(what, StringComparison.Ordinal); at >= 0; at = text.IndexOf(what, at + what.Length, StringComparison.Ordinal))
            count++;
         return count;
      }

      private static string ReadControlPanelFile(string relative)
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate) && Directory.Exists(Path.Join(directory.FullName, "ControlPanel", "Views")))
               return File.ReadAllText(candidate);
         }

         throw new FileNotFoundException("The Control Panel's source was not found above " + AppContext.BaseDirectory, relative);
      }
   }
}
