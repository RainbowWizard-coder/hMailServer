// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Restricted delivery (docs/RestrictedDelivery.md, schema 6084): accounts, or
// a whole domain, that may only exchange mail with named domains - a school
// whose pupils write to the school and to nobody else, a regulated desk that
// talks to the firm and its counterparties. Google Workspace calls the same
// thing "restrict delivery".
//
// THE LIST IS THE DOMAIN'S. hm_domains.domainrestricteddomains holds the
// domains the domain's accounts may exchange mail with: at most 2,000
// characters of domain names separated by commas or spaces, lower-cased on
// save, each either a domain (partner.example) or a suffix written with a
// leading dot (.example), which is the spelling the domain policy list
// already uses and whose matcher this reuses - DomainPolicyEntry::Covers, so
// .example covers example and everything beneath it, anchored at a label
// boundary. The list is ASCII, as the policy list is: an internationalised
// name is written in its IDNA (xn--) form. Empty, the default, means the
// domain is not restricted and nothing here runs.
//
// AN ACCOUNT FOLLOWS ITS DOMAIN UNLESS IT SAYS OTHERWISE.
// hm_accounts.accountrestricteddelivery: 0 follows the domain; 1 is
// restricted even when the domain's list is empty - the account is then
// confined to its own domain, the desk that talks to nobody outside; 2 is
// exempt, an account in a restricted domain that may talk to anyone - the
// head teacher.
//
// ALWAYS ALLOWED, whatever the list: the account's own domain and its alias
// domains (an address at an alias name is an address in the domain); the
// domain's postmaster; and the null sender <>, because a bounce or a DSN must
// reach a restricted account or it never learns that a message failed. Other
// domains this server hosts are allowed only when named: "may only exchange
// mail with named domains" means named.
//
// WHERE IT IS ENFORCED, both directions, through the one Check below:
//
//   1. Inbound, at RCPT TO (SMTPConnection::CheckRestrictedDelivery_): a
//      recipient that is a restricted account whose envelope sender is not
//      allowed is answered 550 5.7.1, per recipient, so one refused RCPT does
//      not end the transaction. The From header is not consulted - it is not
//      known at RCPT, and the envelope is the contract, as it is for every
//      other RCPT-time rule.
//   2. Outbound, at RCPT TO, for a submission - an authenticated session, or
//      a local sender a range admits without authentication - whose account
//      is restricted: a recipient whose domain is not allowed is refused
//      550 5.7.1.
//   3. At local delivery (LocalDelivery::DeliverToLocalAccount_), for a
//      recipient reached by expansion rather than by RCPT - a list member, an
//      alias target, a catch-all: a restricted recipient whose envelope
//      sender is not allowed is not delivered; the log says so, and a
//      non-delivery report goes to the sender only when the sender is local
//      (an outside sender met the 550 for a direct address, and silence is a
//      list's ordinary behaviour for a member that refused). A restricted
//      account's forward - the account forward, a rule's forward, a Sieve
//      redirect - to a domain that is not allowed is not made, with a log
//      line, and the message is still delivered to the mailbox; and its
//      out-of-office reply is not sent to a sender who is not allowed.
//
// Every refusal is a DEBUG log line with the two addresses; none is an error,
// because a refusal is the feature working.

#pragma once

#include <memory>
#include <vector>

namespace HM
{
   class Account;
   class Domain;

   class RestrictedDelivery
   {
   public:
      enum Direction
      {
         // The other address is the envelope sender of a message for the account.
         Inbound = 0,
         // The other address is a recipient of a message the account sends,
         // forwards or answers.
         Outbound = 1
      };

      enum Verdict
      {
         Allowed = 0,
         Refused = 1
      };

      // The account's accountrestricteddelivery values.
      enum Mode
      {
         FollowsDomain = 0,
         Restricted = 1,
         Exempt = 2
      };

      // The list's ceiling, in characters of the stored text.
      static constexpr int MaxListLength = 2000;

      // Whether the account is restricted at all: exempt is never, restricted
      // is always, and following the domain is restricted when the domain has
      // a list.
      static bool IsRestricted(const Account &account, const Domain &domain);

      // The decision, made once for the three enforcement points: whether the
      // account may exchange mail with otherAddress in that direction. An
      // account that is not restricted is always Allowed. Logs every refusal
      // at DEBUG with the two addresses.
      static Verdict Check(const Account &account, const Domain &domain, const String &otherAddress, Direction direction);

      // The same, looking the account's domain up; a missing account or domain
      // is Allowed, since there is nothing to restrict.
      static Verdict Check(std::shared_ptr<const Account> account, const String &otherAddress, Direction direction);

      // The list that applies to the account: the domain's patterns, each as
      // stored, or empty when the account is not restricted or the domain
      // names nothing (a restricted account then has its own domain alone).
      static std::vector<String> EffectiveList(const Account &account, const Domain &domain);

      // The stored form of a list as typed: trimmed, lower-cased, the names
      // separated by ", ", duplicates dropped. False with refusal - a sentence
      // REST answers as 400 and COM as its error - when the text is longer than
      // MaxListLength, is not ASCII, or a name is not a domain (an address, a
      // path, an address literal) or is not a valid domain name.
      static bool CanonicaliseList(const String &text, String &canonical, String &refusal);

      // The account's mode from a word or a digit: 0, 1, 2, domain, restricted
      // or exempt, in any case. False for anything else.
      static bool ReadMode(const String &word, int &mode);

      // The word for a mode: domain, restricted or exempt.
      static String ModeWord(int mode);

      // The two refusal sentences, as the 550 texts and the DSN carry them.
      static String InboundRefusal(const String &recipientAddress);
      static String OutboundRefusal(const String &recipientAddress);

   private:

      // The other side's domain as written, and after the domain aliases; both
      // are judged, since a list may name an alias name or the primary.
      static String DomainOf_(const String &address);
      static String ResolvedDomainOf_(const String &address);
   };
}
