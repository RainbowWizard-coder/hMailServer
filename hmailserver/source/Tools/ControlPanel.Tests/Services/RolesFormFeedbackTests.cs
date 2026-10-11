// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Administrator roles page (review findings F088 and F089). Its form said a
   /// missing address or role in an amber Warning notice under the form - the level a
   /// lost link has - and a role given or taken away cleared the boxes and reloaded
   /// without a word; and nothing said where custom roles are made, nor did REST API
   /// keys lead to the page. What is missing is said on its box, the outcome at Good
   /// once the list has been read again, the roles card says where custom roles are
   /// made with the Control Deck's button under it, and REST API keys, whose key holds
   /// a role, names the page among its related pages.
   ///
   /// Scanned rather than rendered: this assembly has no WPF.
   /// </summary>
   public class RolesFormFeedbackTests
   {
      [Fact]
      public void WhatIsMissing_IsSaidOnItsBox()
      {
         string code = Source();
         string assign = WithheldRowTests.MemberText(code, "private void Assign_()");
         Assert.True(assign != null, "RolesView.Assign_ is gone.");

         Assert.Contains("DialogFields.ClearErrors(accountRow_, roleRow_);", assign, StringComparison.Ordinal);
         Assert.Contains("accountRow_.Error = L(\"Enter the account's address.\");", assign, StringComparison.Ordinal);
         Assert.Contains("roleRow_.Error = L(\"Choose a role to give.\");", assign, StringComparison.Ordinal);
         Assert.Contains("if (DialogFields.ShowFirstError(accountRow_, roleRow_))", assign, StringComparison.Ordinal);
         Assert.DoesNotContain("Warn_(", code, StringComparison.Ordinal);

         Assert.Contains("accountRow_ = FormField_(L(\"_Account\"), address_);", code, StringComparison.Ordinal);
         Assert.Contains("roleRow_ = FormField_(L(\"R_ole\"), roleBox);", code, StringComparison.Ordinal);
      }

      /// <summary>
      /// The buttons stay on the boxes' line when a message appears under a box: each
      /// is under a row that shares the captions' height, as the incoming relays' Add is.
      /// </summary>
      [Fact]
      public void TheButtons_StayBesideTheBoxes()
      {
         string code = Source();
         Assert.Contains("Grid.SetIsSharedSizeScope(form, true);", code, StringComparison.Ordinal);
         Assert.Contains("LabelGroup = RoleFormLine", code, StringComparison.Ordinal);
         Assert.Contains("form.Children.Add(BesideTheBoxes_(give_, new Thickness(0, 0, 8, 12)));", code, StringComparison.Ordinal);
         Assert.Contains("form.Children.Add(BesideTheBoxes_(take_, new Thickness(0, 0, 0, 12)));", code, StringComparison.Ordinal);

         string line = WithheldRowTests.MemberText(code, "private static Grid BesideTheBoxes_(FrameworkElement button, Thickness margin)");
         Assert.True(line != null, "RolesView.BesideTheBoxes_ is gone.");
         Assert.Contains("SharedSizeGroup = RoleFormLine", line, StringComparison.Ordinal);
         Assert.Contains("Grid.SetRow(button, 1);", line, StringComparison.Ordinal);
         Assert.DoesNotContain("VerticalAlignment = VerticalAlignment.Bottom", code, StringComparison.Ordinal);
      }

      /// <summary>A change says what it did once the list shows it: Reload_ empties the form's notice first.</summary>
      [Fact]
      public void AChange_SaysWhatItDid_AfterTheListIsReadAgain()
      {
         string code = Source();
         Assert.Contains("Write_(address, role, RoleWords.DomainsToSend(domains_.Text), F(\"{0} now holds {1}.\", address, role));", code, StringComparison.Ordinal);
         Assert.Contains("Write_(row.Address, \"\", \"\", F(\"{0} no longer holds {1}, and holds what its admin level gives it.\", row.Address, row.Role));", code, StringComparison.Ordinal);

         string write = WithheldRowTests.MemberText(code, "private void Write_(string address, string role, string domains, string outcome)");
         Assert.True(write != null, "RolesView.Write_ takes no outcome.");
         int reload = write.IndexOf("Reload_();", StringComparison.Ordinal);
         int good = write.IndexOf("notice_.Level = StatusLevel.Good;", StringComparison.Ordinal);
         int said = write.IndexOf("notice_.Text = outcome;", StringComparison.Ordinal);
         Assert.True(reload >= 0 && good > reload && said > good, "The outcome is not said at Good after the list is read again.");
      }

      [Fact]
      public void TheRolesCard_SaysWhereCustomRolesAreMade_WithTheDecksButton()
      {
         string code = Source();
         // Between t31a's two sentences on the card (review finding F085).
         Assert.Contains("L(\"Custom roles are made over the REST API, and the Control Deck deletes one that nobody holds.\"),", code, StringComparison.Ordinal);
         Assert.Contains("rolesContent.Children.Add(deckRow_);", code, StringComparison.Ordinal);

         string reload = WithheldRowTests.MemberText(code, "private void Reload_()");
         Assert.True(reload != null, "RolesView.Reload_ is gone.");
         // Read again with the roles, before a read that fails returns.
         int deck = reload.IndexOf("deckRow_.Content = ControlDeckLink.Row(L(\"Open the Control Deck\"));", StringComparison.Ordinal);
         int read = reload.IndexOf("try", StringComparison.Ordinal);
         Assert.True(deck >= 0 && deck < read, "The Control Deck's button is not read again with the roles.");
      }

      /// <summary>
      /// What the card says the Deck does is what the Deck does: its Roles view
      /// deletes a role and makes none (installation/WebAdmin/index.html), so a custom
      /// role is made over REST alone (docs/Roles.md, Custom roles). A Deck that gains a
      /// way to make one fails this, and the card's sentence must say so.
      /// </summary>
      [Fact]
      public void TheDeck_DeletesRoles_AndMakesNone()
      {
         string deck = null;
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null && deck == null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "installation", "WebAdmin", "index.html");
            if (File.Exists(candidate))
               deck = File.ReadAllText(candidate);
         }

         Assert.True(deck != null, "installation/WebAdmin/index.html could not be found from the test binaries.");
         Assert.Contains("await api(\"/api/v1/roles/\"+encodeURIComponent(name),{method:\"DELETE\"});", deck, StringComparison.Ordinal);
         Assert.False(Regex.IsMatch(deck, @"api\(""/api/v1/roles""\s*,\s*\{\s*method\s*:\s*""POST"""), "The Control Deck makes roles now; the roles card says it does not.");
      }

      [Fact]
      public void TheRolesPage_AndTheAccessPages_LeadToEachOther()
      {
         Assert.Contains("roles", NavigationMap.Find("apikeys").SeeAlso);
         Assert.Contains("roles", NavigationMap.Find("adminaccess").SeeAlso);
         Assert.Contains("adminaccess", NavigationMap.Find("roles").SeeAlso);
         Assert.True(NavigationMap.Find("apikeys").SeeAlso.Distinct().Count() == NavigationMap.Find("apikeys").SeeAlso.Count);
      }

      private static string Source()
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "RolesView.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate).Replace("\r\n", "\n");
         }

         Assert.Fail("ControlPanel/Views/RolesView.cs could not be found from the test binaries.");
         return null;
      }
   }
}
