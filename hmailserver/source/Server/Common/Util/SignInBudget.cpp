// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "SignInBudget.h"
#include "../TCPIP/IPAddress.h"

#include <boost/asio/ip/address.hpp>

#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The IPv4 address an IPv6 address carries for a client on the IPv4 side of
      // a translator or a tunnel, for which the /64 is no host's own: v4-mapped
      // (::ffff:a.b.c.d), the NAT64 and SIIT well-known prefix 64:ff9b::/96 (RFC
      // 6052 section 2.1), where one /64 is every IPv4 client of the translator,
      // and Teredo's 2001::/32 (RFC 4380 section 4), where it is every client of
      // one Teredo server and the client's own address is the last 32 bits,
      // inverted. No setting names another translation prefix.
      bool EmbeddedIPv4(const boost::asio::ip::address_v6 &v6, boost::asio::ip::address_v4 &embedded)
      {
         if (v6.is_v4_mapped())
         {
            embedded = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6);
            return true;
         }

         const boost::asio::ip::address_v6::bytes_type bytes = v6.to_bytes();
         boost::asio::ip::address_v4::bytes_type v4 = {};

         const bool wellKnownPrefix = bytes[0] == 0x00 && bytes[1] == 0x64 && bytes[2] == 0xff && bytes[3] == 0x9b &&
                                      bytes[4] == 0 && bytes[5] == 0 && bytes[6] == 0 && bytes[7] == 0 &&
                                      bytes[8] == 0 && bytes[9] == 0 && bytes[10] == 0 && bytes[11] == 0;
         if (wellKnownPrefix)
         {
            for (size_t i = 0; i < 4; i++)
               v4[i] = bytes[12 + i];
            embedded = boost::asio::ip::address_v4(v4);
            return true;
         }

         const bool teredo = bytes[0] == 0x20 && bytes[1] == 0x01 && bytes[2] == 0x00 && bytes[3] == 0x00;
         if (teredo)
         {
            for (size_t i = 0; i < 4; i++)
               v4[i] = static_cast<unsigned char>(~bytes[12 + i]);
            embedded = boost::asio::ip::address_v4(v4);
            return true;
         }

         return false;
      }

      // An address as a holder: an IPv4 address carried in IPv6 as the IPv4 (see
      // EmbeddedIPv4 for which), the IPv6 loopback as it is, any other IPv6
      // address as its /64.
      std::string Holder(const boost::asio::ip::address &address)
      {
         if (address.is_v4())
            return address.to_v4().to_string();

         const boost::asio::ip::address_v6 v6 = address.to_v6();
         boost::asio::ip::address_v4 embedded;
         if (EmbeddedIPv4(v6, embedded))
            return embedded.to_string();

         if (v6.is_loopback())
            return v6.to_string();

         boost::asio::ip::address_v6::bytes_type bytes = v6.to_bytes();
         for (size_t i = 8; i < bytes.size(); i++)
            bytes[i] = 0;

         return boost::asio::ip::address_v6(bytes).to_string() + "/64";
      }
   }

   std::string
   SignInBudget::GuessSource(const IPAddress &peer)
   {
      if (peer.IsAny())
         return std::string(peer.ToString().c_str());

      return Holder(peer.GetAddress());
   }

   void
   SignInBudget::RefusalKey(const IPAddress &peer, unsigned __int64 &high, unsigned __int64 &low)
   {
      high = peer.GetAddress1();
      low = peer.GetAddress2();

      if (peer.GetType() != IPAddress::IPV6)
         return;

      boost::asio::ip::address_v4 embedded;
      if (EmbeddedIPv4(peer.GetAddress().to_v6(), embedded))
      {
         // As ::ffff:a.b.c.d is held: its halves.
         high = 0;
         low = 0x0000FFFF00000000ULL | static_cast<unsigned __int64>(embedded.to_uint());
         return;
      }

      // The other addresses whose high half is zero (::1, the other special
      // forms) are held whole; any other IPv6 address by its /64.
      if (high != 0)
         low = 0;
   }

   std::string
   SignInBudget::SourceGroup(const std::string &source)
   {
      const std::string suffix = "/64";
      const bool prefix64 = source.size() > suffix.size() &&
                            source.compare(source.size() - suffix.size(), suffix.size(), suffix) == 0;
      const std::string text = prefix64 ? source.substr(0, source.size() - suffix.size()) : source;

      boost::system::error_code error;
      const boost::asio::ip::address address = boost::asio::ip::make_address(text, error);
      if (error)
         return source;

      if (address.is_v4())
      {
         if (address.to_v4().is_loopback())
            return source;

         boost::asio::ip::address_v4::bytes_type bytes = address.to_v4().to_bytes();
         bytes[3] = 0;
         return boost::asio::ip::address_v4(bytes).to_string() + "/24";
      }

      // A whole IPv6 address (::1) is not a /64 source and stays as it is.
      if (!prefix64)
         return source;

      boost::asio::ip::address_v6::bytes_type bytes = address.to_v6().to_bytes();
      for (size_t i = 6; i < bytes.size(); i++)
         bytes[i] = 0;

      return boost::asio::ip::address_v6(bytes).to_string() + "/48";
   }

   // ------------------------------------------------------------------------
   // The tests.
   // ------------------------------------------------------------------------

   namespace
   {
      void Expect(bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(what);
      }

      IPAddress Peer(const char *text)
      {
         return IPAddress(boost::asio::ip::make_address(text));
      }

      struct Pending
      {
         std::string source;
         std::string group;
         unsigned __int64 order = 0;
      };

      struct Session
      {
         std::string holder;
         std::string group;
         unsigned __int64 last_seen = 0;
         bool administrator = false;
      };

      const size_t NoTableLimit = std::numeric_limits<size_t>::max();

      // Adds one entry of holder to a map table under the rule, as PasskeyStore's
      // registrations do (and the pending sign-ins did, until 6.3.6's second
      // review sealed them into the browser): the group is the source's.
      void Add(std::map<std::string, Pending> &table, unsigned __int64 &order, const std::string &key,
               const std::string &holder, size_t perHolder, size_t most)
      {
         SignInBudget::MakeRoom(table, holder, perHolder, most,
            [](const std::pair<const std::string, Pending> &entry) -> const std::string & { return entry.second.source; },
            [](const std::pair<const std::string, Pending> &entry) -> const std::string & { return entry.second.group; },
            [](const std::pair<const std::string, Pending> &entry) { return entry.second.order; },
            [](const std::pair<const std::string, Pending> &) { return false; });

         Pending entry;
         entry.source = holder;
         entry.group = SignInBudget::SourceGroup(holder);
         entry.order = ++order;
         table[key] = entry;
      }

      // The same for a vector of sessions, as RestApiServer does: the
      // administrator's exempt and never limited by the table, an account's
      // group its domain.
      void Add(std::vector<Session> &table, const std::string &holder, const std::string &group, bool administrator,
               unsigned __int64 now, size_t perHolder, size_t most)
      {
         for (;;)
         {
            std::vector<Session>::iterator victim = SignInBudget::Victim(table, holder, perHolder,
               administrator ? NoTableLimit : most,
               [](const Session &entry) -> const std::string & { return entry.holder; },
               [](const Session &entry) -> const std::string & { return entry.group; },
               [](const Session &entry) { return entry.last_seen; },
               [](const Session &entry) { return entry.administrator; });
            if (victim == table.end())
               break;
            table.erase(victim);
         }

         Session session;
         session.holder = holder;
         session.group = group;
         session.last_seen = now;
         session.administrator = administrator;
         table.push_back(session);
      }

      // An account's session, in the one domain unless one is named.
      void Add(std::vector<Session> &table, const std::string &holder, bool administrator, unsigned __int64 now,
               size_t perHolder, size_t most)
      {
         Add(table, holder, administrator ? std::string("administrator") : std::string("domain:1"), administrator, now, perHolder, most);
      }

      // A number in lower-case hexadecimal, as an IPv6 group is written.
      std::string Hex(int value)
      {
         const char *digits = "0123456789abcdef";
         std::string text;
         do
         {
            text.insert(text.begin(), digits[value % 16]);
            value /= 16;
         } while (value > 0);
         return text;
      }

      size_t Count(const std::vector<Session> &table, const std::string &holder)
      {
         size_t count = 0;
         for (const Session &session : table)
         {
            if (session.holder == holder)
               count++;
         }
         return count;
      }
   }

   void
   SignInBudgetTester::Test()
   {
      TestSources_();
      TestRule_();
      TestFloods_();
   }

   void
   SignInBudgetTester::TestSources_()
   {
      // The guesser's source: the address, an IPv6 one as its /64.
      Expect(SignInBudget::GuessSource(Peer("192.0.2.10")) == "192.0.2.10",
             "SignInBudgetTester: an IPv4 address is its own source.");
      Expect(SignInBudget::GuessSource(Peer("2001:db8:1:2::5")) == "2001:db8:1:2::/64",
             "SignInBudgetTester: an IPv6 address is counted as its /64.");
      Expect(SignInBudget::GuessSource(Peer("2001:db8:1:2:ffff:ffff:ffff:fffe")) == SignInBudget::GuessSource(Peer("2001:db8:1:2::1")),
             "SignInBudgetTester: two addresses of one /64 are one source.");
      Expect(SignInBudget::GuessSource(Peer("2001:db8:1:3::1")) != SignInBudget::GuessSource(Peer("2001:db8:1:2::1")),
             "SignInBudgetTester: the next /64 is another source.");
      Expect(SignInBudget::GuessSource(Peer("::ffff:192.0.2.7")) == "192.0.2.7",
             "SignInBudgetTester: an IPv4 address carried in IPv6 is the IPv4.");
      Expect(SignInBudget::GuessSource(Peer("::1")) == "::1" && SignInBudget::GuessSource(Peer("127.0.0.1")) == "127.0.0.1",
             "SignInBudgetTester: the loopback addresses are their own sources.");

      // An address a translator or a tunnel gives an IPv4 client is that client:
      // one /64 of them is every client of the translator. Until 6.3.6's re-check
      // they were each the /64, so one client's failures were all of theirs.
      Expect(SignInBudget::GuessSource(Peer("64:ff9b::192.0.2.33")) == "192.0.2.33" &&
             SignInBudget::GuessSource(Peer("64:ff9b::c000:221")) == "192.0.2.33",
             "SignInBudgetTester: an address under the NAT64 well-known prefix is the IPv4 address it carries.");
      Expect(SignInBudget::GuessSource(Peer("64:ff9b::192.0.2.33")) != SignInBudget::GuessSource(Peer("64:ff9b::192.0.2.34")),
             "SignInBudgetTester: two IPv4 clients of one NAT64 translator are two sources.");
      Expect(SignInBudget::GuessSource(Peer("2001:0:4136:e378:8000:63bf:3fff:fdd2")) == "192.0.2.45",
             "SignInBudgetTester: a Teredo address is the client's own IPv4 address, the last 32 bits inverted (RFC 4380 section 4).");
      Expect(SignInBudget::GuessSource(Peer("64:ff9b:0:0:1::5")) == "64:ff9b::/64" &&
             SignInBudget::GuessSource(Peer("2001:db8::1")) == "2001:db8::/64",
             "SignInBudgetTester: an address beside the well-known prefix, and one of the documentation /32, are their /64s.");
      Expect(SignInBudget::SourceGroup(SignInBudget::GuessSource(Peer("64:ff9b::192.0.2.33"))) == "192.0.2.0/24",
             "SignInBudgetTester: a NAT64 client's group is its IPv4 /24.");

      // The halves the REST listener's refused set holds an address by.
      {
         unsigned __int64 high = 1;
         unsigned __int64 low = 1;
         SignInBudget::RefusalKey(Peer("2001:db8:1:2::5"), high, low);
         Expect(high == 0x20010db800010002ULL && low == 0, "SignInBudgetTester: an IPv6 address is refused by its /64.");

         unsigned __int64 mappedHigh = 1;
         unsigned __int64 mappedLow = 1;
         SignInBudget::RefusalKey(Peer("::ffff:192.0.2.33"), mappedHigh, mappedLow);
         SignInBudget::RefusalKey(Peer("64:ff9b::192.0.2.33"), high, low);
         Expect(mappedHigh == 0 && mappedLow == 0x0000FFFFC0000221ULL && high == mappedHigh && low == mappedLow,
                "SignInBudgetTester: a NAT64 client is refused as its IPv4 address carried in IPv6 is, and alone.");

         SignInBudget::RefusalKey(Peer("2001:0:4136:e378:8000:63bf:3fff:fdd2"), high, low);
         Expect(high == 0 && low == 0x0000FFFFC000022DULL, "SignInBudgetTester: a Teredo client is refused as its own IPv4 address.");

         SignInBudget::RefusalKey(Peer("::1"), high, low);
         Expect(high == 0 && low == 1, "SignInBudgetTester: ::1 is refused whole.");

         SignInBudget::RefusalKey(Peer("192.0.2.33"), high, low);
         Expect(high == 0xC0000221ULL && low == 0, "SignInBudgetTester: an IPv4 address is refused as itself.");
      }

      // The groups: an IPv6 source's /48, an IPv4 source's /24, anything else itself.
      Expect(SignInBudget::SourceGroup("2001:db8:1:2::/64") == "2001:db8:1::/48" &&
             SignInBudget::SourceGroup("2001:db8:1:ffff::/64") == SignInBudget::SourceGroup("2001:db8:1:2::/64") &&
             SignInBudget::SourceGroup("2001:db8:2:2::/64") != SignInBudget::SourceGroup("2001:db8:1:2::/64"),
             "SignInBudgetTester: an IPv6 source's group is its /48.");
      Expect(SignInBudget::SourceGroup("192.0.2.10") == "192.0.2.0/24" &&
             SignInBudget::SourceGroup("192.0.2.200") == SignInBudget::SourceGroup("192.0.2.10") &&
             SignInBudget::SourceGroup("192.0.3.10") != SignInBudget::SourceGroup("192.0.2.10"),
             "SignInBudgetTester: an IPv4 source's group is its /24.");
      Expect(SignInBudget::SourceGroup("").empty() && SignInBudget::SourceGroup("::1") == "::1" &&
             SignInBudget::SourceGroup("127.0.0.1") == "127.0.0.1" && SignInBudget::SourceGroup("not an address") == "not an address",
             "SignInBudgetTester: no source, the loopback and anything else are their own group.");
   }

   void
   SignInBudgetTester::TestRule_()
   {
      // Under its cap, in a table with room: nothing goes.
      std::map<std::string, Pending> table;
      unsigned __int64 order = 0;
      Add(table, order, "a1", "a", 3, 10);
      Add(table, order, "a2", "a", 3, 10);
      Add(table, order, "a3", "a", 3, 10);
      Expect(table.size() == 3, "SignInBudgetTester: a holder under its cap in a table with room loses nothing.");

      // 1. At its cap, the holder's own oldest goes, and nobody else's.
      Add(table, order, "b1", "b", 3, 10);
      Add(table, order, "a4", "a", 3, 10);
      Expect(table.count("a1") == 0 && table.count("a2") == 1 && table.count("a4") == 1 && table.count("b1") == 1,
             "SignInBudgetTester: a holder at its cap gives up its own oldest entry.");

      // perHolder 0: no cap of its own.
      std::map<std::string, Pending> uncapped;
      order = 0;
      for (int i = 0; i < 8; i++)
         Add(uncapped, order, "u" + std::to_string(i), "", 0, 10);
      Expect(uncapped.size() == 8, "SignInBudgetTester: a holder with no cap of its own is held by the table alone.");

      // 2. A full table: the holder with the most gives up its oldest.
      std::vector<Session> sessions;
      Add(sessions, "x", false, 1, 0, 6);
      Add(sessions, "y", false, 2, 0, 6);
      Add(sessions, "z", false, 3, 0, 6);
      Add(sessions, "z", false, 4, 0, 6);
      Add(sessions, "z", false, 5, 0, 6);
      Add(sessions, "z", false, 6, 0, 6);
      Add(sessions, "w", false, 7, 0, 6);
      Expect(sessions.size() == 6 && Count(sessions, "x") == 1 && Count(sessions, "y") == 1 && Count(sessions, "z") == 3 && Count(sessions, "w") == 1,
             "SignInBudgetTester: a full table takes from the holder with the most, not the least recently used entry.");
      Expect(sessions[0].holder == "x" && sessions[0].last_seen == 1,
             "SignInBudgetTester: the least recently used entry, a holder's only one, is kept.");

      // The newcomer's own, when it ties for the most.
      std::vector<Session> tied;
      Add(tied, "p", false, 1, 0, 4);
      Add(tied, "q", false, 2, 0, 4);
      Add(tied, "p", false, 3, 0, 4);
      Add(tied, "q", false, 4, 0, 4);
      Add(tied, "q", false, 5, 0, 4);
      Expect(Count(tied, "p") == 2 && Count(tied, "q") == 2,
             "SignInBudgetTester: a newcomer tied for the most gives up its own oldest.");

      // 3. Among holders tied for the most, the one in the largest group gives up
      // its oldest - not the oldest entry of the table, which here is b's.
      std::vector<Session> grouped;
      Add(grouped, "b", "domain:2", false, 1, 0, 4);
      Add(grouped, "a1", "domain:1", false, 2, 0, 4);
      Add(grouped, "a2", "domain:1", false, 3, 0, 4);
      Add(grouped, "a3", "domain:1", false, 4, 0, 4);
      Add(grouped, "a4", "domain:1", false, 5, 0, 4);
      Expect(Count(grouped, "b") == 1 && Count(grouped, "a1") == 0 && Count(grouped, "a4") == 1,
             "SignInBudgetTester: among holders tied for the most, the largest group gives up its oldest.");

      // And in one group, the oldest of the tied goes.
      std::vector<Session> oneGroup;
      Add(oneGroup, "m", false, 1, 0, 3);
      Add(oneGroup, "n", false, 2, 0, 3);
      Add(oneGroup, "o", false, 3, 0, 3);
      Add(oneGroup, "p", false, 4, 0, 3);
      Expect(Count(oneGroup, "m") == 0 && Count(oneGroup, "n") == 1 && Count(oneGroup, "p") == 1,
             "SignInBudgetTester: in one group, the holder whose oldest is the oldest gives it up.");

      // 4. The administrator's sessions are exempt: never taken for anybody else,
      // not when every holder has one...
      std::vector<Session> withAdministrator;
      Add(withAdministrator, "administrator", true, 1, 0, 3);
      Add(withAdministrator, "m", false, 2, 0, 3);
      Add(withAdministrator, "n", false, 3, 0, 3);
      Add(withAdministrator, "o", false, 4, 0, 3);
      Add(withAdministrator, "p", false, 5, 0, 3);
      Expect(Count(withAdministrator, "administrator") == 1 && Count(withAdministrator, "m") == 0 && withAdministrator.size() == 4,
             "SignInBudgetTester: the administrator's session is not taken, and does not count towards the table.");

      // ...and not when it holds the most.
      std::vector<Session> administratorHeavy;
      Add(administratorHeavy, "administrator", true, 1, 2, 3);
      Add(administratorHeavy, "administrator", true, 2, 2, 3);
      Add(administratorHeavy, "m", false, 3, 0, 3);
      Add(administratorHeavy, "n", false, 4, 0, 3);
      Add(administratorHeavy, "o", false, 5, 0, 3);
      Add(administratorHeavy, "p", false, 6, 0, 3);
      Expect(Count(administratorHeavy, "administrator") == 2 && Count(administratorHeavy, "m") == 0 && Count(administratorHeavy, "p") == 1,
             "SignInBudgetTester: the administrator keeps its sessions when it holds the most.");

      // The administrator at its own cap gives up its own oldest.
      Add(administratorHeavy, "administrator", true, 7, 2, 3);
      Expect(Count(administratorHeavy, "administrator") == 2 && administratorHeavy.size() == 5,
             "SignInBudgetTester: the administrator at its cap gives up its own oldest, and nobody else's.");

      // An empty table of size 0 asks nothing.
      std::vector<Session> none;
      Expect(SignInBudget::Victim(none, "a", 1, 0,
                [](const Session &entry) -> const std::string & { return entry.holder; },
                [](const Session &entry) -> const std::string & { return entry.group; },
                [](const Session &entry) { return entry.last_seen; },
                [](const Session &entry) { return entry.administrator; }) == none.end(),
             "SignInBudgetTester: an empty table has nothing to give up.");

      // MakeRoom's batch is what Victim gives one entry at a time: a table of
      // holders holding one to five entries in a handful of groups, the newcomer
      // among them, made room in once by MakeRoom and entry by entry by Victim,
      // keeps the same entries.
      std::map<std::string, Pending> batched;
      unsigned __int64 seed = 12345;
      order = 0;
      for (int holder = 0; holder < 300; holder++)
      {
         seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
         const int entries = 1 + (int) ((seed >> 33) % 5);
         const int group = (int) ((seed >> 20) % 7);
         for (int i = 0; i < entries; i++)
         {
            Pending entry;
            entry.source = "h" + std::to_string(holder);
            entry.group = "g" + std::to_string(group);
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            entry.order = ((seed >> 24) % 1000000) * 10000 + (++order);
            batched["k" + std::to_string(holder) + "-" + std::to_string(i)] = entry;
         }
      }
      std::map<std::string, Pending> oneByOne = batched;
      const size_t before = batched.size();
      const size_t batch = before / 64;

      SignInBudget::MakeRoom(batched, "h7", 0, before,
         [](const std::pair<const std::string, Pending> &entry) -> const std::string & { return entry.second.source; },
         [](const std::pair<const std::string, Pending> &entry) -> const std::string & { return entry.second.group; },
         [](const std::pair<const std::string, Pending> &entry) { return entry.second.order; },
         [](const std::pair<const std::string, Pending> &) { return false; });

      for (size_t i = 0; i < batch; i++)
      {
         std::map<std::string, Pending>::iterator victim = SignInBudget::Victim(oneByOne, "h7", 0, 1,
            [](const std::pair<const std::string, Pending> &entry) -> const std::string & { return entry.second.source; },
            [](const std::pair<const std::string, Pending> &entry) -> const std::string & { return entry.second.group; },
            [](const std::pair<const std::string, Pending> &entry) { return entry.second.order; },
            [](const std::pair<const std::string, Pending> &) { return false; });
         if (victim == oneByOne.end())
            break;
         oneByOne.erase(victim);
      }

      bool same = batch > 1 && batched.size() == before - batch && oneByOne.size() == batched.size();
      for (std::map<std::string, Pending>::const_iterator it = batched.begin(); same && it != batched.end(); ++it)
         same = oneByOne.count(it->first) == 1;
      Expect(same, "SignInBudgetTester: a full table's batch is the entries Victim gives up one at a time.");
   }

   void
   SignInBudgetTester::TestFloods_()
   {
      // A person's pending sign-in, then five thousand starts from one source: the
      // flood spends its own entries. Until 6.3.6 the oldest went whoever held it.
      {
         std::map<std::string, Pending> table;
         unsigned __int64 order = 0;
         Add(table, order, "person", "198.51.100.4", 256, 4096);
         for (int i = 0; i < 5000; i++)
            Add(table, order, "flood" + std::to_string(i), "203.0.113.9", 256, 4096);
         Expect(table.count("person") == 1, "SignInBudgetTester: one source's flood leaves another's sign-in.");
         Expect(table.size() == 257, "SignInBudgetTester: and holds no more than its cap.");
         Expect(table.count("flood4999") == 1 && table.count("flood0") == 0,
                "SignInBudgetTester: the flood's newest is kept and its oldest went.");
      }

      // The same from fifteen hundred sources of one each, which fill no table of
      // four thousand: nobody's goes.
      {
         std::map<std::string, Pending> table;
         unsigned __int64 order = 0;
         Add(table, order, "person", "198.51.100.4", 256, 4096);
         for (int i = 0; i < 1500; i++)
            Add(table, order, "rotating" + std::to_string(i), "src" + std::to_string(i), 256, 4096);
         Expect(table.count("person") == 1, "SignInBudgetTester: a flood from many sources that fills no table takes nothing.");
      }

      // Sixteen sources at once filling a table of 512, each under its cap of 64:
      // still the flooders' own, taken from whichever of them holds the most.
      {
         std::map<std::string, Pending> table;
         unsigned __int64 order = 0;
         Add(table, order, "person", "198.51.100.4", 64, 512);
         for (int round = 0; round < 40; round++)
         {
            for (int source = 0; source < 16; source++)
               Add(table, order, "s" + std::to_string(source) + "-" + std::to_string(round), "flooder" + std::to_string(source), 64, 512);
         }
         Expect(table.count("person") == 1, "SignInBudgetTester: a full table of flooders gives up the flooders' entries.");
         Expect(table.size() <= 512 && table.size() > 512 - 512 / 64,
                "SignInBudgetTester: and stays at its size, less at most one batch.");
      }

      // Starts from more /64s of one /48 - a tunnel broker's free allocation -
      // than the table holds, one each: every holder has one, the table is full,
      // and the /48 gives up its own. Taking the oldest of the tied, as the rule
      // was first written, took the person's. (A table of 1,024 rather than the
      // server's 4,096: the rule does not depend on the size, and the Debug
      // ClassTester walks it for every start.)
      {
         std::map<std::string, Pending> table;
         unsigned __int64 order = 0;
         Add(table, order, "person", "2001:db8:ffff:1::/64", 256, 1024);
         for (int i = 0; i < 1500; i++)
            Add(table, order, "rotating" + std::to_string(i), "2001:db8:1:" + Hex(i + 1) + "::/64", 256, 1024);
         Expect(table.count("person") == 1, "SignInBudgetTester: one /48's thousands of /64s leave a person elsewhere their sign-in.");
         Expect(table.size() <= 1024 && table.size() > 1024 - 1024 / 64,
                "SignInBudgetTester: and the table stays at its size, less at most one batch.");
      }

      // The same from IPv4: a few /24s' worth of addresses, one start each.
      {
         std::map<std::string, Pending> table;
         unsigned __int64 order = 0;
         Add(table, order, "person", "198.51.100.4", 256, 1024);
         for (int i = 0; i < 1500; i++)
         {
            const std::string address = "10." + std::to_string(1 + i / 65536) + "." + std::to_string((i / 256) % 256) + "." + std::to_string(i % 256);
            Add(table, order, "rotating" + std::to_string(i), address, 256, 1024);
         }
         Expect(table.count("person") == 1, "SignInBudgetTester: a few /24s' addresses leave a person elsewhere their sign-in.");
      }

      // The session table: one account signing in a thousand and fifty times leaves
      // the administrator's session and another account's, both idle since.
      {
         std::vector<Session> table;
         unsigned __int64 now = 0;
         Add(table, "administrator", true, ++now, 50, 1000);
         Add(table, "account:2", false, ++now, 50, 1000);
         for (int i = 0; i < 1050; i++)
            Add(table, "account:1", false, ++now, 50, 1000);
         Expect(Count(table, "administrator") == 1 && Count(table, "account:2") == 1,
                "SignInBudgetTester: one account's sign-ins never sign anybody else out.");
         Expect(Count(table, "account:1") == 50, "SignInBudgetTester: an account holds its cap, its newest sessions.");
      }

      // Twenty-one accounts at their cap fill the table; the two idle sessions stay.
      {
         std::vector<Session> table;
         unsigned __int64 now = 0;
         Add(table, "administrator", true, ++now, 50, 1000);
         Add(table, "account:2", false, ++now, 50, 1000);
         for (int round = 0; round < 50; round++)
         {
            for (int account = 100; account < 121; account++)
               Add(table, "account:" + std::to_string(account), false, ++now, 50, 1000);
         }
         Expect(table.size() == 1001, "SignInBudgetTester: the table is full, the administrator's beside it.");
         Expect(Count(table, "administrator") == 1 && Count(table, "account:2") == 1,
                "SignInBudgetTester: a full table takes from the accounts that filled it.");
      }

      // A thousand accounts of one domain with a session each, and then more: the
      // administrator, holding two and so the most, keeps both, and another
      // domain's idle accounts keep theirs. As the rule was first written the
      // administrator gave up one for each such sign-in until it held one, and
      // then the oldest of the tied went, which was the other domain's.
      {
         std::vector<Session> table;
         unsigned __int64 now = 0;
         Add(table, "administrator", true, ++now, 50, 1000);
         Add(table, "administrator", true, ++now, 50, 1000);
         Add(table, "account:2", "domain:2", false, ++now, 50, 1000);
         Add(table, "account:3", "domain:2", false, ++now, 50, 1000);
         for (int account = 100; account < 1300; account++)
            Add(table, "account:" + std::to_string(account), "domain:9", false, ++now, 50, 1000);
         Expect(Count(table, "administrator") == 2,
                "SignInBudgetTester: a table full of other people's single sessions takes none of the administrator's.");
         Expect(Count(table, "account:2") == 1 && Count(table, "account:3") == 1,
                "SignInBudgetTester: one domain's thousand accounts take from that domain, not from another's.");
      }
   }
}
