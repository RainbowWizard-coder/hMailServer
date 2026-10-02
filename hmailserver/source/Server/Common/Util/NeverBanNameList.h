// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <functional>
#include <vector>

namespace HM
{
   class IPAddress;

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
   //     always was and VerifyBan resolves it on the name lookup queue. The ban
   //     is not announced until then: the log line fail2ban matches, the alert
   //     count and the firewall wait for the answer. A spared address has its
   //     range deleted and none of the three happens; any other is announced.
   //     So a listed provider is refused by the server itself for as long as
   //     two lookups take, and never reaches the firewall or fail2ban.
   //
   // A verdict is kept an hour when it spares, ten minutes when it does not, and
   // is forgotten the moment a row is added, changed or removed. A lookup that
   // was not answered decides nothing and is kept for nobody: a ban it could
   // not verify is announced, and asked about again once a minute, three times,
   // by the expiry pass, which lifts it if the answer then spares it.
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

      // A ban has just been written for the address. With an empty list, or a
      // verdict already known not to spare it, announce runs here and now. Else
      // the address is resolved on the name lookup queue, and then either the
      // range is deleted (spared) or announce runs there (not spared). announce
      // is the log line, the alert count and the firewall - everything a ban
      // does below the range. It always runs exactly once unless the address
      // is spared.
      static void VerifyBan(const IPAddress &address, const std::function<void()> &announce);

      // The SMTP session's reverse lookup of the address, on the worker that
      // made it. answered is false when the lookup failed. Forward-confirms a
      // name the list covers, on this thread, and keeps the verdict.
      static void Learn(const IPAddress &address, const std::vector<String> &reverseNames, bool answered);

      // Once a minute, from the expiry pass: asks again about the bans whose
      // lookups were not answered.
      static void RetryUnverified();

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

      static Verdict Resolve_(const IPAddress &address);
      static bool Lift_(const IPAddress &address, const Verdict &verdict);
      static void Unverified_(const IPAddress &address, int attempts);
      static String Key_(const IPAddress &address);
      static bool RealForwardLookup_(const String &name, bool ipv6, std::vector<String> &addresses);

      friend class NeverBanVerifyTask;
      friend class NeverBanRetryTask;
      friend class NeverBanLiftTask;
   };

   // NeverBanNameList and NeverBanName held to what the request asks of them,
   // with no database, no DNS and no setting: the patterns (whole names, suffixes
   // at a label boundary, public suffixes, addresses, stars and the rest
   // refused), the reverse names that may be matched, the forward confirmation
   // (the same address, another address, IPv6, a mapped IPv4 address, a lookup
   // that failed), several reverse names, the most specific pattern, and the
   // verdict cache's keeping and forgetting. Each assertion has its negative
   // control beside it. Runs in ClassTester on Windows and as never-ban-tests
   // on Linux. Throws std::logic_error naming the first thing that is wrong.
   class NeverBanNameListTester
   {
   public:
      void Test();
   };
}
