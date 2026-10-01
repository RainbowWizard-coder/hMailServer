// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // Longest-match lookup against the bundled Public Suffix List
   // (https://publicsuffix.org/), with full support for wildcard (*.) and
   // exception (!) rules and both the ICANN and PRIVATE sections.
   //
   // The data is a generated header compiled into the binary - see
   // build\generate-public-suffix-list.ps1 for why it is not a file loaded at
   // runtime - so there is nothing to initialize, nothing to lock, and no way
   // for the data to be absent or stale on an operator's machine.
   class PublicSuffixList
   {
   public:
      // Which of the list's two sections a lookup reads.
      enum Sections
      {
         // Both, as DMARC reads the list: a shared-hosting suffix of the
         // PRIVATE section (github.io) separates its tenants as a registry
         // does.
         IcannAndPrivateSections = 0,

         // The ICANN section alone: the suffixes registries and registrars
         // run, under which a hosting company registers its own domain. The
         // SURBL test asks a URI list about that domain too (SURBL::NamesToAsk),
         // since a list that is not wildcarded lists a whole hosting domain,
         // duckdns.org, under that name.
         IcannSectionOnly = 1
      };

      // Returns how many of the trailing labels of a domain form its public
      // suffix, per the PSL algorithm: exception rules beat all others,
      // otherwise the matching rule with the most labels wins, and a domain
      // matching no rule at all falls back to the implicit "*" rule (its
      // rightmost label is the suffix).
      //
      // labels must be the domain split on '.', already lowercased and with
      // any trailing root dot removed - exactly what GetOrganizationalDomain
      // prepares. Passing prepared labels rather than the raw domain keeps the
      // one split shared between this lookup and the caller's own label
      // arithmetic, so the two can never disagree about label boundaries.
      //
      // Returns 0 only when the compiled-in tables are empty, which a correct
      // build cannot produce; the caller decides the failure direction for
      // that case (see the comment at the call site in DMARC.cpp).
      //
      // sections says which rules count: both sections, as DMARC counts them,
      // unless a caller asks for the ICANN section alone.
      static size_t GetPublicSuffixLabelCount(const std::vector<String> &labels, Sections sections = IcannAndPrivateSections);

      // Cuts a host name to the name a URI blocklist is asked about and whose MX
      // records the MX-record spam test falls back to: its registrable domain,
      // the public suffix and the one label registered under it, from the same
      // list and the same rules as DMARC's organizational domain. SURBL::CleanHost_
      // and SpamTestMXRecords call it.
      //
      //   www.example.co.uk    -> example.co.uk
      //   a.b.c.kawasaki.jp    -> b.c.kawasaki.jp     (*.kawasaki.jp)
      //   www.city.kawasaki.jp -> city.kawasaki.jp    (!city.kawasaki.jp)
      //   tenant.github.io     -> tenant.github.io    (a PRIVATE suffix)
      //   10.20.30.40          -> 40.30.20.10, isIPAddress set
      //
      // The result is lowercase, and trailing dots are dropped. An IPv4 address
      // comes back with its octets reversed, as a DNS list is asked about one.
      // Returns false, and leaves host as it was, for a name that is a public
      // suffix or shorter (co.uk, com, localhost, the empty name), and for one
      // whose registrable domain would hold an empty label ("a..com"): there is
      // no registrant's name to look up.
      //
      // Until 6.3.6 this was TLD::GetDomainNameFromHost, which cut at two labels,
      // or at three under a name in SURBL's list of two-level domains of 2006,
      // a file beside the binary, and reported error 4335 when that file could
      // not be read. The list is compiled in now and cannot be missing, so 4335
      // is no longer reported; the number is retired and not to be given to
      // another error.
      //
      // With IcannSectionOnly the PRIVATE section's rules do not count:
      // tenant.github.io -> github.io, a.b.blogspot.com -> blogspot.com, and
      // github.io itself is a registrable domain, not a suffix.
      static bool GetRegistrableDomain(String &host, bool &isIPAddress, Sections sections = IcannAndPrivateSections);
   };

   // PublicSuffixList held to what the spam tests need of it: the registrable
   // domain of names under one-label, multi-label, wildcard and exception
   // rules and under PRIVATE suffixes, names that are suffixes themselves,
   // IPv4 addresses, case, trailing dots and empty labels, the names where
   // the cut differs from the one SURBL's list of 2006 gave, the same answer
   // as DMARC's organizational domain for every name that has one, and the
   // cut by the ICANN section alone under PRIVATE names and rules. It
   // runs inside the service with the other class tests (ClassTester, which
   // Utilities.RunTestSuite reaches), and on Linux as public-suffix-tests. It
   // needs no database, no setting, no file and no network, and it reports
   // nothing to the error log. It throws std::logic_error naming the first
   // thing that is wrong.
   class PublicSuffixListTester
   {
   public:
      void Test();
   };
}
