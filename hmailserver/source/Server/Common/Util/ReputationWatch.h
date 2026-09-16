// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Watching this server's own reputation: the addresses it sends from, looked up
// in the DNS block lists other servers use, once a day.
//
// An administrator whose sending address has been listed learns about it from a
// bounce - usually from the person who was expecting the mail, usually days
// after delivery started failing. The lookup that tells them is the same one
// every receiving server makes, and it costs one DNS query per address per list.
//
// WHERE THE ADDRESSES COME FROM
//
// Three places, and none of them is a guess:
//
//   bind      SMTPDeliveryBindToIP, the address outbound SMTP binds to. When it
//             is set, it IS the address the internet sees this server as.
//   rule      the "bind to address" action of any active global rule, which
//             sends some mail from a different address.
//   setting   ReputationWatchAddresses: what the administrator says this server
//             is seen as.
//
// The setting exists because of NAT. A server behind it binds a private address
// and leaves through a public one it cannot see, and the server has no way to
// discover which. It is NOT discovered by asking a "what is my address" web
// service, deliberately: that service is told, every day, that this machine
// exists and is running a mail server; its answer is whatever address the HTTP
// request left from, which on any host with policy routing, a proxy, or a
// firewall that maps port 25 separately is not the address SMTP leaves from;
// and the answer is unauthenticated, so a wrong one produces a clean report
// about an address that is not this server's - the one failure this feature
// must not have. The addresses of the machine's own interfaces are not
// enumerated either, for the same reason in reverse: behind NAT they are
// private, and on Windows a client-edition host's temporary IPv6 addresses
// (RFC 8981) are new every day, so the set would be noise on the hosts where it
// was not simply wrong.
//
// An address that is private, loopback, link-local, shared (CGNAT), multicast or
// unspecified is not looked up at all, and says so in the report: it is not what
// another server sees, and a query for it would tell the list operator about
// this network's inside.
//
// WHICH LISTS
//
// ReputationWatchLists, one entry per list:
//
//     [ipv4:|ipv6:]zone[=codes]
//
// The zone is what a query is built under (2.0.0.127.zone for the test entry,
// the reversed address for a real one). ipv4: or ipv6: says the list covers only
// that family, and an address of the other family is reported as not covered
// rather than as clean - an IPv4-only list answers NXDOMAIN for every IPv6
// address, which reads exactly like "not listed" and is not the same thing. The
// codes are which answers in 127.0.0.0/8 mean listed (127.0.0.2|127.0.0.4-7|
// 127.0.0.*); with none given, any answer in 127.0.0.0/8 that is not an error
// code means listed. They exist because a few lists answer 127.0.0.1 for
// something else entirely - a welcome list - and that must not read as a block.
//
// The four shipped by default are the ones that answer an ordinary resolver for
// a low-volume user without a key or a registration:
//
//     ipv4:zen.spamhaus.org   IPv4. Free below Spamhaus's own limits - and
//                             REFUSED through a public resolver (Google,
//                             Cloudflare, Quad9), which is not a failure to
//                             notice quietly: it answers 127.255.255.254 and
//                             this code reports that as "refused", never as
//                             clean. A server whose DNSServer is a public
//                             resolver should point it at one of its own, or put
//                             a Spamhaus DQS key's zone in this setting instead.
//     ipv4:bl.spamcop.net     IPv4. Free, no registration.
//     ipv4:psbl.surriel.com   IPv4. Free, no registration.
//     ipv4:bl.mailspike.net   IPv4. Free for low volume.
//
// All four are marked ipv4:, including ZEN. Spamhaus answers IPv6 queries on its
// Data Query Service zone rather than on the free public one, and the other three
// are IPv4 lists; an unmarked zone would answer NXDOMAIN for every IPv6 nibble
// name, and this code would read that as "not listed" - a clean report about an
// address no list has looked at. That is the one failure this file exists to
// prevent, so the shipped default says nothing about IPv6 rather than saying the
// wrong thing, and an IPv6 address reads as not_applicable until an administrator
// names a list that covers it (a DQS zone, or another list whose policy they have
// read). The probe cannot catch this: it asks about 127.0.0.2, an IPv4 entry, so
// a zone that answers IPv4 correctly passes it whatever it does with IPv6.
//
// Deliberately not shipped: b.barracudacentral.org (free, but every resolver
// that queries it must be registered first, so it would answer nothing until
// somebody did that); the Abusix and Spamhaus DQS zones (a key per user, which
// cannot have a default); uceprotect's level 2 and 3 (they list whole networks
// for a neighbour's behaviour); and SORBS, which was shut down in 2024. Each of
// those is one line in the setting for an administrator who wants it.
//
// HOW A REFUSAL IS TOLD FROM A LISTING, AND NEITHER FROM CLEAN
//
// Before any address is looked up, each list is asked about the two entries
// RFC 5782 section 5 requires every list to have: 127.0.0.2, which MUST be
// listed, and 127.0.0.1, which MUST NOT be. That one extra pair of queries per
// list per day is what makes "not listed" mean something:
//
//   the answer is 127.255.255.x        the list refused us (Spamhaus's codes:
//                                      .252 mistyped zone, .254 public resolver,
//                                      .255 too many queries) - reported as
//                                      refused, with the reason it gives.
//   the lookup failed                  timeout, SERVFAIL, no resolver - reported
//                                      as a failed lookup, never as clean.
//   127.0.0.2 is not listed            the list is not answering this resolver -
//                                      a retired zone, a resolver hiding it -
//                                      so "not listed" for a real address would
//                                      be a lie, and is not reported.
//   127.0.0.1 IS listed                the zone answers for everything (what a
//                                      retired list's name does when somebody
//                                      else picks it up), so a listing from it
//                                      is not believed.
//   an answer outside 127.0.0.0/8      not a block list answer at all; a
//                                      resolver answering unknown names with a
//                                      search page does this.
//
// Only when both test entries answer correctly is an address's own answer taken
// at face value. The IPv6 test entry (::FFFF:7F00:2) is not used: lists
// implement it unevenly, and a list that answered the IPv4 pair correctly is
// answering this resolver.
//
// A lookup that fails NEVER clears a listing. The row keeps the moment the
// address was first found listed, and the alert stays raised until a check gets
// a definite answer that says otherwise.
//
// Not for ever, though. A zone that is retired under the server's feet answers
// nothing from then on, so a listing it once gave would hold the alert raised
// until somebody edited the setting - years after the address was delisted
// everywhere. Each row therefore also keeps when a list last gave it a definite
// answer, and after a week without one the pair stops counting as listed. It
// does not become clean: it is unknown, which still exits 1 in hmctl and still
// draws as a warning on the Deck.
//
// A check is bounded in time as well. Twenty lists against thirty-two addresses
// is over a thousand lookups, and the scheduler runs its due tasks one after
// another on one thread, so a resolver that answers nothing would hold the alert
// pass, the backups and the key rotations behind it for hours. The budget is
// tested between lookups, and the pairs a check does not reach are reported as
// failed lookups - which is exactly what they are.
//
// WHAT IS RECORDED
//
// One row per address per list in hm_reputationchecks (schema 6049), rewritten
// by each check: the status, the answer, the reason the list gives, when it was
// checked and since when it has been listed. GET /api/v1/reputation reads it,
// the Control Deck draws it, and AlertManager raises reputation.listed from it.

#pragma once

#include <map>
#include <vector>

namespace HM
{
   class ReputationWatch
   {
   public:

      // Stored in hm_reputationchecks.rcstatus. Only NotListed and NotApplicable
      // are answers that say the address is not on the list; every other value is
      // either a listing or a reason the question was not answered.
      enum Status
      {
         StatusNotListed = 0,
         StatusListed = 1,
         StatusRefused = 2,
         StatusLookupFailed = 3,
         StatusUnexpectedAnswer = 4,
         StatusListNotAnswering = 5,
         StatusListListsEverything = 6,
         StatusNotApplicable = 7,

         // Never stored: what the report says about an address and a list that no
         // check has covered yet.
         StatusNotChecked = 8
      };

      enum Family
      {
         FamilyAny = 0,
         FamilyIPv4 = 4,
         FamilyIPv6 = 6
      };

      // What ClassifyAnswer_ made of the A records a list answered with.
      enum Answer
      {
         AnswerNone = 0,
         AnswerNotListed = 1,
         AnswerListed = 2,
         AnswerRefused = 3,
         AnswerUnexpected = 4
      };

      struct List
      {
         List() : family(FamilyAny) {}

         AnsiString zone;
         int family;
         AnsiString codes;    // empty: any 127.0.0.0/8 answer that is not an error code
      };

      struct Address
      {
         Address() : family(FamilyIPv4) {}

         AnsiString address;
         int family;
         AnsiString sources;  // "bind", "rule", "setting", or several joined with a comma
      };

      // An address the watch will not look up, and the one word saying why.
      struct Skipped
      {
         AnsiString address;
         AnsiString source;
         AnsiString reason;
      };

      struct Result
      {
         Result() : id(0), status(StatusNotListed), checked(0), listed_since(0), confirmed(0) {}

         __int64 id;
         AnsiString address;
         AnsiString sources;
         AnsiString zone;
         int status;
         AnsiString code;     // the A record(s) the list answered with
         AnsiString reason;   // the list's TXT, or the sentence saying what went wrong
         __int64 checked;
         __int64 listed_since;

         // When a list last said something definite about this pair - listed, not
         // listed, or "the other family of addresses". An unanswered check carries
         // it forward rather than moving it, which is what lets a listing nothing
         // has confirmed be told from one that was confirmed this morning.
         __int64 confirmed;
      };

      struct RunSummary
      {
         RunSummary() : as_of(0), addresses(0), lists(0), lookups(0), listed(0), unknown(0), stored(true), complete(true) {}

         __int64 as_of;
         int addresses;
         int lists;
         int lookups;
         int listed;
         int unknown;
         bool stored;

         // False when the check reached MaximumCheckSeconds: the pairs it did not
         // reach are in the report as failed lookups, not as clean.
         bool complete;
      };

      // The zones ReputationWatchLists holds on a new installation, and after an
      // upgrade. Schema 6049 seeds the row with this text.
      static const char *DefaultLists;

      static constexpr int MaximumLists = 20;
      static constexpr int MaximumAddresses = 32;

      // A day. The task wakes every hour and checks when the last check was, so a
      // restart neither loses a day nor causes an extra check.
      static constexpr __int64 SecondsBetweenChecks = 86400;

      // How long a check may go on making lookups. Tested between lookups, so it
      // bounds how MANY a check makes rather than how long any one of them takes:
      // DNSQueryTimeout bounds a single query (ten seconds by default, and 0
      // means wait for ever, in which case a check waits with it).
      static constexpr __int64 MaximumCheckSeconds = 120;

      // How long a listing that no later check has confirmed goes on counting as
      // a listing. See the note at the top: a refused or unanswered check must not
      // delist an address, and a retired zone must not assert one for ever.
      static constexpr __int64 UnconfirmedListingSeconds = 7 * 86400;

      // The two settings, read as the watch uses them. Both return the entries
      // they could read and a sentence naming the first they could not, so a row
      // edited by hand into something unusable costs the entries after it and not
      // the whole check.
      static bool ParseLists(const String &text, std::vector<List> &lists, String &refusal);
      static bool ParseAddresses(const String &text, std::vector<AnsiString> &addresses, String &refusal);

      // Empty when the value can be stored, the refusal sentence when it cannot.
      // COM's setters and the REST settings rows share them, so a value the
      // Control Panel refuses is refused in the same words everywhere.
      static String ValidateLists(const String &text);
      static String ValidateAddresses(const String &text);

      // The addresses this server sends from, and the ones that were named and
      // will not be looked up. Reads settings and the global rules; no DNS.
      static void CollectAddresses(std::vector<Address> &addresses, std::vector<Skipped> &skipped);

      // One check of every address against every list: the lookups, the rows, and
      // the alert. Serialised against itself, so the daily task and an
      // administrator's check on demand cannot interleave; a caller that arrives
      // while a check is running waits for it.
      static RunSummary Run(__int64 now);

      // The same, for a caller that would rather be told than wait: false, and
      // nothing done, when a check is already running. The REST route answers 409
      // with it rather than holding an HTTP request open behind a check that may
      // be making a thousand lookups.
      static bool TryRun(__int64 now, RunSummary &summary);

      // The daily check: the decision and the check under one lock. Read outside
      // it, "is it due" is a question about a check that may finish before the
      // answer is used - and the day's queries to every configured list are then
      // made twice. True when a check ran.
      static bool RunIfDue(__int64 now);

      // Whether the daily check is due: the watch is on and the newest check is a
      // day old, or there has never been one.
      static bool IsDue(__int64 now);

      // What GET /api/v1/reputation answers, and COM's GetReputationReport: the
      // configured lists and addresses now, with the stored answers against them.
      static AnsiString ReportJson(__int64 now);

      // The same document with what the run just did in a "run" member.
      static AnsiString RunJson(const RunSummary &summary, __int64 now);

      // For the reputation.listed alert: true when an address the watch checks is
      // listed - or was, at the last check that got a definite answer. known is
      // false when the rows could not be read, which must not clear an alert.
      //
      // clearSummary is the other half, and the reason this does not simply
      // return a bool: when nothing is listed, WHY nothing is listed decides what
      // the clear may say. "No outbound address is on any list" is true only when
      // every configured pair gave a definite answer; when some list refused, or
      // never answered, or a listing has merely aged out, the clear says that
      // instead. A cleared alert that claims an all-clear it has no answers for
      // is the same failure as a clean report, arriving by post.
      static bool CollectListed(String &summary, String &detail, bool &known, String &clearSummary);

      static const char *StatusWord(int status);

      // The word for an address that will not be looked up ("private",
      // "loopback", ...), empty when it will be. Public for the tester.
      static AnsiString NotCheckedReason(const AnsiString &address);

      // The sentence behind that word, for the report.
      static AnsiString NotCheckedSentence(const AnsiString &reason);

      // Which of AnswerNone .. AnswerUnexpected the answers add up to, and the
      // answer text the report shows. Public for the tester.
      static int ClassifyAnswer(const std::vector<AnsiString> &answers, const AnsiString &codes, AnsiString &code);

      // Whether one 127.0.0.0/8 answer is one of a list's listing codes. An empty
      // codes string matches every answer that is not an error code.
      static bool MatchesCodes(const AnsiString &answer, const AnsiString &codes);

   private:

      static bool Lookup_(const AnsiString &name, int resourceType, std::vector<AnsiString> &values, int &status, int &lookups);
      static AnsiString TextReason_(const AnsiString &name, int &lookups);
      static AnsiString FailureSentence_(int status);
      static AnsiString RefusalSentence_(const AnsiString &code);
      static void ProbeList_(const List &list, Result &verdict, int &lookups);
      static void CheckAddress_(const Address &address, const List &list, Result &result, int &lookups);
      static RunSummary RunLocked_(__int64 now);
      static bool ReadRows_(std::vector<Result> &rows);
      static bool WriteRows_(const std::vector<Result> &results, const std::vector<Result> &previous);
      static AnsiString ClearSentence_(int definite, int notApplicable, int unknown, int aged, int notChecked, int pairs);
      static bool IsListedForAlert_(const Result &result, __int64 now);
      static bool IsUnknown_(const Result &result);
      static AnsiString Key_(const AnsiString &address, const AnsiString &zone);
      static __int64 NewestCheck_();
   };

   // The rules this file states, tested in process: the two settings parsed, the
   // codes matched, an address classified, and the precedence of the answers.
   class ReputationWatchTester
   {
   public:
      void Test();
   };
}
