// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Impersonation protection (docs/ImpersonationProtection.md, schema 6087):
// Microsoft 365's anti-phishing policies, per domain. Two checks on a message
// from OUTSIDE - the decision ExternalSenderTagger already makes through
// MessageOrigin, shared and not re-derived - for a recipient whose domain has
// the action on:
//
//   1. A BORROWED NAME. The From header's display name, folded (see FoldName:
//      NFKC where the platform has it, the invisible format characters
//      deleted, lower case, runs of white space and punctuation collapsed,
//      the Latin, Cyrillic and Greek confusables mapped to the Latin letter
//      they look like), equals the folded person name of an ACTIVE account of
//      the recipient's domain - first and last name as the directory composes
//      them - or of a protected person, while the From ADDRESS is not that
//      account's own, nor one of its aliases, nor the protected person's
//      address when one is given. A display name that is itself an address
//      ("ceo@example.test" <x@evil.test>) is compared as an address.
//
//   2. A LOOKALIKE DOMAIN. The From address's domain, and the envelope
//      sender's when it differs, folded the same way (IDNA labels decoded
//      first), is within Damerau-Levenshtein distance 1 of a domain this
//      server hosts or of a protected partner - one insertion, deletion,
//      substitution or transposition inside one label, never one that adds,
//      removes or moves a dot, so that a name is only ever compared with a
//      name of the same label count - or is equal to one after the confusable
//      folding while not equal before it; and is not itself a hosted domain,
//      an alias domain or a partner, nor beneath one (mail.example.test is
//      example.test's own). The candidate and each of its parent names down
//      to two labels are judged, so secure.examp1e.test is caught by the
//      examp1e.test inside it. A DMARC pass changes nothing here: a lookalike
//      that passes its own DMARC is still a lookalike.
//
// WHAT HAPPENS. Flag (1): the copy gets one header,
//   X-hMailServer-Impersonation: name "<shown>" matches <person> (<owner>); from <address>
//   X-hMailServer-Impersonation: domain <x> looks like <y>; from <address>
// one line, RFC 2047 encoded when the shown name is not ASCII, added as
// X-hMailServer-External is added; the subject is left alone; the webmail
// turns it into a warning bar and a mark on the row. Quarantine (2): the copy
// goes to the quarantine through QuarantineStore::Quarantine with the reason
// as the row's reason and score 0, exactly as a spam verdict does; the digest
// and the administrator's release then work as they do for spam, and a
// release re-delivers WITHOUT re-checking, as a spam release does. With the
// quarantine store off, a quarantine action flags instead - a message that
// was not stored anywhere is not discarded. Either way one application-log
// line at DEBUG with the addresses, never the error log.
//
// EXEMPT: a sender the recipient's own sender list allows (SenderListCache,
// the reader's safe senders - an address or a domain the person marked safe,
// or the domain's or the server's allow list where nothing nearer decides),
// and a From domain that is itself hosted here or a protected partner,
// exactly (the envelope sender is still judged). Nothing else.
//
// COST. Off, the shipped state, nothing is read: the domain's action is a
// field on the cached domain. On, what a domain's judgement needs - its
// active accounts' names and addresses with their aliases, folded, its
// protected persons, and the hosted and partner names folded - is read once
// and held per domain id (Facts), invalidated when an account, an alias, a
// domain or an alias domain is saved or deleted and after sixty seconds as a
// safety net; a copy from outside then costs the header parse and the
// comparisons, and a copy from inside nothing beyond the origin decision the
// tagger shares.

#pragma once

#include <memory>
#include <vector>

#include "MessageOrigin.h"

namespace HM
{
   class Account;
   class Domain;
   class Message;

   class ImpersonationCheck
   {
   public:
      enum Action
      {
         Off = 0,
         Flag = 1,
         Quarantine = 2
      };

      enum Outcome
      {
         // The domain's action is off, the message is from inside, or nothing
         // matched.
         Clean = 0,
         // The header was written on the copy.
         Flagged = 1,
         // The copy is in the quarantine and must not be delivered.
         Quarantined = 2
      };

      // The two lists' ceiling, in characters of the stored text.
      static const int MaxListLength = 2000;

      // How much of a display name the reason carries before an ellipsis, so
      // that the header line stays within what RFC 5322 allows.
      static const int MaxShownLength = 120;

      // How long a domain's facts are held before they are read again
      // whatever happened, the safety net under the invalidation.
      static const int CacheSeconds = 60;

      // A name the domain protects: an account's person name with the
      // account's own addresses (its address and its aliases), or a protected
      // person's name with the address given for it, if any. owner is what the
      // header says the name belongs to; folded is FoldName(name), computed
      // once where the person is built and used when it is there.
      struct Person
      {
         String name;
         String folded;
         std::vector<String> addresses;
         String owner;
      };

      // A hosted or partner name prepared once for the lookalike judgement:
      // as stored (lower case), IDNA decoded, and folded.
      struct Name
      {
         String raw;
         String idna;
         String folded;
      };

      // What one domain's judgement reads, held per domain id: the unit's
      // own, public only so that the holder can name it.
      struct Facts;

      static const char *Header() { return "X-hMailServer-Impersonation"; }

      // Runs both checks on this account's copy and does what the domain asks;
      // origin is the shared external-sender decision, judged here (from the
      // copy's header and sendersIP) if the tagger has not judged it yet.
      static Outcome Apply(std::shared_ptr<const Account> account,
                           std::shared_ptr<const Domain> domain,
                           std::shared_ptr<Message> message,
                           const String &sendersIP,
                           MessageOrigin::Verdict &origin);

      // Forgets every domain's held facts: called when an account, an alias,
      // a domain or an alias domain is saved or deleted.
      static void Invalidate();

      // THE TWO CHECKS, as functions of values, so that they can be tested
      // without a delivery. Both answer the reason in the header's own shape.

      // True with reason when displayName borrows a name in people while
      // fromAddress is none of that person's addresses.
      static bool JudgeName(const String &displayName, const String &fromAddress,
                            const std::vector<Person> &people, String &reason);

      // True with reason when domain, or a parent name of it down to two
      // labels, is a lookalike of a name in names. label is what the reason
      // calls the address - "from" or "envelope sender".
      static bool JudgeDomain(const String &domain, const std::vector<Name> &names,
                              const String &address, const String &label, String &reason);

      // The same with the hosted and partner names as stored, prepared here.
      static bool JudgeDomain(const String &domain, const std::vector<String> &hosted,
                              const std::vector<String> &partners, const String &address,
                              const String &label, String &reason);

      // The names as stored - lower-case ASCII - prepared for the judgement.
      static std::vector<Name> PrepareNames(const std::vector<String> &names);

      // THE FOLDING, in the doc's table. FoldName is for a display name;
      // FoldDomain decodes IDNA labels first and does not collapse white space,
      // which a domain has none of.
      static String FoldName(const String &name);
      static String FoldDomain(const String &domain);

      // A domain with its IDNA labels decoded and lower-cased and nothing
      // else: the U-label and the A-label of one name compare equal here, and
      // that is what the exemption and the not-equal-before-folding test read.
      static String IdnaOf(const String &domain);

      // The confusable folding alone, on a string that is already lower case:
      // rn to m, vv to w, 1 l I | to l, 0 to o, and the Cyrillic and Greek
      // letters that look like Latin ones to the Latin letter.
      static String FoldConfusables(const String &text);

      // Restricted Damerau-Levenshtein (optimal string alignment) distance.
      static int EditDistance(const String &a, const String &b);

      // RFC 3492 Punycode, one label without its xn-- prefix. False when the
      // label is not valid Punycode, in which case the caller keeps it as is.
      static bool DecodePunycode(const AnsiString &label, String &decoded);

      // THE TWO LISTS' STORED FORMS.

      // Protected persons: entries separated by line breaks or commas - a
      // comma inside a quoted name is part of the name - each "Name <address>",
      // "Name" or "address"; stored one per line, a name holding a comma in
      // quotes, addresses lower-cased, names as typed. False with a sentence -
      // REST's 400, COM's error - when the text is longer than MaxListLength,
      // an entry's address is not an address, or a quote is unbalanced.
      static bool CanonicalisePersons(const String &text, String &canonical, String &refusal);

      // Protected partners: domain names separated by commas or spaces, ASCII
      // (IDNA for an internationalised name), lower-cased, ", " between them,
      // duplicates dropped. False with a sentence when an entry is an address,
      // not a domain name, not ASCII, or the text is too long.
      static bool CanonicalisePartners(const String &text, String &canonical, String &refusal);

      // The persons list as stored, parsed back into people for JudgeName; a
      // bare address that is an account here protects that account's name too.
      static std::vector<Person> ParsePersons(const String &stored);

      // The action from a digit or a word: 0/off, 1/flag, 2/quarantine, in any
      // case. False for anything else.
      static bool ReadAction(const String &word, int &action);
      static String ActionWord(int action);

   private:
      static std::shared_ptr<const Facts> FactsFor_(std::shared_ptr<const Domain> domain);
      static std::vector<Person> AccountsOf_(std::shared_ptr<const Domain> domain);
      static std::vector<String> HostedDomains_();
      static std::vector<String> SplitList_(const String &stored);
      static String OneLine_(const String &text);
   };

   // Run from ClassTester::DoTests.
   class ImpersonationCheckTester
   {
   public:
      void Test();

   private:
      void TestFolding_();
      void TestNames_();
      void TestDomains_();
      void TestLists_();
   };
}
