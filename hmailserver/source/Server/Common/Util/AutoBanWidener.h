// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <functional>
#include <vector>

namespace HM
{
   class IPAddress;

   // An auto-ban widened to its reverse-name neighbourhood (GitLab work item #55).
   //
   // An auto-ban is one address: an expiring IP range whose lower and upper are
   // the peer (AccountLogon::CreateIPRange). A scanner that works through
   // 203.0.113.10, .11, .12 under one reverse domain earns a ban, a firewall
   // rule and a sweep for each. With AutoBanWidenByReverseName on - it is OFF by
   // default, and nothing here happens until an administrator turns it on - the
   // ban is written exactly as before, at once, and then widened in the
   // background, off every session's thread, to the contiguous run of addresses
   // around it whose reverse names share its registrable domain:
   //
   //   203.0.113.14 -> 203.0.113.10-203.0.113.22 via example.net
   //
   // WHAT IS WALKED. The banned address must be IPv4: an IPv6 ban is never
   // widened, not even to its /64. Nor is the ban of an address of a network's
   // own (OwnNetwork: private, shared, loopback or link-local): its neighbours
   // are the network's own machines, whose reverse zone - an Active Directory
   // domain's, a home router's - names every one of them under one domain, so
   // one colleague's stale password would ban the office. Its reverse names (at most
   // NeverBanNameList::MaxReverseNames) must all be host names under ONE
   // registrable domain - the public suffix list the quarantine and DMARC use,
   // both sections, so mail5.outbound.example.net is example.net and a host
   // under a shared-hosting suffix of the list's private section is a domain of
   // its own (www.tenant.github.io is tenant.github.io), or no domain at all
   // where a wildcard rule makes its name a public suffix
   // (ec2-...compute-1.amazonaws.com), and never the provider's - and one of
   // them must resolve back to the address. That
   // last is not in the work item, and it is what keeps the widening from being
   // aimed at somebody else: the holder of an address writes its reverse name,
   // and most hosting companies let a customer write their own, so without it a
   // scanner named x.bigcorp.example in a /24 bigcorp shares would have
   // bigcorp's own hosts banned with it. Only the holder of bigcorp's forward
   // zone can make that name resolve to the scanner. A neighbour's reverse name
   // need not resolve back: whoever wrote it put that neighbour in the domain.
   //
   // From the address the walk goes down and then up, one address at a time,
   // and each direction stops at the first address that is not like it:
   //
   //   - the edge of the enclosing /24, which is never crossed;
   //   - MaxLookupsEachSide reverse lookups;
   //   - an address on AutoBanNeverBan (never looked up at all);
   //   - an address an IP range an administrator wrote tells apart from the
   //     banned one - a range that covers one of the two and not the other -
   //     or lets in at a priority not below the ban's 100 (never looked up
   //     either). Such a range is a statement about that address, which the
   //     ban's priority would otherwise override, or its firewall rule ignore;
   //   - an address with no reverse name, or one whose reverse names are not
   //     all host names under the same registrable domain;
   //   - an address one of whose reverse names the never-ban list by reverse
   //     name (NeverBanNameList.h) covers, forward-confirmed or not.
   //
   // Holes stay unbanned: the run is what was seen, contiguous. A reverse
   // lookup that is not answered anywhere in the walk, or a walk that takes
   // longer than DeadlineSeconds, leaves the single address as it was. So does
   // a banned address whose own reverse name the never-ban list by reverse name
   // covers: that list decides about it, and may still lift its ban.
   //
   // WHAT IS WRITTEN. The ban's own row, and only it: the newest unexpired
   // single-address "Auto-ban: " range for the address - a range an
   // administrator wrote is never widened - gets the run's lower and upper, by
   // an update that touches the row only while it is still that single address
   // (PersistentSecurityRange::WidenAutoBan). One row, not one per neighbour,
   // with the ban's own expiry; it is lifted by the same expiry sweep. The
   // firewall follows (AutoBanFirewall::Synchronise): one Windows Defender
   // Firewall rule for the span, and AutoBanCommand and the packaged Linux hook
   // told "ban" for each address of it, as they always were, one address at a
   // time. The application log says what happened, before and after:
   //
   //   Auto-ban: 203.0.113.14 was widened to 203.0.113.10-203.0.113.22 via example.net: ...
   //
   // A ban that stays a single address says why, in the same log:
   //
   //   Auto-ban: 203.0.113.14 stays a single address: the reverse lookup of 203.0.113.18 was not answered.
   //
   // Neither is the shape the packaged fail2ban filter matches, which goes on
   // counting the banned address's own lines.
   //
   // WHEN. Banned is called by AutoBanFirewall's three announcements, which run
   // only once the never-ban list by reverse name has not spared the address.
   // It queues the address and returns; the walks run one at a time on the name
   // lookup queue, at most MaxWaiting waiting, each address once.
   class AutoBanWidener
   {
   public:
      // An IPv4 address as a number, 203.0.113.14 being 0xCB00710E: what the IP
      // range table stores it as.
      typedef unsigned int Address4;

      // The lookups the walk asks, so that the tester can answer them. A false
      // return is a lookup that was not answered; true with nothing is an answer
      // that there is nothing.
      typedef std::function<bool(Address4 address, std::vector<String> &names)> ReverseLookup;
      typedef std::function<bool(const String &name, std::vector<String> &addresses)> ForwardLookup;

      // Why an address may not be taken into the run whatever its reverse name
      // says, or empty when it may: AutoBanNeverBan, an administrator's range.
      typedef std::function<String(Address4 address)> Barrier;

      // An IPv4 range an administrator wrote - every range but the expiring
      // "Auto-ban: " ones - as the walk reads it.
      struct AdministratorRange
      {
         Address4 lower = 0;
         Address4 upper = 0;
         long priority = 0;
         String name;
      };

      // Whether the never-ban list by reverse name covers a normalised name.
      typedef std::function<bool(const String &normalisedName)> NameSpared;

      // Whether the walk has run out of time.
      typedef std::function<bool()> Expired;

      struct Walk
      {
         // The run is wider than the banned address.
         bool widened = false;

         // A lookup was not answered, or the time ran out: the address stays
         // as it was, whatever had been seen.
         bool abandoned = false;

         // The run, the banned address alone when not widened.
         Address4 lower = 0;
         Address4 upper = 0;

         // The registrable domain the run shares, and the banned address's
         // reverse name that resolved back to it.
         String domain;
         String name;

         // Why the address stays as it was, when it does; and where each
         // direction of a walk stopped and why.
         String reason;
         String below;
         String above;

         // The reverse lookups made of neighbours.
         int lookups = 0;
      };

      // The walk, with no database and no network: every question is asked of
      // the arguments. barrier, spared and expired may be empty.
      static Walk Compute(Address4 seed, const ReverseLookup &reverse, const ForwardLookup &forward,
                          const Barrier &barrier, const NameSpared &spared, const Expired &expired);

      // The registrable domain of a normalised reverse name, or empty when the
      // name is not one the walk compares: not usable by the never-ban list's
      // rules (an address, a public suffix or shorter, a character no host name
      // has), or a reverse-mapping name under .arpa.
      static String DomainOf(const String &normalisedName);

      // Why an administrator's ranges keep the address out of the run of the ban
      // of seed, written at banPriority, or empty when none does: a range that
      // covers one of the two and not the other, or one covering the address at a
      // priority not below the ban's, which lets it in whatever the ban says. The
      // range's name is in the answer as printable text.
      static String RangeBarrier(Address4 seed, Address4 address, long banPriority, const std::vector<AdministratorRange> &ranges);

      // "203.0.113.14".
      static String Text(Address4 address);

      // Whether an address is one a ban of is ever widened - IPv4 - and its
      // number.
      static bool Walkable(const IPAddress &address, Address4 &value);

      // Whether an IPv4 address is a network's own rather than the Internet's,
      // so that a ban of it is never widened: 0.0.0.0/8, 10.0.0.0/8,
      // 100.64.0.0/10 (RFC 6598, the shared space behind a carrier's NAT),
      // 127.0.0.0/8, 169.254.0.0/16, 172.16.0.0/12 and 192.168.0.0/16 (RFC 1918
      // and RFC 6890). Not the documentation and benchmarking blocks, which no
      // network numbers its machines from.
      static bool OwnNetwork(Address4 address);

      // How the firewall mirrors an auto-ban's range (AutoBanFirewall::ActiveBans_):
      // a single address as itself; an IPv4 run inside one /24 as one rule
      // "lower-upper" and its addresses one by one for a hook. False for any
      // other shape - a range that crosses a /24, an IPv6 run, a range upside
      // down - which the auto-ban never writes and the firewall does not mirror.
      static bool MirrorShape(const IPAddress &lower, const IPAddress &upper, String &rule, std::vector<String> &members);

      // A ban has been announced for the address. Queues it and returns at once:
      // nothing is looked up and nothing is read from the database here, so it is
      // safe inside an SMTP command. Does nothing while AutoBanWidenByReverseName
      // is off, and nothing for an IPv6 address; for an address of a network's
      // own (OwnNetwork) it only says in the application log that the ban stays
      // a single address.
      static void Banned(const IPAddress &address);

      static constexpr int MaxLookupsEachSide = 32;
      static constexpr int DeadlineSeconds = 30;
      static constexpr size_t MaxWaiting = 64;

   private:

      friend class AutoBanWidenTask;

      static void Schedule_();
      static void Widen_(Address4 seed);
      static bool RealReverse_(Address4 address, std::vector<String> &names);
      static bool RealForward_(const String &name, std::vector<String> &addresses);
   };

   // AutoBanWidener held to what work item #55 asks of it, with no database, no
   // DNS and no setting: the run of a shared registrable domain (not one label
   // stripped), each direction's stops - a foreign domain, no name, an address
   // literal, a public suffix, a name the never-ban list covers, AutoBanNeverBan,
   // an administrator's range that tells the two apart or lets the neighbour in
   // at the ban's priority, the /24's edges, the bound on lookups - the
   // banned address's own name forward-confirmed, a lookup not answered or time
   // run out leaving the single address, a name its zone's holder filled with a
   // line break never reaching a log line, and the shapes the firewall mirrors.
   // Each assertion has its negative control beside it. Runs in ClassTester on
   // Windows and as autoban-widen-tests on Linux. Throws std::logic_error naming
   // the first thing that is wrong.
   class AutoBanWidenerTester
   {
   public:
      void Test();
   };
}
