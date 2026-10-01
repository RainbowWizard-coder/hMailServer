// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The cut the SURBL lookups and the MX-record spam test make, held to the
// Public Suffix List. See PublicSuffixListTester in PublicSuffixList.h.

#include "StdAfx.h"

#include "PublicSuffixList.h"
#include "DMARC.h"

#include <stdexcept>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // One host and what GetRegistrableDomain must make of it: the name, or
      // nullptr where it must return false and leave the host as it was.
      struct Cut
      {
         const wchar_t *host;
         const wchar_t *expected;
         bool isIPAddress;
         const char *why;
      };

      const Cut cuts[] =
      {
         // The plain cases, cut as SURBL's list of 2006 also cut them.
         { L"example.com", L"example.com", false, "a registered name under a one-label suffix" },
         { L"www.example.com", L"example.com", false, "a host under it" },
         { L"a.b.example.co.uk", L"example.co.uk", false, "a host under a two-label suffix" },
         { L"open-resolver-code.test", L"open-resolver-code.test", false, "a TLD the list does not name (the implicit rule)" },
         { L"mail.example.invalid", L"example.invalid", false, "a host under a TLD the list does not name" },

         // Lowercased, and the root's dots dropped.
         { L"WWW.Example.CO.UK", L"example.co.uk", false, "upper case" },
         { L"A.B.C.KAWASAKI.JP", L"b.c.kawasaki.jp", false, "upper case under a wildcard" },
         { L"mail.example.com.", L"example.com", false, "a trailing dot" },
         { L"mail.example.com..", L"example.com", false, "two trailing dots" },

         // A public suffix, or no name at all: false, and the host unchanged.
         { L"com", nullptr, false, "a TLD" },
         { L"co.uk", nullptr, false, "a two-label suffix" },
         { L"act.edu.au", nullptr, false, "a three-label suffix" },
         { L"c.kawasaki.jp", nullptr, false, "a suffix under a wildcard" },
         { L"c.ck", nullptr, false, "a suffix under a one-label wildcard" },
         { L"blogspot.com", nullptr, false, "a PRIVATE suffix" },
         { L"localhost", nullptr, false, "one label" },
         { L"", nullptr, false, "the empty name" },
         { L".", nullptr, false, "the root" },
         { L"a..com", nullptr, false, "an empty label where the registered one would be" },

         // Empty labels to the left of the registrable domain do not matter.
         { L"a..example.com", L"example.com", false, "an empty label to the left" },
         { L".example.com", L"example.com", false, "a leading dot" },

         // IPv4 addresses, reversed as a DNS list is asked about one.
         { L"10.20.30.40", L"40.30.20.10", true, "an IPv4 address" },
         { L"192.0.2.1.", L"1.2.0.192", true, "an IPv4 address with the root's dot" },
         { L"999.1.2.3", L"2.3", false, "four labels that are not an address" },

         // Wildcard and exception rules, which a cut at two or three labels
         // never followed.
         { L"a.b.c.kawasaki.jp", L"b.c.kawasaki.jp", false, "*.kawasaki.jp" },
         { L"b.c.kawasaki.jp", L"b.c.kawasaki.jp", false, "*.kawasaki.jp, the registered name itself" },
         { L"kawasaki.jp", L"kawasaki.jp", false, "the name a wildcard is under is not a suffix" },
         { L"www.city.kawasaki.jp", L"city.kawasaki.jp", false, "!city.kawasaki.jp" },
         { L"city.kawasaki.jp", L"city.kawasaki.jp", false, "!city.kawasaki.jp, the name itself" },
         { L"a.b.c.ck", L"b.c.ck", false, "*.ck" },
         { L"x.www.ck", L"www.ck", false, "!www.ck" },

         // Suffixes of three labels and more.
         { L"mail.example.act.edu.au", L"example.act.edu.au", false, "act.edu.au" },
         { L"www.example.pvt.k12.ma.us", L"example.pvt.k12.ma.us", false, "pvt.k12.ma.us" },
         { L"www.city.abiko.chiba.jp", L"city.abiko.chiba.jp", false, "abiko.chiba.jp" },

         // The PRIVATE section: a tenant of a hosting name is its own registrant.
         { L"x.blogspot.com", L"x.blogspot.com", false, "blogspot.com" },
         { L"a.tenant.github.io", L"tenant.github.io", false, "github.io" },

         // Where the list of 2006 cut too long: names it listed that are not public
         // suffixes today.
         { L"mail.gov.je", L"gov.je", false, "gov.je, in the list of 2006, is registered under je" },
         { L"shop.gb.com", L"gb.com", false, "gb.com, in the list of 2006, is registered under com" },
         { L"x.y.uk.tt", L"uk.tt", false, "uk.tt, in the list of 2006, is registered under tt" },

         // Where it cut too short: suffixes it did not list, and its "edu.es "
         // with a trailing space, which never matched.
         { L"shop.example.my.id", L"example.my.id", false, "my.id" },
         { L"mail.example.co.ke", L"example.co.ke", false, "co.ke" },
         { L"a.b.example.edu.es", L"example.edu.es", false, "edu.es" },
         { L"a.b.example.sch.uk", L"b.example.sch.uk", false, "*.sch.uk" },
         { L"mail.example.sch.uk", L"mail.example.sch.uk", false, "*.sch.uk, the registered name itself" },

         // Internationalised names, in the form DNS carries them and in the
         // form an EAI address can.
         { L"www.example.xn--p1ai", L"example.xn--p1ai", false, "an IDN TLD, ASCII" },
         { L"www.example.\u0440\u0444", L"example.\u0440\u0444", false, "an IDN TLD, Unicode" },
         { L"www.example.xn--55qx5d.cn", L"example.xn--55qx5d.cn", false, "an IDN suffix of two labels, ASCII" },
         { L"www.example.\u516C\u53F8.cn", L"example.\u516C\u53F8.cn", false, "an IDN suffix of two labels, Unicode" },
      };

      // The cut by the ICANN section alone (PublicSuffixList::IcannSectionOnly),
      // which the SURBL test asks a list about as well: the PRIVATE section's
      // rules - normal and wildcard - do not count, and the ICANN section's
      // wildcard and exception rules still do.
      const Cut icannCuts[] =
      {
         { L"x.blogspot.com", L"blogspot.com", false, "a tenant of a PRIVATE suffix: the hosting company's domain" },
         { L"a.b.blogspot.com", L"blogspot.com", false, "a host under a tenant of a PRIVATE suffix" },
         { L"a.tenant.github.io", L"github.io", false, "github.io, under the ICANN suffix io" },
         { L"evil.duckdns.org", L"duckdns.org", false, "duckdns.org, which URIBL lists whole" },
         { L"github.io", L"github.io", false, "a PRIVATE suffix is a registered name in the ICANN section" },
         { L"blogspot.com", L"blogspot.com", false, "the same, under com" },
         { L"ec2-192-0-2-1.us-west-2.compute.amazonaws.com", L"amazonaws.com", false, "a PRIVATE wildcard (*.compute.amazonaws.com) does not count" },
         { L"www.example.co.uk", L"example.co.uk", false, "an ICANN suffix of two labels still counts" },
         { L"a.b.c.kawasaki.jp", L"b.c.kawasaki.jp", false, "an ICANN wildcard (*.kawasaki.jp) still counts" },
         { L"www.city.kawasaki.jp", L"city.kawasaki.jp", false, "an ICANN exception (!city.kawasaki.jp) still counts" },
         { L"mail.example.act.edu.au", L"example.act.edu.au", false, "an ICANN suffix of three labels still counts" },
         { L"10.20.30.40", L"40.30.20.10", true, "an IPv4 address, reversed as for both sections" },
         { L"co.uk", nullptr, false, "an ICANN suffix is still no registered name" },
         { L"com", nullptr, false, "a TLD" },
      };

      void Fail(const Cut &cut, const std::string &what)
      {
         throw std::logic_error("PublicSuffixListTester: " + std::string(cut.why) + " (" +
            std::string(AnsiString(String(cut.host))) + "): " + what);
      }

      // One cut against its expectation. False where it must be no cut, and
      // none was made.
      bool Check(const Cut &cut, PublicSuffixList::Sections sections, String &host)
      {
         host = cut.host;

         // Set, so that a result that leaves it alone is seen.
         bool isIPAddress = true;

         const bool cutMade = PublicSuffixList::GetRegistrableDomain(host, isIPAddress, sections);

         if (cut.expected == nullptr)
         {
            if (cutMade)
               Fail(cut, "cut to " + std::string(AnsiString(host)) + " where no name is registered");

            if (host != String(cut.host))
               Fail(cut, "the host was changed to " + std::string(AnsiString(host)) + " although no cut was made");

            return false;
         }

         if (!cutMade)
            Fail(cut, "no cut made");

         if (host != String(cut.expected))
            Fail(cut, "cut to " + std::string(AnsiString(host)) + ", not " + std::string(AnsiString(String(cut.expected))));

         if (isIPAddress != cut.isIPAddress)
            Fail(cut, cut.isIPAddress ? "not taken for an address" : "taken for an address");

         return true;
      }
   }

   void
   PublicSuffixListTester::Test()
   {
      for (const Cut &cut : cuts)
      {
         String host;
         if (!Check(cut, PublicSuffixList::IcannAndPrivateSections, host))
            continue;

         // A name has one registrable domain whichever of the server's checks
         // asks: DMARC's organizational domain by the list is the same name.
         if (!cut.isIPAddress)
         {
            const String organizational = DMARC::GetOrganizationalDomainByPublicSuffixList(String(cut.host));
            if (organizational != host)
               Fail(cut, "DMARC's organizational domain is " + std::string(AnsiString(organizational)) + ", not the cut's " + std::string(AnsiString(host)));
         }
      }

      for (const Cut &cut : icannCuts)
      {
         String host;
         Check(cut, PublicSuffixList::IcannSectionOnly, host);
      }
   }
}
