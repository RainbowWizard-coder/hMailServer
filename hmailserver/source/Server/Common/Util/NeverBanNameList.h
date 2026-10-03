// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <functional>
#include <vector>

namespace HM
{
   class IPAddress;
   class NeverBanWaitingBans;

   // The never-ban list by reverse name (GitLab work item #56): a provider whose
   // outbound hosts share a reverse name and whose addresses move, named once
   // by that name, so that no auto-ban - a run of failed logons, a spam trap, a
   // multi-recipient null sender - is placed on one of its hosts.
   //
   // AutoBanNeverBan (AutoBanFirewall::IsNeverBanned) is the list of addresses
   // and stays it: nets with no reverse name, or one the administrator does not
   // trust, still belong there. The domain white list is neither: it skips
   // greylisting and the score, and stops no ban.
   //
   // WHAT MATCHES. The patterns are the domain white and black list's
   // (BO/NeverBanName.h): mail.example.net that name alone, .example.net that
   // name and every name beneath it at a label boundary. They are compared
   // with a reverse name of the connecting address that is
   //
   //   - a name, not an address written as one, and not a public suffix or
   //     shorter (com, co.uk, localhost): no pattern may cover those, and a
   //     pattern that is one is refused when it is added; and
   //   - FORWARD-CONFIRMED: the name resolves (A for an IPv4 address, AAAA for
   //     an IPv6 one, through a CNAME) back to the very address. A reverse name
   //     is written by whoever holds the address block, and most hosting
   //     companies let a customer write their own; without the forward lookup
   //     anybody could spare their scanner by naming it mail.google.com. Only
   //     the holder of the forward zone can make that name resolve to it.
   //
   // An address with several reverse names is spared when any one of them
   // qualifies. No reverse name, a lookup that was not answered, or a name the
   // forward lookup does not confirm: no match, and the ban proceeds.
   //
   // WHERE IT IS ASKED, AND WHY DNS IS NEVER WAITED FOR ON A SESSION. A ban is
   // placed on a protocol thread - inside RCPT TO for a spam trap - and the
   // decision is read there from what is already known, never looked up:
   //
   //   - the SMTP session's own reverse lookup (SMTPConnection::PrefetchPtrRecord_,
   //     on the name lookup queue at connect) hands every name it got to Learn,
   //     which forward-confirms a name the list covers on that same worker, so
   //     that by the time a session misbehaves the verdict is usually waiting
   //     and no range is ever written;
   //   - IsSpared reads that verdict, and the three ban writers ask it next to
   //     AutoBanNeverBan (AccountLogon::RegisterFailedLogin, SpamTrapMonitor::OnHit,
   //     SMTPConnection's null-sender ban);
   //   - where nothing is known yet - IMAP, POP3, the REST API, ManageSieve, or
   //     a session whose lookup had not finished - the ban is written as it
   //     always was, and VerifyBan resolves it on the never-ban verification
   //     queue, a queue of its own. The ban is not announced until then: the
   //     log line fail2ban matches, the alert count and the firewall wait for
   //     the answer, and the address is PENDING from before its range is
   //     written (Hold) until the answer is in, so that no reconciliation -
   //     another ban's, the expiry pass's - mirrors its range below the server
   //     meanwhile (AutoBanFirewall::ActiveBans_ asks IsPending). A spared
   //     address has its range deleted and none of the three happens; any
   //     other is announced. So a listed provider is refused by the server
   //     itself for as long as the lookups take, and reaches neither the
   //     firewall nor fail2ban - unless its answer is not in by the time the
   //     bounds below give it, when it is announced unverified.
   //
   // WHY A QUEUE OF ITS OWN, AND WHY A BAN WAITS RATHER THAN BEING ANNOUNCED.
   // Before 6.3.6 was released the verifications shared the name lookup queue
   // with every SMTP session's reverse lookup, and past sixteen in flight a ban
   // was announced at once: an outsider whose reverse zone does not answer
   // kept that queue backed up and earned sixteen bans of their own, and the
   // next ban of a listed provider reached the firewall and fail2ban
   // unverified - and the expiry pass mirrored a pending range in any case.
   // Now
   //
   //   - the verifications run on VerifyThreads threads of their own, and each
   //     lookup is given up after LookupDeadlineSeconds whatever the resolver's
   //     own timeout, so the sessions' lookups cannot hold one up;
   //   - a ban waits for its turn rather than being announced: the waiting
   //     bans are taken by network in turn - an IPv4 /24, an IPv6 /48 - so that
   //     one network's bans, however many, delay another's by one lookup each;
   //     MaxPendingPerNetwork of one network wait at once and MaxPending in
   //     all; a ban past its network's is announced at once, and one past
   //     MaxPending gives up the oldest waiting ban of the network with the
   //     most that no thread is asking about, so that the bans of one network
   //     cannot keep another's out;
   //   - a ban whose lookups were not answered is asked about again a minute
   //     later, and a minute a ban waits past its time without a turn counts as
   //     an attempt, so that after MaxAttempts, or HoldSeconds in any case, it
   //     is announced unverified; and a waiting ban whose range has gone -
   //     expired, or deleted by an administrator - is forgotten, not announced.
   //
   // A verdict is kept an hour when it spares, ten minutes when it does not, and
   // is forgotten the moment a row is added, changed or removed. A lookup that
   // was not answered decides nothing and is kept for nobody.
   //
   // An empty list - the default - costs one flag test and changes nothing: a
   // stock server bans exactly as it did.
   class NeverBanNameList
   {
   public:
      // What the list says about one address.
      struct Verdict
      {
         // The address is spared: no range, no firewall rule, no log line for
         // fail2ban.
         bool spared = false;

         // The answer is one the DNS gave, not a lookup that failed. Only a
         // definitive verdict is kept.
         bool definitive = false;

         // The reverse name that decided - forward-confirmed when spared - and
         // the row that covers it. For an address that is not spared, the name
         // and row that came nearest, or empty.
         String name;
         String pattern;

         // One clause for the log line and the check route: why.
         String reason;

         // Set by IsSpared the first time a kept verdict is read, so that the
         // caller writes its application-log line once per verdict rather than
         // once per failed logon.
         bool first_use = false;
      };

      // The forward lookup the evaluation asks: the addresses a name resolves
      // to, A records for ipv6 false and AAAA for true. False when the lookup
      // could not be answered; true with nothing for a name with no such record.
      typedef std::function<bool(const String &name, bool ipv6, std::vector<String> &addresses)> ForwardLookup;

      // ---- the rows ----------------------------------------------------------

      // The rows changed: the next question re-reads them, and every verdict
      // reached under the old rows is forgotten. Called by the persister on
      // every save and delete.
      static void SetNeedRefresh();

      // Whether the list has rows, as last read. Re-reads the table when it was
      // changed, and every five minutes in any case, so that a row another node
      // added to a shared database is in force here too.
      static bool HasEntries();

      // The rows' patterns as last read, normalised, re-read as HasEntries says;
      // for the widening of a ban (AutoBanWidener.h), which stops at a
      // neighbour whose reverse name one of them covers.
      static std::vector<String> Patterns();

      // The list as a JSON document for COM and the Control Panel: items, each
      // with id, pattern, note and created. readable is false when the table
      // could not be read, and the document then has no items.
      static AnsiString Document(bool &readable);

      // Adds a row. False with refusal when the pattern is not a name the list
      // may hold, the note is longer than 255, or the pattern is listed already.
      static bool Add(const String &pattern, const String &note, __int64 &id, String &refusal);

      // Removes the row with that id, answering whether there was one.
      static bool Remove(__int64 id, bool &found);

      // ---- the ban writers ---------------------------------------------------

      // Whether a verdict already known spares the address. Never looks
      // anything up and never blocks on the database: safe inside an SMTP
      // command. False when nothing is known, which is the common case for an
      // address the list does not cover.
      static bool IsSpared(const IPAddress &address, Verdict &verdict);

      // A ban is about to be written for the address (AccountLogon's
      // CreateIPRange, before the range is saved): while the list has rows and
      // nothing is known about the address, it is pending from now, so that a
      // reconciliation that reads the range before VerifyBan has run does not
      // mirror it. Release takes it back when the range could not be saved.
      // Neither looks anything up.
      static void Hold(const IPAddress &address);
      static void Release(const IPAddress &address);

      // A ban has just been written for the address. With an empty list, or a
      // verdict already known not to spare it, announce runs here and now. Else
      // the address stays pending and is resolved on the verification queue,
      // and then either the range is deleted (spared) or announce runs there
      // (not spared, not answered MaxAttempts times, or HoldSeconds gone).
      // announce is the log line, the alert count and the firewall - everything
      // a ban does below the range. It runs at most once, and never for an
      // address that is spared or whose range has gone before an answer.
      static void VerifyBan(const IPAddress &address, const std::function<void()> &announce);

      // Whether the address's ban waits for the list's answer: the firewall
      // reconciliation leaves its single-address range out until it is in.
      static bool IsPending(const IPAddress &address);

      // The SMTP session's reverse lookup of the address, on the worker that
      // made it. answered is false when the lookup failed. Forward-confirms a
      // name the list covers, on this thread, and keeps the verdict.
      static void Learn(const IPAddress &address, const std::vector<String> &reverseNames, bool answered);

      // Once a minute, from the expiry pass: forgets the waiting bans whose
      // ranges have gone, counts a minute a ban waited past its time as an
      // attempt, announces the bans out of attempts or time, and sets the
      // verification queue to work on the bans whose lookups were not answered
      // a minute ago.
      static void RetryUnverified();

      // The bounds of the waiting bans and their lookups (the class comment
      // says why each is there).
      static constexpr int VerifyThreads = 4;
      static constexpr int LookupDeadlineSeconds = 5;
      static constexpr int MaxAbandonedLookups = 32;
      static constexpr int RetrySeconds = 60;
      static constexpr int MaxAttempts = 4;
      static constexpr int HoldSeconds = 600;
      static constexpr size_t MaxPending = 1024;
      static constexpr size_t MaxPendingPerNetwork = 16;

      // The name of the verification queue, which the application makes with
      // VerifyThreads threads beside the session queues (Application.cpp).
      static const wchar_t *QueueName();

      // ---- diagnosis ---------------------------------------------------------

      // What the list would decide about an address, resolved now on the
      // calling thread (seconds at worst): the document COM's
      // CheckAutoBanNeverBanName and POST /api/v1/never-ban-names/check answer:
      //   {address, reverse_names, answered, spared, name, pattern, reason}
      // Nothing is kept. False with refusal when the text is not an address.
      static bool Check(const String &addressText, AnsiString &document, String &refusal);

      // ---- the rules, with no database and no network -------------------------

      // A reverse name as the list compares it: trimmed, lower-cased, no
      // trailing dot.
      static String NormaliseName(const String &name);

      // Whether a normalised reverse name may be matched at all: not empty, not
      // an address, no empty label, and not a public suffix or shorter.
      static bool IsUsableReverseName(const String &normalisedName);

      // The most specific of the patterns covering the name - a whole name beats
      // a suffix, then the longer pattern - or empty when none does.
      static String CoveringPattern(const String &normalisedName, const std::vector<String> &patterns);

      // Whether a forward lookup's answer is the address: the same address in
      // the same family, an IPv4 address mapped into IPv6 counted as the IPv4
      // one.
      static bool SameAddress(const IPAddress &address, const String &answer);

      // The decision, from the reverse names an address has, the patterns, and
      // the forward lookup to confirm a covered name with.
      static Verdict Evaluate(const IPAddress &address, const std::vector<String> &reverseNames, bool answered,
                              const std::vector<String> &patterns, const ForwardLookup &forward);

      // How many reverse names of one address are looked at, and how many forward
      // lookups one verdict may cost. An address can carry any number of PTR
      // records, and each covered one is a lookup.
      static constexpr int MaxReverseNames = 8;
      static constexpr int MaxForwardLookups = 4;

   private:

      friend class NeverBanNameListTester;

      // The rows as last read, re-read when changed or five minutes old. False
      // for an empty list.
      static bool EnsureRows_(std::vector<String> &patterns);
      static bool Refresh_();

      // The verdict cache. A verdict is kept only when it is definitive and the
      // rows are still the ones it was reached under (generation, read before
      // the lookups began): a verdict that raced a change of the rows is
      // dropped, never kept.
      static unsigned Generation_();
      static void Keep_(const IPAddress &address, const Verdict &verdict, unsigned generation);
      static bool Known_(const IPAddress &address, Verdict &verdict, bool markUsed);
      static void Forget_();
      static size_t KnownCount_();

      static bool Lift_(const IPAddress &address, const Verdict &verdict);
      static String Key_(const IPAddress &address);
      static bool RealForwardLookup_(const String &name, bool ipv6, std::vector<String> &addresses);

      // The address's reverse names looked up and evaluated, every lookup held
      // to LookupDeadlineSeconds, for the waiting bans (NeverBanWaitingBans, in
      // NeverBanNameList.cpp, which holds them, takes them by network in turn
      // and decides them).
      static Verdict ResolveWithinDeadline_(const IPAddress &address);

      // The network a waiting ban is taken in turn with: an IPv4 address's
      // /24, an IPv6 address's /48.
      static String Network_(const IPAddress &address);

      friend class NeverBanWaitingBans;
   };

   // NeverBanNameList and NeverBanName held to what the request asks of them,
   // with no database, no DNS and no setting: the patterns (whole names, suffixes
   // at a label boundary, public suffixes, addresses, stars and the rest
   // refused), the reverse names that may be matched, the forward confirmation
   // (the same address, another address, IPv6, a mapped IPv4 address, a lookup
   // that failed), several reverse names, the most specific pattern, and the
   // verdict cache's keeping and forgetting; and the waiting bans, with a
   // stand-in resolver that hangs until the test lets it answer: a pending ban
   // is not announced and is left out of the firewall's bans, is lifted when it
   // verifies and announced when it does not, takes its turn by network while
   // another network's bans crowd the queue, is announced after its attempts
   // or its time, is forgotten when its range goes - and not when it was
   // written while the ranges were being read - a full set gives up another
   // network's ban rather than announcing a new one at once, a worker its
   // queue asks to stop takes no more bans, and a lookup that hangs is given
   // up at its deadline, or sooner when its worker is asked to stop. Each
   // assertion has its negative control beside it. Runs in ClassTester on
   // Windows and as never-ban-tests on Linux. Throws std::logic_error naming
   // the first thing that is wrong.
   class NeverBanNameListTester
   {
   public:
      void Test();
   };
}
