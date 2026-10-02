// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Brand logos on receipt (BIMI): the logo a sender's domain publishes, shown
// as the avatar of its mail in the webmail - what Gmail and Outlook.com show.
//
// What makes a logo safe to put beside a message is that the message really
// came from the domain, and that the domain enforces that. So a logo is looked
// up only for mail this server judged dmarc=pass and recorded so in its own
// Authentication-Results header (RestApiServer reads that, not this), and it is
// served only when:
//
//   * default._bimi.<domain> - or, when the domain has none, the same name at
//     its organizational domain - is a v=BIMI1 record with an l= location;
//   * the domain's DMARC policy, found as the DMARC check finds it, is
//     quarantine or reject for all of its mail (pct=100), and the
//     subdomain policy is not none when the policy is the organization's;
//   * the location is https, reached only at a public address, within ten
//     seconds and 32 KB, with no redirect followed - through HttpProxy when
//     one is set (HttpsClient::RequestPublicThroughProxy);
//   * what comes back is SVG Tiny Portable/Secure: an svg root declaring
//     baseProfile="tiny-ps", a title, and nothing that runs or reaches out -
//     no script, no foreignObject, no image, no link, no animation, no event
//     attribute, no entity, no external reference.
//
// A Verified Mark Certificate (a=) is recorded but not checked: the logo is
// the one the domain publishes, which is exactly as much as DMARC proves, and
// no more than that is claimed. Answers are kept a day per domain, found or
// not, so a list's logo is fetched once a day however much it sends.
#pragma once

namespace HM
{
   class BrandLogo
   {
   public:
      enum Outcome
      {
         Found = 0,
         BadDomain,
         SwitchedOff,
         NoRecord,
         Declined,
         WeakPolicy,
         NotHttps,
         FetchFailed,
         NotTinyPs
      };

      // The logo of a domain, or why there is none. reason is one sentence.
      static Outcome Lookup(const String &domain, AnsiString &svg, AnsiString &reason);

      // The answer Lookup gives without asking anybody - a name that is not a
      // domain, the feature switched off, or an answer kept from the last day -
      // and true; false when Lookup would ask the DNS and the logo's host, which
      // is what a REST worker has to be admitted for (RestApiServer's
      // ReachingOut_).
      static bool Known(const String &domain, Outcome &outcome, AnsiString &svg, AnsiString &reason);

      // The word an outcome is reported by: found, bad_domain, switched_off,
      // no_record, declined, weak_policy, not_https, fetch_failed, not_tiny_ps.
      static const char *Word(Outcome outcome);

      // A domain name the lookup will take: lower-cased, letters, digits,
      // hyphens and dots, two labels at least, 253 characters at most. Empty
      // when the text is not one.
      static String NormaliseDomain(const String &text);

      // The pieces the lookup is made of, for the tester.
      static bool ParseRecord(const AnsiString &record, AnsiString &location, AnsiString &authority);
      static bool IsTinyPs(const AnsiString &svg, AnsiString &reason);
      static bool PolicyEnforces(const String &p, const String &sp, int pct, bool atOrganizationalDomain);

      // A lookup that took three seconds or more - the DNS, the logo's host, or
      // the two together, whatever it came to - is held against the whole
      // organizational domain for an hour, not only the name asked: whoever
      // owns attacker.example has a new subdomain for every request, and runs
      // both the DNS each one is looked up in and the host its logo is fetched
      // from; each would otherwise hold a REST worker for the DNS queries'
      // DNSQueryTimeout apiece and the fetch's ten seconds. Silence alone is
      // not the test, because an answer after a wait - a 404, a logo - costs the
      // worker the same and is as easy to give. Known answers FetchFailed for
      // every name under a held domain without asking anybody, except a name
      // whose answer is already kept.
      //
      // The organizational domain is the Public Suffix List's alone
      // (DMARC::GetOrganizationalDomainByPublicSuffixList), with no DNS query.
      // The RFC 9989 tree walk DMARC::GetOrganizationalDomain does by default
      // answers a name with no DMARC record as its own organization, so a
      // tarpit that publishes none would have every subdomain held apart.
      static void NoteUnreachable(const String &domain);
      static bool OrganizationUnreachable(const String &domain);

      // Forgets every answer: a test that changes the zone asks again.
      static void ClearCache();
   };

   class BrandLogoTester
   {
   public:
      void Test();
   };
}
