// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class HostNameAndIpAddress;
   class DNSRecord;

   class DNSResolver
   {
   private:

   public:
      DNSResolver();
      virtual ~DNSResolver();

      // The addresses of a domain's mail servers - its MX records' exchanges, or
      // the domain itself when it publishes no MX - each address once. False
      // when the lookup did not complete, and on Windows for a null MX (RFC
      // 7505); when the lookup of one exchange did not complete, what the others
      // have is added all the same, each address once. The POSIX resolver drops
      // a null MX's exchange, the root, so there such a domain is one with no MX,
      // and its own addresses are its mail servers.
      bool GetEmailServers(const String &sDomainName, std::vector<HostNameAndIpAddress> &saFoundNames);

      // The names of a domain's exchanges. True with none when the domain
      // publishes no MX, or does not exist (NXDOMAIN) - of a name that does not
      // exist, no CNAME query is made. False when the lookup did not complete:
      // the MX query failed, or it found nothing and was not answered NXDOMAIN
      // (it was answered NODATA, or on Windows the name is an alias, whose
      // records the DNS client returns under the name it is an alias of) and
      // the query that asks whether the name is an alias failed; and on Windows
      // for a null MX (RFC 7505), which the MX-record test reads as a lookup
      // that failed. The POSIX resolver drops a null MX's exchange, so there it
      // is true with none.
      bool GetMXRecords(const String &sDomain, std::vector<String> &vecFoundNames);

      // The name's IPv4 and IPv6 addresses, A and AAAA (AAAA where IPv6 is
      // available on this server), and with followCnameRecords those of the
      // name it is an alias of when it holds none itself. True when addresses
      // were found, or when every query made was answered and none had any - the
      // name does not exist, or holds no address: then nothing is added. A name
      // whose every address query answered NXDOMAIN does not exist, and is asked
      // no CNAME query. False when nothing was found and a query failed (a
      // SERVFAIL, a timeout, no DNS server), because then whether the name has
      // addresses is not known.
      // Until upstream PR 640 a failed A query beside an AAAA query answered "no
      // AAAA records" - the answer of every IPv4-only host - was true with none.
      bool GetIpAddresses(const String &sDomain, std::vector<String> &saFoundNames, bool followCnameRecords);

      // What a DNS list - a DNS blacklist, SURBL - answers for one name: its A
      // records, the only records a list answers with, for an IPv6 address as
      // for an IPv4 one (RFC 5782 sections 2.1 and 2.4), so no AAAA query is
      // made. Asked once more, past the DNS cache, when the query fails, because
      // the answer is a verdict and one failed query would otherwise stand for
      // "not listed" (upstream PR 640). True with the answers added, or with none
      // when the list answered that it holds nothing for the name (NXDOMAIN,
      // NODATA): not listed. False when both attempts failed - a SERVFAIL, a
      // timeout - and then nothing is added.
      bool GetDnsListAnswers(const String &name, std::vector<String> &answers);

      bool GetTXTRecords(const String &sDomain, std::vector<String> &foundResult);
      bool GetPTRRecords(const String &sIP, std::vector<String> &vecFoundNames);

      // Whether the name exists in the DNS at all. NXDOMAIN is the only answer
      // that means no; a name with no records of the type asked for still exists.
      bool DomainExists(const String &sDomain, bool &dnsError);

      // The values of one record type, as the bytes they arrived as, following a
      // CNAME where the name holds none of that type. False means the lookup could
      // not be completed - the query failed, or it found nothing and was not
      // answered NXDOMAIN (it was answered NODATA, or on Windows the name is an
      // alias, whose records the DNS client returns under the name it is an
      // alias of) and the query asking whether the name is an alias failed; true
      // with nothing in values means the name holds no record of that type, or
      // does not exist (NXDOMAIN, which no CNAME query follows). resourceType is
      // a DNS_TYPE_ constant, as DNSResolverWinApi takes.
      //
      // For SPF, which cannot use the typed methods above because each of them adds
      // something its own callers want and RFC 7208 forbids. GetIpAddresses merges A
      // and AAAA into one list and leaves AAAA out unless IPv6 is available on this
      // server, where sections 5.3 and 5.4 select by the family the CLIENT connected
      // over; GetPTRRecords builds a reverse-mapping name out of an address, which an
      // evaluation has already built for the i and v macros, and a name built twice
      // can be built two ways; GetMXRecords reports the null MX of RFC 7505 the way it
      // reports a failed query, which would make it a temperror, where section 5.4
      // reads it as a domain with no exchangers.
      //
      // Bytes rather than String, because a TXT record may hold a byte the wide
      // conversion has no business interpreting, and section 3.1 needs the evaluator
      // to see that byte in order to reject the record. The DNSSEC handling of
      // GetTXTRecords applies here too, for the same record type and the same reason.
      bool GetRecordsOfType(const AnsiString &query, int resourceType, std::vector<AnsiString> &values);

   private:

      bool GetEmailServersRecursive_(const String &sDomainName, std::vector<HostNameAndIpAddress> &saFoundNames, int recursionLevel);
      bool GetIpAddressesRecursive_(const String &hostName, std::vector<String> &addresses, int recursionLevel, bool followCnameRecords);
      bool GetTXTRecordsRecursive_(const String &sDomain, std::vector<String> &foundResult, int recursionLevel);

      // The records of one type, following a CNAME where the name holds none of that
      // type. GetTXTRecordsRecursive_ and GetRecordsOfType differ only in what they do
      // with what comes back, so this is where both look.
      bool GetRecordsOfTypeRecursive_(const String &query, int resourceType, std::vector<DNSRecord> &records, int recursionLevel);

      // The DNSSEC gate GetTXTRecords applies to a TXT lookup: Bogus is a forged
      // answer and fails the lookup, Secure answers from the validating resolver, and
      // Insecure falls through to the system one. answered says which of the three
      // happened - false means "fall through and ask the way you would have".
      bool TryValidatedTxtRecords_(const String &sDomain, std::vector<AnsiString> &texts, bool &answered, bool &bogus);
      bool GetMXRecordsRecursive_(const String &sDomain, std::vector<String> &vecFoundNames, int recursionLevel);

      std::vector<String> GetDnsRecordsValues_(std::vector<DNSRecord> records);
   };

   // What a lookup answers when one of its queries fails, against answers the
   // tester scripts on its own thread in place of the DNS (every other thread
   // asks the DNS as always): an address lookup fails unless addresses were
   // found or every query - A, AAAA and, when one is made, the CNAME query -
   // was answered, whichever of A and AAAA failed and whatever the other
   // answered; NXDOMAIN and NODATA stay "no addresses"; an alias is followed;
   // GetDnsListAnswers asks A alone, and a query that failed once more, and
   // only that one, past the DNS cache; a TXT, MX or other record lookup
   // that found nothing and was not answered NXDOMAIN - NODATA in the answers
   // scripted here, and on Windows an alias too, whose records the DNS client
   // returns under the name it is an alias of - fails when the query asking
   // whether the name is an alias failed (the shape of upstream PR 640, which
   // leaves the record lookups as they were), and a lookup answered NXDOMAIN
   // - for an address lookup, by A and AAAA both - asks no such query and has
   // no records, whatever that query would have met; and GetEmailServers,
   // when the lookup of one of a domain's exchanges failed, still holds an
   // address two of the others share once. It runs inside the service with
   // the other class tests (ClassTester), and on Linux in public-suffix-tests.
   // No DNS, database or setting. It throws std::logic_error naming the first
   // thing that is wrong.
   class DNSResolverTester
   {
   public:
      void Test();
   };

}
