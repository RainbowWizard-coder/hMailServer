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
      // An address as a holder: an IPv4 address carried in IPv6 as the IPv4, the
      // IPv6 loopback as it is, any other IPv6 address as its /64.
      std::string Holder(const boost::asio::ip::address &address)
      {
         if (address.is_v4())
            return address.to_v4().to_string();

         const boost::asio::ip::address_v6 v6 = address.to_v6();
         if (v6.is_v4_mapped())
            return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6).to_string();

         if (v6.is_loopback())
            return v6.to_string();

         boost::asio::ip::address_v6::bytes_type bytes = v6.to_bytes();
         for (size_t i = 8; i < bytes.size(); i++)
            bytes[i] = 0;

         return boost::asio::ip::address_v6(bytes).to_string() + "/64";
      }

      std::string Trimmed(const std::string &text)
      {
         const size_t first = text.find_first_not_of(" \t");
         if (first == std::string::npos)
            return "";
         const size_t last = text.find_last_not_of(" \t");
         return text.substr(first, last - first + 1);
      }
   }

   bool
   SignInBudget::IsLoopback(const IPAddress &peer)
   {
      if (peer.IsAny())
         return false;

      const boost::asio::ip::address address = peer.GetAddress();
      if (address.is_v4())
         return address.to_v4().is_loopback();

      const boost::asio::ip::address_v6 v6 = address.to_v6();
      if (v6.is_v4_mapped())
         return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6).is_loopback();

      return v6.is_loopback();
   }

   std::string
   SignInBudget::GuessSource(const IPAddress &peer)
   {
      if (peer.IsAny())
         return std::string(peer.ToString().c_str());

      return Holder(peer.GetAddress());
   }

   std::string
   SignInBudget::LastForwardedAddress(const AnsiString &request)
   {
      // The head alone, as RestApiServer::GetHeader_ reads it: a line in the body
      // is whatever the sender wrote and was never a header.
      const std::string text(request.c_str(), (size_t) request.GetLength());
      const size_t headEnd = text.find("\r\n\r\n");
      const std::string head = headEnd == std::string::npos ? text : text.substr(0, headEnd + 2);

      std::string lower = head;
      for (size_t i = 0; i < lower.size(); i++)
      {
         if (lower[i] >= 'A' && lower[i] <= 'Z')
            lower[i] = (char) (lower[i] - 'A' + 'a');
      }

      // The LAST line of that name: a proxy that adds its own line rather than
      // extending the one the client sent (HAProxy's option forwardfor does)
      // writes it after the client's.
      const std::string needle = "\r\nx-forwarded-for:";
      const size_t found = lower.rfind(needle);
      if (found == std::string::npos)
         return "";

      const size_t valueStart = found + needle.size();
      const size_t lineEnd = head.find("\r\n", valueStart);
      const std::string value = head.substr(valueStart, lineEnd == std::string::npos ? std::string::npos : lineEnd - valueStart);

      // Its last element: the address the nearest proxy wrote.
      const size_t comma = value.rfind(',');
      std::string element = Trimmed(comma == std::string::npos ? value : value.substr(comma + 1));

      if (!element.empty() && element[0] == '[')
      {
         // [2001:db8::1] or [2001:db8::1]:443.
         const size_t close = element.find(']');
         if (close == std::string::npos)
            return "";
         element = element.substr(1, close - 1);
      }
      else if (element.find(':') != std::string::npos && element.find(':') == element.rfind(':'))
      {
         // 192.0.2.7:51234 - one colon is an IPv4 address and a port, never IPv6.
         element = element.substr(0, element.find(':'));
      }

      if (element.empty() || element.size() > 64)
         return "";

      boost::system::error_code error;
      const boost::asio::ip::address address = boost::asio::ip::make_address(element, error);
      if (error)
         return "";

      // An address on this machine, or none, says nothing about the browser: a
      // second proxy on this host writes its own 127.0.0.1 here for every one of
      // them. No source of its own, as no address at all.
      const IPAddress forwarded(address);
      if (IsLoopback(forwarded) || address.is_unspecified() ||
          (address.is_v6() && address.to_v6().is_v4_mapped() &&
           boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, address.to_v6()).is_unspecified()))
         return "";

      return Holder(address);
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

   std::string
   SignInBudget::PendingSource(const AnsiString &request, const IPAddress &peer)
   {
      if (IsLoopback(peer))
         return LastForwardedAddress(request);

      return GuessSource(peer);
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

      AnsiString Head(const std::string &headerLines)
      {
         const std::string request = "GET /portal/oidc/start HTTP/1.1\r\nHost: mail.example.test\r\n" + headerLines + "\r\n";
         return AnsiString(request.c_str());
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

      // Adds one entry of holder to a map table under the rule, as OidcSignIn,
      // SamlSignIn and PasskeyStore do: the group is the source's.
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

      Expect(SignInBudget::IsLoopback(Peer("127.0.0.1")) && SignInBudget::IsLoopback(Peer("127.0.0.2")) &&
             SignInBudget::IsLoopback(Peer("::1")) && SignInBudget::IsLoopback(Peer("::ffff:127.0.0.1")),
             "SignInBudgetTester: 127.0.0.0/8, ::1 and a mapped 127.x are this machine.");
      Expect(!SignInBudget::IsLoopback(Peer("192.0.2.1")) && !SignInBudget::IsLoopback(Peer("2001:db8::1")) &&
             !SignInBudget::IsLoopback(IPAddress()),
             "SignInBudgetTester: another address, and no address at all, is not.");

      // A pending sign-in's source: the header is believed from this machine only,
      // and only its last address.
      Expect(SignInBudget::PendingSource(Head("X-Forwarded-For: 198.51.100.4\r\n"), Peer("192.0.2.10")) == "192.0.2.10",
             "SignInBudgetTester: X-Forwarded-For from a peer that is not on this machine is not believed.");
      Expect(SignInBudget::PendingSource(Head("X-Forwarded-For: 198.51.100.1, 203.0.113.5\r\n"), Peer("127.0.0.1")) == "203.0.113.5",
             "SignInBudgetTester: behind a proxy on this machine the source is the address the proxy wrote, the last.");
      Expect(SignInBudget::PendingSource(Head("x-forwarded-for: 203.0.113.5\r\nX-Forwarded-For: 198.51.100.9\r\n"), Peer("::1")) == "198.51.100.9",
             "SignInBudgetTester: of two lines, the last line's.");
      Expect(SignInBudget::PendingSource(Head("X-Forwarded-For: 2001:db8:9:9::42\r\n"), Peer("127.0.0.1")) == "2001:db8:9:9::/64",
             "SignInBudgetTester: a forwarded IPv6 address is its /64.");
      Expect(SignInBudget::PendingSource(Head("X-Forwarded-For: [2001:db8:9:9::42]:443\r\n"), Peer("127.0.0.1")) == "2001:db8:9:9::/64",
             "SignInBudgetTester: a bracketed IPv6 address with a port is read.");
      Expect(SignInBudget::PendingSource(Head("X-Forwarded-For: 203.0.113.5:51234\r\n"), Peer("127.0.0.1")) == "203.0.113.5",
             "SignInBudgetTester: an IPv4 address with a port is read.");
      Expect(SignInBudget::PendingSource(Head(""), Peer("127.0.0.1")).empty(),
             "SignInBudgetTester: a loopback request with no forwarded address has no source of its own.");
      Expect(SignInBudget::PendingSource(Head("X-Forwarded-For: 203.0.113.5, unknown\r\n"), Peer("127.0.0.1")).empty() &&
             SignInBudget::PendingSource(Head("X-Forwarded-For: _hidden\r\n"), Peer("127.0.0.1")).empty() &&
             SignInBudget::PendingSource(Head("X-Forwarded-For: 203.0.113.5.example\r\n"), Peer("127.0.0.1")).empty(),
             "SignInBudgetTester: a last element that is not an address is no source, never the element before it.");

      // A forwarded address on this machine, or none, is every browser behind a
      // second proxy on this host: no source of its own, not one capped source
      // that one asker could fill for all of them.
      Expect(SignInBudget::PendingSource(Head("X-Forwarded-For: 203.0.113.5, 127.0.0.1\r\n"), Peer("127.0.0.1")).empty() &&
             SignInBudget::PendingSource(Head("X-Forwarded-For: 127.0.0.2\r\n"), Peer("127.0.0.1")).empty() &&
             SignInBudget::PendingSource(Head("X-Forwarded-For: ::1\r\n"), Peer("::1")).empty() &&
             SignInBudget::PendingSource(Head("X-Forwarded-For: [::1]:8080\r\n"), Peer("127.0.0.1")).empty() &&
             SignInBudget::PendingSource(Head("X-Forwarded-For: ::ffff:127.0.0.1\r\n"), Peer("127.0.0.1")).empty() &&
             SignInBudget::PendingSource(Head("X-Forwarded-For: 0.0.0.0\r\n"), Peer("127.0.0.1")).empty() &&
             SignInBudget::PendingSource(Head("X-Forwarded-For: ::\r\n"), Peer("127.0.0.1")).empty() &&
             SignInBudget::PendingSource(Head("X-Forwarded-For: ::ffff:0.0.0.0\r\n"), Peer("127.0.0.1")).empty(),
             "SignInBudgetTester: a forwarded loopback or unspecified address is no source of its own.");

      // A header in the body is not a header.
      const AnsiString bodyOnly = Head("Content-Length: 30\r\n") + AnsiString("\r\nX-Forwarded-For: 203.0.113.77");
      Expect(SignInBudget::PendingSource(bodyOnly, Peer("127.0.0.1")).empty(),
             "SignInBudgetTester: an X-Forwarded-For line in the body is not read.");

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
