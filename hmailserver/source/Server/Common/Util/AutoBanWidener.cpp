// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See AutoBanWidener.h.

#include "StdAfx.h"

#include "AutoBanWidener.h"

#include "AutoBanFirewall.h"
#include "NeverBanNameList.h"
#include "ServerStatus.h"
#include "VariantDateTime.h"
#include "../AntiSpam/DMARC/PublicSuffixList.h"
#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../BO/SecurityRange.h"
#include "../BO/SecurityRanges.h"
#include "../Persistence/PersistentSecurityRange.h"
#include "../TCPIP/DNSResolver.h"
#include "../TCPIP/IPAddress.h"
#include "../Threading/Task.h"
#include "../Threading/WorkQueue.h"
#include "Parsing/StringParser.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The addresses waiting for a walk, oldest first, and whether a walk is
      // queued or running. One walk at a time: a walk is up to sixty-five reverse
      // lookups made one after another, and a flood of bans must cost the
      // resolver a queue, not a burst.
      std::mutex waiting_mutex;
      std::deque<AutoBanWidener::Address4> waiting;
      bool walking = false;

      // When the walk now queued or running was handed to the queue or began. A
      // walk takes DeadlineSeconds and a lookup at most; one "walking" for longer
      // than this was lost - its queue removed with its task still waiting in it,
      // as stopping the servers does - and the next ban queues another, rather
      // than every ban waiting for ever behind a walk that will never run.
      std::chrono::steady_clock::time_point walking_since;
      const int LostAfterSeconds = 180;

      const wchar_t *RangeNamePrefix = L"Auto-ban: ";

      // A reverse name fit for a log line: printable ASCII and no quote, at most
      // 255 characters. The reverse zone's holder chooses these bytes.
      String Printable(const String &name)
      {
         String out;
         const int length = name.GetLength() > 255 ? 255 : name.GetLength();

         for (int i = 0; i < length; i++)
         {
            const wchar_t c = name.GetAt(i);
            out += (c < 0x21 || c > 0x7E || c == L'"' || c == L'\\') ? L'?' : c;
         }

         return out;
      }

      // An administrator's range name, fit for a log line: its spaces kept, and
      // no control character, quote or backslash - the log's fields are quoted.
      String PrintableText(const String &text)
      {
         String out;
         const int length = text.GetLength() > 100 ? 100 : text.GetLength();

         for (int i = 0; i < length; i++)
         {
            const wchar_t c = text.GetAt(i);
            out += (c < 0x20 || c == 0x7F || c == L'"' || c == L'\\') ? L'?' : c;
         }

         return out;
      }

      // Whether every reverse name of a neighbour puts it in the run's domain,
      // and when not, why, as the end of a sentence that begins with the
      // neighbour's address.
      bool InDomain(const std::vector<String> &names, const String &domain, const AutoBanWidener::NameSpared &spared, String &why)
      {
         if (names.empty())
         {
            why = _T("has no reverse name");
            return false;
         }

         if ((int) names.size() > NeverBanNameList::MaxReverseNames)
         {
            why = _T("has more reverse names than are looked at");
            return false;
         }

         for (const String &raw : names)
         {
            const String name = NeverBanNameList::NormaliseName(raw);
            const String nameDomain = AutoBanWidener::DomainOf(name);

            if (nameDomain.IsEmpty())
            {
               why = _T("is named ") + Printable(name) + _T(", which is an address, a public suffix or not a host name");
               return false;
            }

            if (nameDomain != domain)
            {
               why = _T("is named under ") + nameDomain;
               return false;
            }

            if (spared && spared(name))
            {
               why = _T("is named ") + name + _T(", which the never-ban list by reverse name covers");
               return false;
            }
         }

         return true;
      }
   }

   // One walk, on the name lookup queue; it queues the next one when it is done.
   class AutoBanWidenTask : public Task
   {
   public:
      AutoBanWidenTask() : Task("AutoBanWidener::Widen") { }

      virtual void DoWork()
      {
         AutoBanWidener::Address4 seed = 0;

         {
            std::lock_guard<std::mutex> guard(waiting_mutex);

            if (waiting.empty())
            {
               walking = false;
               return;
            }

            seed = waiting.front();
            waiting.pop_front();
            walking_since = std::chrono::steady_clock::now();
         }

         try
         {
            AutoBanWidener::Widen_(seed);
         }
         catch (...)
         {
            // The ban is in the table as it was written, a single address, and
            // expires on its own; nothing below the server was told anything new.
            LOG_DEBUG(_T("AutoBanWidener - the walk around ") + AutoBanWidener::Text(seed) +
                      _T(" stopped on an exception; its ban stays a single address."));
         }

         {
            std::lock_guard<std::mutex> guard(waiting_mutex);

            if (waiting.empty())
            {
               walking = false;
               return;
            }
         }

         AutoBanWidener::Schedule_();
      }
   };

   // ---- the queue -----------------------------------------------------------------

   void
   AutoBanWidener::Banned(const IPAddress &address)
   {
      Address4 seed = 0;
      if (!Walkable(address, seed))
         return;

      if (!Configuration::Instance()->GetAutoBanWidenByReverseName())
         return;

      // Never walked, never looked up: the neighbours of a network's own address
      // are its own machines. Said where the widening is, so that an
      // administrator who turned it on sees why this ban did not widen.
      if (OwnNetwork(seed))
      {
         LOG_APPLICATION(_T("Auto-ban: ") + Text(seed) + _T(" stays a single address: it is a private, shared, loopback or link-local address, whose neighbours are this network's own machines."));
         return;
      }

      {
         std::lock_guard<std::mutex> guard(waiting_mutex);

         if (std::find(waiting.begin(), waiting.end(), seed) != waiting.end())
            return;

         if (waiting.size() >= MaxWaiting)
         {
            LOG_DEBUG(_T("AutoBanWidener - ") + StringParser::IntToString((int) MaxWaiting) +
                      _T(" bans are waiting to be widened already; the ban of ") + Text(seed) + _T(" stays a single address."));
            return;
         }

         waiting.push_back(seed);

         const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

         if (walking && now - walking_since < std::chrono::seconds(LostAfterSeconds))
            return;

         walking = true;
         walking_since = now;
      }

      Schedule_();
   }

   void
   AutoBanWidener::Schedule_()
   {
      // The name lookup queue exists while the servers run; asked for at any
      // other moment, the application reports its absence as an error, which a
      // ban that stays a single address is not.
      const int state = ServerStatus::Instance()->GetState();
      std::shared_ptr<WorkQueue> queue;

      if (state == ServerStatus::StateRunning || state == ServerStatus::StateDraining)
         queue = Application::Instance()->GetNameLookupWorkQueue();

      if (!queue)
      {
         // Starting or stopping: the bans stay single addresses, as they were
         // before this feature, and expire on their own.
         std::lock_guard<std::mutex> guard(waiting_mutex);
         waiting.clear();
         walking = false;
         return;
      }

      queue->AddTask(std::shared_ptr<Task>(new AutoBanWidenTask()), WorkQueue::TaskMayBlock);
   }

   // ---- one walk, against the database and the DNS ------------------------------------

   bool
   AutoBanWidener::RealReverse_(Address4 address, std::vector<String> &names)
   {
      DNSResolver resolver;
      return resolver.GetPTRRecords(Text(address), names);
   }

   bool
   AutoBanWidener::RealForward_(const String &name, std::vector<String> &addresses)
   {
      // The resource-record number of RFC 1035, as NeverBanNameList asks it.
      const int ResourceTypeA = 0x0001;

      DNSResolver resolver;
      std::vector<AnsiString> values;

      if (!resolver.GetRecordsOfType(AnsiString(name), ResourceTypeA, values))
         return false;

      for (const AnsiString &value : values)
         addresses.push_back(String(value));

      return true;
   }

   void
   AutoBanWidener::Widen_(Address4 seed)
   {
      // Turned off since the ban was queued: nothing is walked.
      if (!Configuration::Instance()->GetAutoBanWidenByReverseName())
         return;

      const String seedText = Text(seed);

      SecurityRanges ranges;
      if (!ranges.Refresh())
      {
         LOG_DEBUG(_T("AutoBanWidener - the IP ranges could not be read; the ban of ") + seedText + _T(" stays a single address."));
         return;
      }

      // The ban's own row - the newest unexpired single-address "Auto-ban: "
      // range for the address - and the IPv4 ranges an administrator wrote,
      // which the walk may not cross.
      std::shared_ptr<SecurityRange> row;
      std::vector<AdministratorRange> written;
      const DateTime now = DateTime::GetCurrentTime();

      for (std::shared_ptr<SecurityRange> range : ranges.GetSnapshot())
      {
         if (!range)
            continue;

         const IPAddress lower = range->GetLowerIP();
         const IPAddress upper = range->GetUpperIP();

         if (lower.GetType() != IPAddress::IPV4 || upper.GetType() != IPAddress::IPV4)
            continue;

         const bool ours = range->GetExpires() && range->GetName().StartsWith(RangeNamePrefix);

         if (!ours)
         {
            AdministratorRange administrators;
            administrators.lower = (Address4) lower.GetAddress1();
            administrators.upper = (Address4) upper.GetAddress1();
            administrators.priority = range->GetPriority();
            administrators.name = range->GetName();
            written.push_back(administrators);
            continue;
         }

         if ((Address4) lower.GetAddress1() != seed || (Address4) upper.GetAddress1() != seed)
            continue;

         if (!(now < range->GetExpiresTime()))
            continue;

         if (!row || range->GetID() > row->GetID())
            row = range;
      }

      if (!row)
      {
         LOG_DEBUG(_T("AutoBanWidener - ") + seedText + _T(" has no single-address auto-ban to widen: it was lifted, deleted or has expired."));
         return;
      }

      const long banPriority = row->GetPriority();

      Barrier barrier = [seed, banPriority, &written](Address4 address) -> String
      {
         if (AutoBanFirewall::IsNeverBanned(IPAddress((__int64) address)))
            return _T("is on AutoBanNeverBan");

         return RangeBarrier(seed, address, banPriority, written);
      };

      const std::vector<String> patterns = NeverBanNameList::Patterns();
      NameSpared spared = [&patterns](const String &name) -> bool
      {
         return !NeverBanNameList::CoveringPattern(name, patterns).IsEmpty();
      };

      const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(DeadlineSeconds);
      Expired expired = [deadline]() -> bool
      {
         return std::chrono::steady_clock::now() > deadline;
      };

      const Walk walk = Compute(seed, &AutoBanWidener::RealReverse_, &AutoBanWidener::RealForward_, barrier, spared, expired);

      if (!walk.widened)
      {
         // In the application log, as the widening is: an administrator who turned
         // this on is owed the reason a ban did not widen as much as the one it
         // did. Not the shape the packaged fail2ban filter matches either, and
         // every name in the reason is printable text.
         LOG_APPLICATION(_T("Auto-ban: ") + seedText + _T(" stays a single address: ") + walk.reason + _T("."));
         return;
      }

      // Turned off while it was walked: the answer is not acted on.
      if (!Configuration::Instance()->GetAutoBanWidenByReverseName())
         return;

      const IPAddress lower((__int64) walk.lower);
      const IPAddress upper((__int64) walk.upper);

      if (!PersistentSecurityRange::WidenAutoBan(row->GetID(), IPAddress((__int64) seed), lower, upper))
      {
         LOG_DEBUG(_T("AutoBanWidener - the ban of ") + seedText + _T(" could not be widened in the database; it stays a single address."));
         return;
      }

      // The update touches the row only while it is still the single address,
      // so what it now holds says whether it did.
      std::shared_ptr<SecurityRange> after(new SecurityRange());
      if (!PersistentSecurityRange::ReadObject(after, row->GetID()) ||
          (Address4) after->GetLowerIP().GetAddress1() != walk.lower ||
          (Address4) after->GetUpperIP().GetAddress1() != walk.upper)
      {
         LOG_DEBUG(_T("AutoBanWidener - the ban of ") + seedText + _T(" was lifted, deleted or changed while its neighbours were looked up; it is left as it is."));
         return;
      }

      const int count = (int) (walk.upper - walk.lower) + 1;

      // Before and after, the domain, and where each direction stopped. Not the
      // shape the packaged fail2ban filter matches.
      LOG_APPLICATION(_T("Auto-ban: ") + seedText + _T(" was widened to ") + Text(walk.lower) + _T("-") + Text(walk.upper) +
                      _T(" via ") + walk.domain + _T(": the ") + StringParser::IntToString(count) +
                      _T(" addresses of the run have reverse names under that domain, and its own, ") + walk.name +
                      _T(", resolves back to it; the run stopped below because ") + walk.below +
                      _T(" and above because ") + walk.above + _T("."));

      AutoBanFirewall::Synchronise(_T("ban widened"));
   }

   // ---- the walk -----------------------------------------------------------------------

   AutoBanWidener::Walk
   AutoBanWidener::Compute(Address4 seed, const ReverseLookup &reverse, const ForwardLookup &forward,
                           const Barrier &barrier, const NameSpared &spared, const Expired &expired)
   {
      Walk walk;
      walk.lower = seed;
      walk.upper = seed;

      const IPAddress seedAddress((__int64) seed);

      if (!reverse || !forward)
      {
         walk.abandoned = true;
         walk.reason = _T("there is nothing to look its neighbours up with");
         return walk;
      }

      if (expired && expired())
      {
         walk.abandoned = true;
         walk.reason = _T("the walk ran out of time");
         return walk;
      }

      // ---- the banned address's own reverse names: one domain, and one name
      // that resolves back to it.
      std::vector<String> names;
      if (!reverse(seed, names))
      {
         walk.abandoned = true;
         walk.reason = _T("its reverse lookup was not answered");
         return walk;
      }

      if (names.empty())
      {
         walk.reason = _T("it has no reverse name");
         return walk;
      }

      if ((int) names.size() > NeverBanNameList::MaxReverseNames)
      {
         walk.reason = _T("it has more reverse names than are looked at");
         return walk;
      }

      String domain;
      std::vector<String> usable;

      for (const String &raw : names)
      {
         const String name = NeverBanNameList::NormaliseName(raw);
         const String nameDomain = DomainOf(name);

         if (nameDomain.IsEmpty())
         {
            walk.reason = _T("its reverse name ") + Printable(name) + _T(" is an address, a public suffix or not a host name");
            return walk;
         }

         if (domain.IsEmpty())
         {
            domain = nameDomain;
         }
         else if (nameDomain != domain)
         {
            walk.reason = _T("its reverse names are under more than one domain, ") + domain + _T(" and ") + nameDomain;
            return walk;
         }

         if (spared && spared(name))
         {
            walk.reason = _T("its reverse name ") + name + _T(" is covered by the never-ban list by reverse name, which decides about it");
            return walk;
         }

         usable.push_back(name);
      }

      bool unanswered = false;
      int asked = 0;

      for (const String &name : usable)
      {
         if (asked >= NeverBanNameList::MaxForwardLookups)
            break;

         if (expired && expired())
         {
            walk.abandoned = true;
            walk.reason = _T("the walk ran out of time");
            return walk;
         }

         asked++;

         std::vector<String> addresses;
         if (!forward(name, addresses))
         {
            unanswered = true;
            continue;
         }

         for (const String &answer : addresses)
         {
            if (NeverBanNameList::SameAddress(seedAddress, answer))
            {
               walk.name = name;
               break;
            }
         }

         if (!walk.name.IsEmpty())
            break;
      }

      if (walk.name.IsEmpty())
      {
         walk.abandoned = unanswered;
         walk.reason = unanswered
            ? String(_T("the forward lookup of its reverse name was not answered"))
            : _T("its reverse name ") + usable.front() + _T(" does not resolve back to it, so anybody could have written it");
         return walk;
      }

      walk.domain = domain;

      // ---- each direction, one address at a time, inside the /24.
      const Address4 net = seed & 0xFFFFFF00u;
      bool abandoned = false;
      String abandonedWhy;

      auto side = [&](bool down, String &why) -> Address4
      {
         const Address4 edge = down ? net : (net | 0xFFu);
         Address4 at = seed;
         int lookups = 0;

         for (;;)
         {
            if (at == edge)
            {
               why = _T("the /24 ends at ") + Text(at);
               return at;
            }

            if (lookups >= MaxLookupsEachSide)
            {
               why = _T("the walk looks at no more than ") + StringParser::IntToString(MaxLookupsEachSide) + _T(" addresses each way");
               return at;
            }

            const Address4 next = down ? at - 1 : at + 1;

            const String barred = barrier ? barrier(next) : String();
            if (!barred.IsEmpty())
            {
               why = Text(next) + _T(" ") + barred;
               return at;
            }

            if (expired && expired())
            {
               abandoned = true;
               abandonedWhy = _T("the walk ran out of time");
               return at;
            }

            lookups++;
            walk.lookups++;

            std::vector<String> neighbour;
            if (!reverse(next, neighbour))
            {
               abandoned = true;
               abandonedWhy = _T("the reverse lookup of ") + Text(next) + _T(" was not answered");
               return at;
            }

            String mismatch;
            if (!InDomain(neighbour, domain, spared, mismatch))
            {
               why = Text(next) + _T(" ") + mismatch;
               return at;
            }

            at = next;
         }
      };

      const Address4 lower = side(true, walk.below);
      const Address4 upper = abandoned ? seed : side(false, walk.above);

      if (abandoned)
      {
         walk.abandoned = true;
         walk.reason = abandonedWhy;
         return walk;
      }

      walk.lower = lower;
      walk.upper = upper;
      walk.widened = lower != upper;

      if (!walk.widened)
      {
         walk.reason = _T("no neighbour joins it under ") + domain + _T(": below, ") + walk.below +
                       _T("; above, ") + walk.above;
      }

      return walk;
   }

   // ---- the rules ------------------------------------------------------------------------

   String
   AutoBanWidener::RangeBarrier(Address4 seed, Address4 address, long banPriority, const std::vector<AdministratorRange> &ranges)
   {
      for (const AdministratorRange &range : ranges)
      {
         const bool coversSeed = seed >= range.lower && seed <= range.upper;
         const bool coversAddress = address >= range.lower && address <= range.upper;

         // A statement about one of the two and not the other.
         if (coversSeed != coversAddress)
            return _T("is told apart from it by the IP range '") + PrintableText(range.name) + _T("'");

         // A range that would win over the ban for this address - the server
         // reads the highest priority, and a tie either way - is the
         // administrator letting it in. The server's own refusal would not reach
         // it, and a firewall rule for the run must not either.
         if (coversAddress && range.priority >= banPriority)
            return _T("is let in by the IP range '") + PrintableText(range.name) + _T("', whose priority is not below the ban's");
      }

      return String();
   }

   String
   AutoBanWidener::DomainOf(const String &normalisedName)
   {
      if (!NeverBanNameList::IsUsableReverseName(normalisedName))
         return String();

      // A reverse-mapping name is never a host's: an RFC 2317 delegation
      // answers with one, 14.0/26.113.0.203.in-addr.arpa, and all of them
      // would otherwise share the domain 203.in-addr.arpa.
      if (normalisedName == _T("arpa") || normalisedName.EndsWith(_T(".arpa")))
         return String();

      String host = normalisedName;
      bool isAddress = false;

      if (!PublicSuffixList::GetRegistrableDomain(host, isAddress) || isAddress)
         return String();

      return host;
   }

   String
   AutoBanWidener::Text(Address4 address)
   {
      String text;
      text.Format(_T("%u.%u.%u.%u"), (address >> 24) & 0xFFu, (address >> 16) & 0xFFu, (address >> 8) & 0xFFu, address & 0xFFu);
      return text;
   }

   bool
   AutoBanWidener::Walkable(const IPAddress &address, Address4 &value)
   {
      if (address.GetType() != IPAddress::IPV4)
         return false;

      value = (Address4) address.GetAddress1();
      return true;
   }

   bool
   AutoBanWidener::OwnNetwork(Address4 address)
   {
      struct Block
      {
         Address4 network;
         Address4 mask;
      };

      static const Block blocks[] =
      {
         { 0x00000000u, 0xFF000000u },   // 0.0.0.0/8
         { 0x0A000000u, 0xFF000000u },   // 10.0.0.0/8
         { 0x64400000u, 0xFFC00000u },   // 100.64.0.0/10
         { 0x7F000000u, 0xFF000000u },   // 127.0.0.0/8
         { 0xA9FE0000u, 0xFFFF0000u },   // 169.254.0.0/16
         { 0xAC100000u, 0xFFF00000u },   // 172.16.0.0/12
         { 0xC0A80000u, 0xFFFF0000u },   // 192.168.0.0/16
      };

      for (const Block &block : blocks)
      {
         if ((address & block.mask) == block.network)
            return true;
      }

      return false;
   }

   bool
   AutoBanWidener::MirrorShape(const IPAddress &lower, const IPAddress &upper, String &rule, std::vector<String> &members)
   {
      rule.Empty();
      members.clear();

      if (lower.GetType() != upper.GetType())
         return false;

      if (lower.GetType() == IPAddress::IPV6)
      {
         if (lower.GetAddress1() != upper.GetAddress1() || lower.GetAddress2() != upper.GetAddress2())
            return false;

         rule = String(lower.ToString());
         members.push_back(rule);
         return true;
      }

      const Address4 low = (Address4) lower.GetAddress1();
      const Address4 high = (Address4) upper.GetAddress1();

      if (low == high)
      {
         rule = Text(low);
         members.push_back(rule);
         return true;
      }

      if (low > high || (low & 0xFFFFFF00u) != (high & 0xFFFFFF00u))
         return false;

      rule = Text(low) + _T("-") + Text(high);

      for (Address4 address = low; ; address++)
      {
         members.push_back(Text(address));

         if (address == high)
            break;
      }

      return true;
   }

   // ---- the tester -----------------------------------------------------------------------

   namespace
   {
      void Expect(bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(what);
      }

      AutoBanWidener::Address4 Four(const char *text)
      {
         IPAddress address;
         AutoBanWidener::Address4 value = 0;
         if (!address.TryParse(AnsiString(text), false) || !AutoBanWidener::Walkable(address, value))
            throw std::logic_error("AutoBanWidenerTester: a test address did not parse as IPv4.");
         return value;
      }

      // A zone of the tester's own: reverse names by address, the addresses that
      // do not answer, A records by name, and every address it was asked about.
      struct WalkZone
      {
         std::map<AutoBanWidener::Address4, std::vector<String> > ptr;
         std::set<AutoBanWidener::Address4> silent;
         std::map<String, std::vector<String> > a;
         std::set<String> silentNames;
         std::vector<AutoBanWidener::Address4> asked;
         int forwardAsked = 0;

         void Name(const char *address, const String &name)
         {
            ptr[Four(address)].push_back(name);
         }

         // Every address from first to last, named prefix<last octet>.domain.
         void Run(const char *first, const char *last, const String &prefix, const String &domain)
         {
            for (AutoBanWidener::Address4 address = Four(first); ; address++)
            {
               ptr[address].push_back(prefix + StringParser::IntToString((int) (address & 0xFF)) + _T(".") + domain);
               if (address == Four(last))
                  break;
            }
         }

         AutoBanWidener::ReverseLookup Reverse()
         {
            return [this](AutoBanWidener::Address4 address, std::vector<String> &names) -> bool
            {
               asked.push_back(address);

               if (silent.count(address) > 0)
                  return false;

               auto it = ptr.find(address);
               if (it != ptr.end())
                  names = it->second;
               return true;
            };
         }

         AutoBanWidener::ForwardLookup Forward()
         {
            return [this](const String &name, std::vector<String> &addresses) -> bool
            {
               forwardAsked++;

               if (silentNames.count(name) > 0)
                  return false;

               auto it = a.find(name);
               if (it != a.end())
                  addresses = it->second;
               return true;
            };
         }

         bool WasAsked(const char *address) const
         {
            return std::find(asked.begin(), asked.end(), Four(address)) != asked.end();
         }
      };

      AutoBanWidener::Walk WalkFrom(WalkZone &zone, const char *seed,
                                const AutoBanWidener::Barrier &barrier = AutoBanWidener::Barrier(),
                                const AutoBanWidener::NameSpared &spared = AutoBanWidener::NameSpared(),
                                const AutoBanWidener::Expired &expired = AutoBanWidener::Expired())
      {
         zone.asked.clear();
         zone.forwardAsked = 0;
         return AutoBanWidener::Compute(Four(seed), zone.Reverse(), zone.Forward(), barrier, spared, expired);
      }

      bool Span(const AutoBanWidener::Walk &walk, const char *lower, const char *upper)
      {
         return walk.lower == Four(lower) && walk.upper == Four(upper);
      }

      bool Single(const AutoBanWidener::Walk &walk, const char *seed)
      {
         return !walk.widened && walk.lower == Four(seed) && walk.upper == Four(seed);
      }

      bool Clean(const String &text)
      {
         return text.Find(_T("\r")) < 0 && text.Find(_T("\n")) < 0 && text.Find(_T("\"")) < 0;
      }
   }

   void
   AutoBanWidenerTester::Test()
   {
      // ---- the registrable domain, not one label stripped
      Expect(AutoBanWidener::DomainOf(_T("mail5.outbound.example.net")) == _T("example.net"), "mail5.outbound.example.net is not under example.net.");
      Expect(AutoBanWidener::DomainOf(_T("outbound.example.net")) == _T("example.net"), "A name one label below the domain is not under it.");
      Expect(AutoBanWidener::DomainOf(_T("host.example.co.uk")) == _T("example.co.uk"), "A name under co.uk is not under its registrable domain.");
      // The list's private section counts, as DMARC counts it, so no cloud's
      // customers share the provider's domain. Under the wildcard rule
      // *.compute-1.amazonaws.com a host's name is a public suffix itself and no
      // domain at all; under *.compute.amazonaws.com, and under github.io, each
      // customer is a domain of its own.
      Expect(AutoBanWidener::DomainOf(_T("ec2-203-0-113-14.compute-1.amazonaws.com")).IsEmpty(),
             "A host a wildcard rule of the list's private section makes a public suffix was given a domain - every customer would share the provider's.");
      Expect(AutoBanWidener::DomainOf(_T("ec2-203-0-113-14.eu-west-1.compute.amazonaws.com")) == _T("ec2-203-0-113-14.eu-west-1.compute.amazonaws.com") &&
             AutoBanWidener::DomainOf(_T("ec2-203-0-113-15.eu-west-1.compute.amazonaws.com")) == _T("ec2-203-0-113-15.eu-west-1.compute.amazonaws.com"),
             "Two customers' hosts under a wildcard rule of the list's private section are not each a domain of their own.");
      Expect(AutoBanWidener::DomainOf(_T("www.tenant.github.io")) == _T("tenant.github.io") &&
             AutoBanWidener::DomainOf(_T("mail.other.github.io")) == _T("other.github.io"),
             "Two tenants of a shared-hosting suffix of the list's private section share a domain.");
      {
         // The negative control: the ICANN section alone gives a tenant the
         // provider's domain, so the cases above are the private section's doing.
         String host = _T("www.tenant.github.io");
         bool isAddress = false;
         Expect(PublicSuffixList::GetRegistrableDomain(host, isAddress, PublicSuffixList::IcannSectionOnly) && host == _T("github.io"),
                "The ICANN section alone did not give a github.io tenant the provider's domain - the private-section cases above prove nothing.");
      }
      Expect(AutoBanWidener::DomainOf(_T("co.uk")).IsEmpty(), "A public suffix has a domain.");
      Expect(AutoBanWidener::DomainOf(_T("localhost")).IsEmpty(), "A single label has a domain.");
      Expect(AutoBanWidener::DomainOf(_T("203.0.113.14")).IsEmpty(), "An address written as a reverse name has a domain.");
      Expect(AutoBanWidener::DomainOf(_T("14.0/26.113.0.203.in-addr.arpa")).IsEmpty(), "A reverse-mapping name has a domain.");
      Expect(AutoBanWidener::DomainOf(_T("x\r\nevil.example.net")).IsEmpty(), "A name with a line break has a domain.");
      Expect(AutoBanWidener::DomainOf(_T("")).IsEmpty(), "The empty name has a domain.");

      Expect(AutoBanWidener::Text(Four("203.0.113.14")) == _T("203.0.113.14"), "An address did not print as itself.");
      Expect(AutoBanWidener::Text(0xFFFFFFFFu) == _T("255.255.255.255"), "The highest address did not print as itself.");
      Expect(AutoBanWidener::Text(0) == _T("0.0.0.0"), "The lowest address did not print as itself.");

      // ---- IPv6 is never walked
      {
         IPAddress v6;
         AutoBanWidener::Address4 value = 0;
         Expect(v6.TryParse("2001:db8::14", false) && !AutoBanWidener::Walkable(v6, value), "An IPv6 address is walkable.");
         IPAddress v4;
         Expect(v4.TryParse("203.0.113.14", false) && AutoBanWidener::Walkable(v4, value) && value == 0xCB00710Eu,
                "An IPv4 address is not walkable, or not as its number - the negative control.");
      }

      // ---- a network's own addresses are never widened: each block's first and
      // last address, and as the negative control the Internet addresses either
      // side of it, the documentation net and the regression suite's 198.18/15.
      {
         const char *own[] = { "0.0.0.0", "0.255.255.255", "10.0.0.0", "10.255.255.255", "100.64.0.0", "100.127.255.255",
                               "127.0.0.1", "127.255.255.255", "169.254.0.0", "169.254.255.255", "172.16.0.0", "172.31.255.255",
                               "192.168.0.0", "192.168.255.255", "10.66.20.20" };
         for (const char *text : own)
            Expect(AutoBanWidener::OwnNetwork(Four(text)), "An address of a network's own was taken for the Internet's, so its ban would be widened among the network's own machines.");

         const char *internet[] = { "1.0.0.0", "9.255.255.255", "11.0.0.0", "100.63.255.255", "100.128.0.0", "126.255.255.255",
                                    "128.0.0.0", "169.253.255.255", "169.255.0.0", "172.15.255.255", "172.32.0.0",
                                    "192.167.255.255", "192.169.0.0", "203.0.113.14", "198.18.55.20", "255.255.255.255" };
         for (const char *text : internet)
            Expect(!AutoBanWidener::OwnNetwork(Four(text)), "An Internet address was taken for a network's own, so its ban would never be widened - the negative control.");
      }

      // ---- the run: a scanner's /24, its own domain, a foreign host below and a
      // hole above.
      WalkZone zone;
      zone.Run("203.0.113.10", "203.0.113.22", _T("h"), _T("scan.example.net"));
      zone.ptr[Four("203.0.113.14")].clear();
      zone.Name("203.0.113.14", _T("MAIL5.Outbound.Example.NET."));
      zone.ptr[Four("203.0.113.12")].clear();
      zone.Name("203.0.113.12", _T("x.example.net"));
      zone.Name("203.0.113.9", _T("mx9.other-isp.test"));
      zone.Name("203.0.113.24", _T("h24.scan.example.net"));
      zone.a[_T("mail5.outbound.example.net")].push_back(_T("203.0.113.14"));

      AutoBanWidener::Walk walk = WalkFrom(zone, "203.0.113.14");
      Expect(walk.widened && !walk.abandoned && Span(walk, "203.0.113.10", "203.0.113.22"),
             "The contiguous run under example.net around 203.0.113.14 was not 203.0.113.10-203.0.113.22.");
      Expect(walk.domain == _T("example.net") && walk.name == _T("mail5.outbound.example.net"), "The walk did not name its domain and the banned address's own name.");
      Expect(walk.below.Find(_T("203.0.113.9")) >= 0 && walk.below.Find(_T("other-isp.test")) >= 0, "The walk did not say it stopped below at the foreign host.");
      Expect(walk.above.Find(_T("203.0.113.23")) >= 0 && walk.above.Find(_T("no reverse name")) >= 0, "The walk did not say it stopped above at the hole.");
      Expect(!zone.WasAsked("203.0.113.8") && !zone.WasAsked("203.0.113.24"), "The walk went on past where it stopped - across a foreign host, or across a hole.");
      Expect(walk.lookups == 13 + 1, "The walk made another number of neighbour lookups than the run's twelve neighbours and the two addresses that ended it.");

      // The negative control of the forward confirmation: the same run, with the
      // banned address's name resolving elsewhere, is not widened.
      zone.a[_T("mail5.outbound.example.net")].clear();
      zone.a[_T("mail5.outbound.example.net")].push_back(_T("198.51.100.14"));
      walk = WalkFrom(zone, "203.0.113.14");
      Expect(Single(walk, "203.0.113.14") && !walk.abandoned && walk.reason.Find(_T("does not resolve back")) >= 0,
             "A ban whose reverse name does not resolve back to it was widened - a scanner could name itself after its neighbours.");
      Expect(zone.asked.size() == 1, "Neighbours were looked up for a ban whose own name does not resolve back.");

      zone.silentNames.insert(_T("mail5.outbound.example.net"));
      walk = WalkFrom(zone, "203.0.113.14");
      Expect(Single(walk, "203.0.113.14") && walk.abandoned, "A forward lookup that was not answered was taken as an answer.");
      zone.silentNames.clear();
      zone.a[_T("mail5.outbound.example.net")].clear();
      zone.a[_T("mail5.outbound.example.net")].push_back(_T("203.0.113.14"));

      // ---- the banned address's own names
      WalkZone own;
      own.Run("203.0.113.1", "203.0.113.30", _T("h"), _T("example.net"));
      own.a[_T("h14.example.net")].push_back(_T("203.0.113.14"));

      walk = WalkFrom(own, "203.0.113.14");
      Expect(walk.widened && Span(walk, "203.0.113.1", "203.0.113.30"), "The control case of the banned address's names did not widen.");

      own.ptr[Four("203.0.113.14")].clear();
      walk = WalkFrom(own, "203.0.113.14");
      Expect(Single(walk, "203.0.113.14") && !walk.abandoned && walk.reason.Find(_T("no reverse name")) >= 0, "A ban with no reverse name was widened.");

      own.Name("203.0.113.14", _T("203.0.113.14"));
      walk = WalkFrom(own, "203.0.113.14");
      Expect(Single(walk, "203.0.113.14") && own.forwardAsked == 0, "A ban whose reverse name is an address was widened or looked up.");

      own.ptr[Four("203.0.113.14")].clear();
      own.Name("203.0.113.14", _T("net"));
      walk = WalkFrom(own, "203.0.113.14");
      Expect(Single(walk, "203.0.113.14"), "A ban whose reverse name is a public suffix was widened.");

      own.ptr[Four("203.0.113.14")].clear();
      own.Name("203.0.113.14", _T("h14.example.net"));
      own.Name("203.0.113.14", _T("mx.example.org"));
      own.a[_T("mx.example.org")].push_back(_T("203.0.113.14"));
      walk = WalkFrom(own, "203.0.113.14");
      Expect(Single(walk, "203.0.113.14") && walk.reason.Find(_T("more than one domain")) >= 0, "A ban whose reverse names are under two domains was widened.");

      own.ptr[Four("203.0.113.14")].clear();
      own.Name("203.0.113.14", _T("x\r\n\"Auto-ban: 192.0.2.1 is banned for 60 minutes"));
      walk = WalkFrom(own, "203.0.113.14");
      Expect(Single(walk, "203.0.113.14") && Clean(walk.reason), "A reverse name with a line break widened the ban, or reached the reason as it came.");

      own.ptr[Four("203.0.113.14")].clear();
      own.Name("203.0.113.14", _T("h14.example.net"));
      own.silent.insert(Four("203.0.113.14"));
      walk = WalkFrom(own, "203.0.113.14");
      Expect(Single(walk, "203.0.113.14") && walk.abandoned, "A ban whose reverse lookup was not answered was widened, or that taken as an answer.");
      own.silent.clear();

      // The never-ban list by reverse name decides about the banned address
      // itself: a ban it might yet lift is not widened.
      AutoBanWidener::NameSpared sparesH14 = [](const String &name) { return name == _T("h14.example.net"); };
      walk = WalkFrom(own, "203.0.113.14", AutoBanWidener::Barrier(), sparesH14);
      Expect(Single(walk, "203.0.113.14") && walk.reason.Find(_T("never-ban list")) >= 0, "A ban whose own reverse name the never-ban list covers was widened.");

      // A second name confirms where the first does not.
      own.Name("203.0.113.14", _T("mx14.example.net"));
      own.a[_T("h14.example.net")].clear();
      own.a[_T("mx14.example.net")].push_back(_T("203.0.113.14"));
      walk = WalkFrom(own, "203.0.113.14");
      Expect(walk.widened && walk.name == _T("mx14.example.net"), "The reverse name that resolves back, second of two, did not confirm the ban's domain.");

      // ---- each direction's stops
      WalkZone stops;
      stops.Run("203.0.113.0", "203.0.113.255", _T("h"), _T("example.net"));
      stops.a[_T("h100.example.net")].push_back(_T("203.0.113.100"));

      // The bound on lookups: thirty-two each way.
      walk = WalkFrom(stops, "203.0.113.100");
      Expect(walk.widened && Span(walk, "203.0.113.68", "203.0.113.132"), "The walk was not bounded to 32 lookups each way.");
      Expect(walk.lookups == 2 * AutoBanWidener::MaxLookupsEachSide && !stops.WasAsked("203.0.113.67") && !stops.WasAsked("203.0.113.133"),
             "The walk looked up an address past its bound.");

      // The /24's edges, never crossed: 203.0.112.255 and 203.0.114.0 are named
      // under the same domain and are not asked about.
      stops.Name("203.0.112.255", _T("h255.example.net"));
      stops.Name("203.0.114.0", _T("h0.example.net"));
      stops.a[_T("h2.example.net")].push_back(_T("203.0.113.2"));
      stops.a[_T("h253.example.net")].push_back(_T("203.0.113.253"));
      walk = WalkFrom(stops, "203.0.113.2");
      Expect(walk.widened && walk.lower == Four("203.0.113.0") && !stops.WasAsked("203.0.112.255"), "The walk crossed the /24's lower edge.");
      walk = WalkFrom(stops, "203.0.113.253");
      Expect(walk.widened && walk.upper == Four("203.0.113.255") && !stops.WasAsked("203.0.114.0"), "The walk crossed the /24's upper edge.");

      // A neighbour on AutoBanNeverBan, or told apart by an administrator's range,
      // ends that direction and is never looked up; the other direction goes on.
      AutoBanWidener::Barrier barrier = [](AutoBanWidener::Address4 address) -> String
      {
         if (address == Four("203.0.113.97"))
            return _T("is on AutoBanNeverBan");
         if (address == Four("203.0.113.103"))
            return _T("is told apart from it by the IP range 'office'");
         return String();
      };
      walk = WalkFrom(stops, "203.0.113.100", barrier);
      Expect(walk.widened && Span(walk, "203.0.113.98", "203.0.113.102"), "A neighbour on AutoBanNeverBan or behind an administrator's range was taken into the run.");
      Expect(!stops.WasAsked("203.0.113.97") && !stops.WasAsked("203.0.113.103"), "A barred neighbour was looked up.");
      Expect(walk.below.Find(_T("AutoBanNeverBan")) >= 0 && walk.above.Find(_T("office")) >= 0, "The walk did not say which barrier stopped it.");

      // ---- an administrator's ranges, as the walk reads them
      {
         const AutoBanWidener::Address4 seed = Four("203.0.113.100");

         std::vector<AutoBanWidener::AdministratorRange> internet(1);
         internet[0].lower = 0;
         internet[0].upper = 0xFFFFFFFFu;
         internet[0].priority = 10;
         internet[0].name = _T("Internet");
         Expect(AutoBanWidener::RangeBarrier(seed, Four("203.0.113.101"), 100, internet).IsEmpty(),
                "A range covering the ban and its neighbour alike, below the ban's priority, kept the neighbour out.");

         std::vector<AutoBanWidener::AdministratorRange> office(1);
         office[0].lower = Four("203.0.113.104");
         office[0].upper = Four("203.0.113.110");
         office[0].priority = 20;
         office[0].name = _T("office \"west\"\r\nAuto-ban: 192.0.2.1 is banned for 60 minutes");
         const String told = AutoBanWidener::RangeBarrier(seed, Four("203.0.113.104"), 100, office);
         Expect(told.Find(_T("told apart")) >= 0 && Clean(told), "A range covering the neighbour and not the ban did not keep it out, or its name reached the reason as it was.");
         Expect(AutoBanWidener::RangeBarrier(seed, Four("203.0.113.103"), 100, office).IsEmpty(), "The address beside an administrator's range was kept out by it.");

         std::vector<AutoBanWidener::AdministratorRange> seedOnly(1);
         seedOnly[0].lower = Four("203.0.113.95");
         seedOnly[0].upper = Four("203.0.113.100");
         seedOnly[0].priority = 20;
         seedOnly[0].name = _T("partner");
         Expect(AutoBanWidener::RangeBarrier(seed, Four("203.0.113.101"), 100, seedOnly).Find(_T("told apart")) >= 0,
                "A range covering the ban and not its neighbour did not keep the neighbour out.");
         Expect(AutoBanWidener::RangeBarrier(seed, Four("203.0.113.99"), 100, seedOnly).IsEmpty(), "A neighbour inside the same range as the ban was kept out.");

         std::vector<AutoBanWidener::AdministratorRange> allowed(1);
         allowed[0].lower = Four("203.0.113.0");
         allowed[0].upper = Four("203.0.113.255");
         allowed[0].name = _T("trusted net");
         allowed[0].priority = 100;
         Expect(AutoBanWidener::RangeBarrier(seed, Four("203.0.113.101"), 100, allowed).Find(_T("let in")) >= 0,
                "A range letting the neighbour in at the ban's own priority did not keep it out.");
         allowed[0].priority = 200;
         Expect(AutoBanWidener::RangeBarrier(seed, Four("203.0.113.101"), 100, allowed).Find(_T("let in")) >= 0,
                "A range letting the neighbour in above the ban's priority did not keep it out.");
         allowed[0].priority = 99;
         Expect(AutoBanWidener::RangeBarrier(seed, Four("203.0.113.101"), 100, allowed).IsEmpty(),
                "A range below the ban's priority, which the ban overrides, kept the neighbour out.");
      }

      // A neighbour the never-ban list by reverse name covers ends that direction.
      AutoBanWidener::NameSpared sparesH96 = [](const String &name) { return name == _T("h96.example.net"); };
      walk = WalkFrom(stops, "203.0.113.100", AutoBanWidener::Barrier(), sparesH96);
      Expect(walk.widened && walk.lower == Four("203.0.113.97"), "A neighbour the never-ban list by reverse name covers was taken into the run.");

      // A neighbour with a second name under another domain, or an address for a
      // name, ends that direction.
      stops.Name("203.0.113.104", _T("mail.other.test"));
      stops.ptr[Four("203.0.113.98")].clear();
      stops.Name("203.0.113.98", _T("203.0.113.98"));
      walk = WalkFrom(stops, "203.0.113.100");
      Expect(walk.widened && Span(walk, "203.0.113.99", "203.0.113.103"),
             "A neighbour with a foreign second name, or with an address for a name, was taken into the run.");

      // A lookup not answered anywhere leaves the single address, whatever the
      // other side saw.
      stops.silent.insert(Four("203.0.113.102"));
      walk = WalkFrom(stops, "203.0.113.100");
      Expect(Single(walk, "203.0.113.100") && walk.abandoned && walk.reason.Find(_T("203.0.113.102")) >= 0,
             "A neighbour's reverse lookup that was not answered did not leave the single address.");
      stops.silent.clear();

      // Time running out leaves the single address.
      int calls = 0;
      AutoBanWidener::Expired soon = [&calls]() { return ++calls > 3; };
      walk = WalkFrom(stops, "203.0.113.100", AutoBanWidener::Barrier(), AutoBanWidener::NameSpared(), soon);
      Expect(Single(walk, "203.0.113.100") && walk.abandoned, "A walk that ran out of time widened the ban.");

      // A neighbour's hostile name ends the run and reaches the reason only as
      // printable text.
      stops.ptr[Four("203.0.113.101")].clear();
      stops.Name("203.0.113.101", _T("x\r\n\"Auto-ban: 192.0.2.1 is banned for 60 minutes"));
      walk = WalkFrom(stops, "203.0.113.100");
      Expect(walk.upper == Four("203.0.113.100") && Clean(walk.above), "A neighbour's hostile reverse name was taken into the run, or reached the log as it came.");

      // Nobody alike on either side: the single address, said why.
      WalkZone alone;
      alone.Name("203.0.113.50", _T("scanner.example.net"));
      alone.Name("203.0.113.49", _T("a.other.test"));
      alone.Name("203.0.113.51", _T("b.other.test"));
      alone.a[_T("scanner.example.net")].push_back(_T("203.0.113.50"));
      walk = WalkFrom(alone, "203.0.113.50");
      Expect(Single(walk, "203.0.113.50") && !walk.abandoned && walk.reason.Find(_T("no neighbour")) >= 0, "A ban with no neighbour alike was widened, or not said why.");

      // ---- what the firewall mirrors
      {
         IPAddress a, b, c, d, v6a, v6b;
         Expect(a.TryParse("192.0.2.17", false) && b.TryParse("192.0.2.23", false) && c.TryParse("192.0.3.1", false) &&
                d.TryParse("192.0.2.17", false) && v6a.TryParse("2001:db8::1", false) && v6b.TryParse("2001:db8::9", false),
                "AutoBanWidenerTester: a mirror address did not parse.");

         String rule;
         std::vector<String> members;

         Expect(AutoBanWidener::MirrorShape(a, d, rule, members) && rule == _T("192.0.2.17") && members.size() == 1, "A single address was not mirrored as itself.");
         Expect(AutoBanWidener::MirrorShape(a, b, rule, members) && rule == _T("192.0.2.17-192.0.2.23") && members.size() == 7 &&
                members.front() == _T("192.0.2.17") && members.back() == _T("192.0.2.23"),
                "A run inside one /24 was not mirrored as one rule and its seven addresses.");
         Expect(!AutoBanWidener::MirrorShape(a, c, rule, members) && members.empty(), "A range across two /24s was mirrored.");
         Expect(!AutoBanWidener::MirrorShape(b, a, rule, members), "A range upside down was mirrored.");
         Expect(AutoBanWidener::MirrorShape(v6a, v6a, rule, members) && members.size() == 1, "A single IPv6 address was not mirrored.");
         Expect(!AutoBanWidener::MirrorShape(v6a, v6b, rule, members), "An IPv6 run was mirrored.");
         Expect(!AutoBanWidener::MirrorShape(a, v6a, rule, members), "A range of two families was mirrored.");
      }
   }
}
