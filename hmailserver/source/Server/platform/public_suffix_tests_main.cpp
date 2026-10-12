// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The Public Suffix List's cut of a host name - the name the SURBL lookups ask
// about and the MX-record spam test falls back to - as a program of its own, for
// the POSIX build: the counterpart of spf-tests and xml-tree-tests. With it, the
// SURBL test's reading of the links in a message, which hands the cut its hosts:
// the host after a link's userinfo, the forms of an address, a host as UTS 46
// reads it (the code points a browser deletes, the ones it maps, xn-- forms;
// idna-tests holds UTS 46 itself), links with no scheme, HTML's character references,
// and the answers the lists publish as errors; and what the lookup SURBL and the
// DNS blacklists make - of the A record alone - and the address lookup answer
// when a query fails (DNSResolverTester, upstream PR 640). And the session names
// that are not names of GitLab issue 51 - a HELO name or a sender's domain that
// is an address, one label or a public suffix by the same list - with the
// carve-outs of the refusal made with them (FqdnCheckTester), and before it what
// an address is, which that refusal asks first (IPAddressTester). And the
// Return-Path a SpamAssassin scan sends ahead of the message, with the
// Return-Path fields left out of the request and of spamd's reply byte for byte,
// and the path read from the topmost Return-Path of a message collected from an
// external account, which the scan of such a message is sent
// (SpamAssassinReturnPathTester).
//
// On Windows PublicSuffixListTester, SURBLTester, FqdnCheckTester and
// SpamAssassinReturnPathTester run inside the service with the other class
// tests: ClassTester::DoTests calls them and the regression suite reaches them
// through Utilities.RunTestSuite. That route does
// not exist here - it is a COM method, and the Linux suite's shim skips it - so
// the CMake build makes this, and linux-build.sh runs it with every other *-tests
// program. It needs no database, no configuration, no file and no network: the
// list is compiled in, and nothing is looked up: DNSResolverTester's answers are
// its own, given on its own thread in place of the DNS.
//
// It prints one line for each tester and exits non-zero on the first failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/AntiSpam/DMARC/PublicSuffixList.h"
#include "../Common/AntiSpam/SURBL.h"
#include "../Common/AntiSpam/FqdnCheck.h"
#include "../Common/TCPIP/DNSResolver.h"
#include "../Common/AntiSpam/SpamAssassin/SpamAssassinReturnPath.h"
#include "../Common/TCPIP/IPAddress.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   try
   {
      HM::PublicSuffixListTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the Public Suffix List's cut: %s\n", failure.what());
      return 1;
   }

   printf("ok    the Public Suffix List's cut: hosts, addresses, suffixes, wildcard and exception rules, PRIVATE names, DMARC's answer for each, and the cut by the ICANN section alone\n");

   try
   {
      HM::SURBLTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the SURBL test's links: %s\n", failure.what());
      return 1;
   }

   printf("ok    the SURBL test's links: the host after the userinfo, ports, IPv6 and the IPv4 addresses it carries, percent-encoding as UTF-8, UTS 46 (the code points it ignores, fullwidth letters, ideographic full stops, xn-- forms, the labels it refuses), the forms of an address, links with no scheme, HTML's references and quotes, the names asked about, a lookup that fails passed over and one that throws ending the list, the lists' error codes, and the sender lists' URL entries read the same way and matched against every link\n");

   // What an address lookup answers when one of its queries fails, and the
   // lookup the SURBL lookups above and the DNS blacklists make - of the A
   // record alone, asked once more when it fails: the same answers here as on
   // Windows, where DNSResolverTester runs in ClassTester.
   try
   {
      HM::DNSResolverTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  a lookup with a failed query: %s\n", failure.what());
      return 1;
   }

   printf("ok    a lookup with a failed query: a failure and not \"no addresses\" unless addresses were found, whichever of A and AAAA failed and whatever the other answered; NXDOMAIN and NODATA still no addresses; an alias followed; a DNS list asked for A alone, and once more past the DNS cache when that query fails, and only then; a lookup of an alias whose CNAME query failed a failure, and one answered NODATA whose CNAME query failed without records, as before; a name answered NXDOMAIN asked no CNAME query, and without records whatever one would meet; a domain's mail servers with an exchange whose lookup failed, each address once\n");

   // What is an address, which the refusal below asks first: the same answer
   // here as on Windows, where IPAddressTester runs in ClassTester.
   try
   {
      HM::IPAddressTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  what is an address: %s\n", failure.what());
      return 1;
   }

   printf("ok    what is an address: four decimal parts, RFC 4291's IPv6 with a zone, and none of the older IPv4 forms, brackets or ports Windows' own parser reads\n");

   try
   {
      HM::FqdnCheckTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the session names that are not names: %s\n", failure.what());
      return 1;
   }

   printf("ok    the session names that are not names: addresses with and without brackets, one label, the root, public suffixes by the same list, characters no host name holds, an all-digit top label, and the refusal's switches and carve-outs one at a time\n");

   try
   {
      HM::SpamAssassinReturnPathTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the Return-Path a SpamAssassin scan is sent: %s\n", failure.what());
      return 1;
   }

   printf("ok    the Return-Path a SpamAssassin scan is sent: the line ahead of the message, and the Return-Path fields of a header block left out byte for byte - first, last, twice, folded, in any case, with LF lines - and nothing after it, in every split and a byte at a time; and the path of a collected message's topmost Return-Path, in every form a path takes, under Received fields and the first of two, and none for a value that is no path\n");
   return 0;
}
