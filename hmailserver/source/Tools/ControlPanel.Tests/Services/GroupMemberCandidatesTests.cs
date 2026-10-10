// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;
using Account = hMailServer.ControlPanel.Services.GroupMemberCandidates.Account;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The group members dialog's Add account… (review finding F203): a list under a
   /// search box of the accounts that are not members yet, several of which can be
   /// added at once. It was a drop-down that could not be typed in, of every address
   /// on the server - the members among them, refused only once picked - one account
   /// at a time.
   /// </summary>
   [Collection("Loc")]
   public class GroupMemberCandidatesTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      [Fact]
      public void TheMembers_AreLeftOut_AndTheRestListedByAddressWithoutRegardToCase()
      {
         var accounts = new[]
         {
            new Account(4, "zoe@example.com"),
            new Account(2, "Bob@example.com"),
            new Account(7, "alice@example.com"),
            new Account(9, "carol@example.org")
         };

         IReadOnlyList<Account> offered = GroupMemberCandidates.NotMembers(accounts, new[] { 7, 9 });

         Assert.Equal(new[] { "Bob@example.com", "zoe@example.com" }, offered.Select(a => a.Address));
         Assert.Equal(new[] { 2, 4 }, offered.Select(a => a.Id));
      }

      [Fact]
      public void AnAccountReadTwice_IsOfferedOnce_AndOneWithNoAddressNotAtAll()
      {
         var accounts = new[]
         {
            new Account(3, "dave@example.com"),
            new Account(3, "dave@example.com"),
            new Account(5, ""),
            new Account(6, null),
            null
         };

         IReadOnlyList<Account> offered = GroupMemberCandidates.NotMembers(accounts, Array.Empty<int>());

         Assert.Single(offered);
         Assert.Equal("dave@example.com", offered[0].ToString());
      }

      [Fact]
      public void EveryAccountAMember_LeavesNothingToOffer()
      {
         var accounts = new[] { new Account(1, "a@example.com"), new Account(2, "b@example.com") };
         Assert.Empty(GroupMemberCandidates.NotMembers(accounts, new[] { 1, 2 }));
         Assert.Empty(GroupMemberCandidates.NotMembers(null, null));
      }

      [Fact]
      public void TheAdd_IsSaidByAddressForOne_AndCountedForMore()
      {
         Assert.Equal("dave@example.com added to the group.", GroupMemberCandidates.Added(new[] { "dave@example.com" }));
         Assert.Equal("3 accounts added to the group.", GroupMemberCandidates.Added(new[] { "a@example.com", "b@example.com", "c@example.com" }));
         Assert.Equal("No account that is not a member has \"xyz\" in its address.", GroupMemberCandidates.NoneMatches("xyz"));
      }

      /// <summary>
      /// The picker is a list of several choices under a search that filters it as it
      /// is typed in, of the accounts NotMembers leaves; no drop-down of every address.
      /// </summary>
      [Fact]
      public void ThePicker_IsASearchedListOfSeveralChoices_OfTheAccountsThatAreNotMembers()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "GroupMembersDialog.cs"));

         string add = WithheldRowTests.MemberText(code, "private void AddMember()");
         Assert.True(add != null, "GroupMembersDialog.AddMember is gone.");
         Assert.Contains("GroupMemberCandidates.NotMembers(everyAccount, memberAccountIds_)", add, StringComparison.Ordinal);
         Assert.Contains("PickAccountDialog.Pick(this, groupName_, candidates)", add, StringComparison.Ordinal);
         Assert.Contains("foreach (GroupMemberCandidates.Account account in chosen)", add, StringComparison.Ordinal);
         Assert.Contains("GroupMemberCandidates.Added(added)", add, StringComparison.Ordinal);

         string pick = WithheldRowTests.MemberText(code, "internal static FramedDialog Build(");
         Assert.True(pick != null, "PickAccountDialog.Build is gone.");
         Assert.Contains("SelectionMode = SelectionMode.Extended", pick, StringComparison.Ordinal);
         Assert.Contains("ListSearch.Apply(list, query,", pick, StringComparison.Ordinal);
         Assert.Contains("search.PreviewKeyDown", pick, StringComparison.Ordinal);
         Assert.Contains("SelectionGate.Bind(list, ok);", pick, StringComparison.Ordinal);
         Assert.DoesNotContain("ComboBox", pick, StringComparison.Ordinal);
      }

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
