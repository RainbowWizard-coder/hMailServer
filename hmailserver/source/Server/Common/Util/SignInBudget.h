// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Who may push whom out of a full table of sign-in state.
//
// Two tables hold what a sign-in leaves in memory: the browser sessions of the
// REST listener (RestApiServer.cpp), each standing for an account, the
// administrator or a support session; and a passkey registration's challenge,
// held by the credential that asked (PasskeyStore). Each is bounded, so each has
// to make room when it is full, and until 6.3.6 each made room by dropping its
// oldest entry, whoever it belonged to: a mailbox user who signed in over and
// over with their own password pushed every other session out of the table, the
// administrator's Control Deck included.
//
// The pending halves of the sign-ins that begin with no credential at all - an
// OpenID Connect start, a SAML start, a passkey sign-in's challenge - were three
// more such tables, which a stranger asking for starts filled; 6.3.6 first held
// each source to its share of them under this rule, which a pool of addresses as
// large as a table still beat, and since its second review they are not held
// here at all: the browser carries each one sealed (SignInState.h).
//
// THE RULE, the same for both. Every entry has a HOLDER - the principal a
// session stands for, the credential a registration is for - and room is made
// for one more entry of a holder as follows:
//
//   1. A holder at its own cap gives up its own oldest entry. The cap is what
//      keeps one principal or one source from taking a table over.
//   2. A table that is full gives up the oldest entry of the holder that holds
//      the most entries - the newcomer's own when it is among them. Whoever
//      fills a table pays for it out of their own entries, and a holder with one
//      entry loses it only when no holder has more than one, which is a table
//      genuinely full of different holders.
//   3. Among holders tied for the most, the one whose GROUP holds the most
//      entries gives up its oldest, and among those the holder whose oldest is
//      the oldest. A group is what one party commonly holds many holders of: an
//      IPv6 /48 (a site's allocation, which a tunnel broker hands anybody for
//      free and which holds 65,536 /64s), an IPv4 /24, an account's domain. So
//      a table filled by thousands of single-entry sources out of one /48 gives
//      up those sources' entries, not the person's from elsewhere whose entry
//      happens to be the oldest (6.3.6's re-check; first written, the oldest of
//      the tied went, which a /48 could aim at everybody else).
//   4. An EXEMPT entry (the administrator's sessions) is never given up for
//      another holder and does not count towards the table's size: it is held
//      by its own cap alone, so nobody but the administrator can push an
//      administrator's session out, however many accounts they hold.
//
// "Oldest" is the table's own: the least recently used for a session, the first
// made for a registration's challenge. A table whose erase leaves its other
// entries where they are (the std::map of the registrations) is made room in by
// MakeRoom, which weighs a full table once for a batch; the sessions, a vector,
// by Victim one entry at a time.
//
// GuessSource is the source a guess of the administrator password is counted
// against, for its failed-sign-in throttle: the peer address, an IPv6 one reduced
// to its /64 - one host's allocation, which a single machine can fill with as many
// addresses as it likes, so that a guesser rotating through them shares one delay -
// and an IPv4 address carried in IPv6 read as the IPv4: v4-mapped, NAT64's and
// SIIT's 64:ff9b::/96 (one /64 of which is every IPv4 client of the translator)
// and a Teredo client's own address. No header is believed for it. RefusalKey is
// the same reading for the REST listener's refused set.
//
// SignInBudgetTester holds the rule and the sources; ClassTester runs it on
// Windows and sign-in-budget-tests on Linux, beside PasskeyStoreTester and
// SignInStateTester.
#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace HM
{
   class IPAddress;

   class SignInBudget
   {
   public:

      // The source a guess of the administrator password is counted against: the
      // peer, an IPv6 address reduced to "<prefix>::/64", an IPv4 address carried
      // in IPv6 read as the IPv4 - v4-mapped, under the NAT64 and SIIT well-known
      // prefix 64:ff9b::/96, a Teredo client's own - the loopback addresses as they
      // are. No header.
      static std::string GuessSource(const IPAddress &peer);

      // The halves the REST listener's refused set holds peer by
      // (RestApiServer::RefuseAddress_): an IPv6 address by its /64 (the low half
      // 0), an IPv4 address carried in IPv6 as GuessSource reads it, in the form
      // ::ffff:a.b.c.d has, and the other addresses whole.
      static void RefusalKey(const IPAddress &peer, unsigned __int64 &high, unsigned __int64 &low);

      // The group of a source, for rule 3: an IPv6 "<prefix>::/64" source's /48
      // ("<prefix>::/48"), an IPv4 source's /24 ("a.b.c.0/24"); the source itself
      // for anything else - "", the loopback addresses.
      static std::string SourceGroup(const std::string &source);

      // The entry of table that goes before one more entry of holder is added, or
      // table.end() when none has to. The caller erases it and asks again until
      // the answer is end(). perHolder 0 is no cap for this holder; most is the
      // table's size, counted over the entries that are not exempt. holderOf(entry)
      // and groupOf(entry) answer a reference to the entry's holder and group,
      // held in the entry (one holder is always in one group); ageOf(entry) a
      // number that is smaller for an older (less recently used) entry; and
      // exemptOf(entry) whether rule 4 keeps it out of rules 2 and 3. A newcomer
      // whose own entries are exempt passes the most it may hold as most (its cap
      // already does the bounding), so that it never takes from anybody else.
      template <typename Table, typename HolderOf, typename GroupOf, typename AgeOf, typename ExemptOf>
      static typename Table::iterator Victim(Table &table, const std::string &holder, size_t perHolder, size_t most,
                                             HolderOf holderOf, GroupOf groupOf, AgeOf ageOf, ExemptOf exemptOf)
      {
         typedef typename Table::iterator Iterator;

         size_t counted = 0;
         Iterator own = Scan_(table, holder, perHolder, holderOf, ageOf, exemptOf, counted);
         if (own != table.end())
            return own;

         if (counted == 0 || counted < most)
            return table.end();

         std::vector<Iterator> chosen;
         Choose_(table, holder, counted, 1, holderOf, groupOf, ageOf, exemptOf, chosen);
         return chosen.empty() ? table.end() : chosen.front();
      }

      // Victim's rule for a table whose erase leaves its other entries where they
      // are (a std::map), making the room itself. The newcomer's own entries at
      // its cap go one at a time, as Victim gives them; a full table is weighed
      // once for a batch of a sixty-fourth of its size, given up one entry after
      // another exactly as Victim would choose them. Weighing a table of four
      // thousand single-entry sources takes some hundreds of microseconds under
      // the table's lock, so a stranger with a /48 who kept it full would
      // otherwise have every request they made cost that much; a batch pays it
      // once per sixty-four.
      template <typename Map, typename HolderOf, typename GroupOf, typename AgeOf, typename ExemptOf>
      static void MakeRoom(Map &table, const std::string &holder, size_t perHolder, size_t most,
                           HolderOf holderOf, GroupOf groupOf, AgeOf ageOf, ExemptOf exemptOf)
      {
         typedef typename Map::iterator Iterator;

         size_t counted = 0;
         for (;;)
         {
            Iterator own = Scan_(table, holder, perHolder, holderOf, ageOf, exemptOf, counted);
            if (own == table.end())
               break;
            table.erase(own);
         }

         if (counted == 0 || counted < most)
            return;

         const size_t needed = counted - most + 1;
         const size_t batch = most / 64 > needed ? most / 64 : needed;

         std::vector<Iterator> chosen;
         Choose_(table, holder, counted, batch, holderOf, groupOf, ageOf, exemptOf, chosen);
         for (size_t i = 0; i < chosen.size(); i++)
            table.erase(chosen[i]);
      }

   private:

      // One walk of the table for rule 1 and rule 4: the newcomer's own oldest
      // when it is at its cap, else end(); and in counted the entries that count
      // towards the table's size.
      template <typename Table, typename HolderOf, typename AgeOf, typename ExemptOf>
      static typename Table::iterator Scan_(Table &table, const std::string &holder, size_t perHolder,
                                            HolderOf holderOf, AgeOf ageOf, ExemptOf exemptOf, size_t &counted)
      {
         typedef typename Table::iterator Iterator;

         counted = 0;
         size_t own = 0;
         Iterator oldestOwn = table.end();
         for (Iterator it = table.begin(); it != table.end(); ++it)
         {
            if (!exemptOf(*it))
               counted++;

            if (perHolder == 0 || holderOf(*it) != holder)
               continue;
            own++;
            if (oldestOwn == table.end() || ageOf(*it) < ageOf(*oldestOwn))
               oldestOwn = it;
         }

         return perHolder > 0 && own >= perHolder ? oldestOwn : table.end();
      }

      // Rules 2 and 3, for picks entries in turn: each pick is the oldest entry of
      // the holder holding the most - the newcomer's own when it is among them,
      // else the one in the largest group, else the one whose oldest is the
      // oldest - and is counted out before the next pick is weighed. The exempt
      // entries are never weighed. The tally is keyed by the strings in the table
      // itself, which stay put until the caller erases, and kept in one array, so
      // a table of four thousand single-entry sources copies none of them and is
      // walked quickly for each pick.
      template <typename Table, typename HolderOf, typename GroupOf, typename AgeOf, typename ExemptOf>
      static void Choose_(Table &table, const std::string &holder, size_t counted, size_t picks,
                          HolderOf holderOf, GroupOf groupOf, AgeOf ageOf, ExemptOf exemptOf,
                          std::vector<typename Table::iterator> &chosen)
      {
         typedef typename Table::iterator Iterator;

         struct TextHash
         {
            size_t operator()(const std::string *text) const { return std::hash<std::string>()(*text); }
         };

         struct TextEqual
         {
            bool operator()(const std::string *left, const std::string *right) const { return *left == *right; }
         };

         struct Tally
         {
            size_t count = 0;              // entries not yet picked
            size_t next = 0;               // its oldest not yet picked, in entries
            unsigned long long next_age = 0;
            const std::string *group = nullptr;
            size_t *group_count = nullptr; // entries of its group not yet picked
         };

         struct Entry
         {
            size_t holder;                 // its index in tallies
            unsigned long long age;
            Iterator it;
         };

         // Each holder's index in tallies, and each entry with its holder's index.
         std::unordered_map<const std::string *, size_t, TextHash, TextEqual> indexOf;
         indexOf.reserve(counted);
         std::vector<Tally> tallies;
         tallies.reserve(counted);
         std::vector<Entry> entries;
         entries.reserve(counted);

         for (Iterator it = table.begin(); it != table.end(); ++it)
         {
            if (exemptOf(*it))
               continue;

            std::pair<typename std::unordered_map<const std::string *, size_t, TextHash, TextEqual>::iterator, bool> added =
               indexOf.insert(std::make_pair(&holderOf(*it), tallies.size()));
            if (added.second)
            {
               tallies.push_back(Tally());
               tallies.back().group = &groupOf(*it);
            }

            const size_t index = added.first->second;
            tallies[index].count++;
            const Entry entry = { index, (unsigned long long) ageOf(*it), it };
            entries.push_back(entry);
         }

         if (tallies.empty())
            return;

         // Each holder's entries together, oldest first.
         std::sort(entries.begin(), entries.end(),
            [](const Entry &left, const Entry &right)
            {
               if (left.holder != right.holder)
                  return left.holder < right.holder;
               return left.age < right.age;
            });

         for (size_t i = entries.size(); i > 0; i--)
         {
            tallies[entries[i - 1].holder].next = i - 1;
            tallies[entries[i - 1].holder].next_age = entries[i - 1].age;
         }

         std::unordered_map<const std::string *, size_t, TextHash, TextEqual> groups;
         groups.reserve(tallies.size());
         for (size_t i = 0; i < tallies.size(); i++)
            groups[tallies[i].group] += tallies[i].count;
         for (size_t i = 0; i < tallies.size(); i++)
            tallies[i].group_count = &groups[tallies[i].group];

         typename std::unordered_map<const std::string *, size_t, TextHash, TextEqual>::const_iterator ownIndex = indexOf.find(&holder);
         const size_t newcomer = ownIndex == indexOf.end() ? tallies.size() : ownIndex->second;

         for (size_t pick = 0; pick < picks; pick++)
         {
            size_t best = tallies.size();
            for (size_t i = 0; i < tallies.size(); i++)
            {
               const Tally &tally = tallies[i];
               if (tally.count == 0)
                  continue;

               if (best == tallies.size())
               {
                  best = i;
                  continue;
               }

               const Tally &current = tallies[best];
               bool better = false;
               if (tally.count != current.count)
                  better = tally.count > current.count;
               else if ((i == newcomer) != (best == newcomer))
                  better = i == newcomer;
               else if (*tally.group_count != *current.group_count)
                  better = *tally.group_count > *current.group_count;
               else
                  better = tally.next_age < current.next_age;

               if (better)
                  best = i;
            }

            if (best == tallies.size())
               return;

            Tally &taken = tallies[best];
            chosen.push_back(entries[taken.next].it);
            taken.next++;
            taken.count--;
            (*taken.group_count)--;
            if (taken.count > 0)
               taken.next_age = entries[taken.next].age;
         }
      }
   };

   // The rule and the sources, without a server: run by ClassTester on Windows and
   // by sign-in-budget-tests on Linux. Throws std::logic_error on the first failure.
   class SignInBudgetTester
   {
   public:
      void Test();

   private:
      void TestSources_();
      void TestRule_();
      void TestFloods_();
   };
}
