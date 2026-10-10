// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The accounts the group members dialog offers to add, and what it says after
   /// an add (review finding F203). Add account… was a drop-down that could not be
   /// typed in, of every address on the server, the group's own members among them,
   /// one account at a time: a member picked again was refused only after the pick,
   /// and with hundreds of mailboxes there was no way to find one but to scroll. The
   /// picker is now a list under a search box, of the accounts that are not members
   /// yet, several of which can be added at once.
   ///
   /// No WPF here, so the tests hold the list and the words.
   /// </summary>
   public static class GroupMemberCandidates
   {
      /// <summary>An account as the picker lists it: its database id and its address.</summary>
      public sealed class Account
      {
         public Account(int id, string address)
         {
            Id = id;
            Address = address ?? "";
         }

         public int Id { get; }
         public string Address { get; }

         // What the list shows and what its search reads.
         public override string ToString() => Address;
      }

      /// <summary>
      /// The accounts that are not members of the group yet, each once, by address
      /// in the order a reader looks one up - without regard to case. An account
      /// with no address is left out, as there is nothing to find it by.
      /// </summary>
      public static IReadOnlyList<Account> NotMembers(IEnumerable<Account> accounts, IEnumerable<int> memberAccountIds)
      {
         var members = new HashSet<int>(memberAccountIds ?? Enumerable.Empty<int>());
         return (accounts ?? Enumerable.Empty<Account>())
            .Where(account => account != null && account.Address.Length > 0 && !members.Contains(account.Id))
            .GroupBy(account => account.Id)
            .Select(group => group.First())
            .OrderBy(account => account.Address, StringComparer.OrdinalIgnoreCase)
            .ToList();
      }

      /// <summary>
      /// What the dialog says after an add: the account by its address when there
      /// is one, and how many otherwise.
      /// </summary>
      public static string Added(IReadOnlyList<string> addresses)
      {
         int count = addresses?.Count ?? 0;
         if (count == 1)
            return F("{0} added to the group.", addresses[0]);
         return Plural(count, "{0} account added to the group.", "{0} accounts added to the group.");
      }

      /// <summary>What the dialog says, in place of the picker, when every account is a member already.</summary>
      public static string EveryAccountIsAMember => L("Every account on this server is a member of this group already.");

      /// <summary>What the picker's list says when the search hides every account.</summary>
      public static string NoneMatches(string query) => F("No account that is not a member has \"{0}\" in its address.", query);
   }
}
