// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Validating DNSSEC stub resolver (RFC 4033-4035) used for DANE TLSA
// lookups (RFC 7672).
//
// The Windows system resolver is a non-validating stub and does not
// expose a trustworthy AD bit, so this class performs full chain-of-
// trust validation itself: it fetches TLSA/DNSKEY/DS RRsets together
// with their RRSIG records (DO bit set) and verifies every signature
// from the queried name up to the built-in IANA root trust anchors.
//
// Supported algorithms: RSA/SHA-256 (8), RSA/SHA-512 (10),
// ECDSA P-256/SHA-256 (13), ECDSA P-384/SHA-384 (14), Ed25519 (15).
// Supported DS digests: SHA-256 (2), SHA-384 (4). A zone whose validated DS
// RRset names only algorithms or digests outside these is Insecure, as RFC
// 4035 section 5.2 says: there is no path this resolver can follow into it.
//
// A delegation without a DS RRset is Insecure only when the parent proves
// the absence - an NSEC or NSEC3 (SHA-1, Opt-Out understood) in the
// authority section, signed by the parent; without the proof, or with one
// that does not verify, it is Bogus (RFC 4035 section 5.2, RFC 5155
// section 8). A stripped DS is therefore a failure, not a downgrade.
//
// THE TLSA AND MX LOOKUPS ASK WHERE THE NAME IS (RFC 7672 section 2.1.1).
// An answer for them that carries no signature, an answer with no records
// and no proof of their absence, and a lookup that did not complete - no
// server answered, SERVFAIL, a reply that would not parse - are not taken
// for an unsigned name: the resolver walks from the root down to the name,
// asking each label's DS, and the answer is Insecure only when a signed zone
// proves an unsigned delegation above the name (or an NSEC3 Opt-Out span
// leaves one possible), or the root proves the name's top-level name is not
// there at all (a private name, company.local, outside the signed tree).
// Under a zone the walk finds signed, the answer is
// Indeterminate: a signature taken away, records taken out, a lookup made to
// fail. An empty answer under a signed zone counts as no records only with an
// NSEC or NSEC3 proof, signed by that zone, of the name's absence (NXDOMAIN,
// with the wildcard's) or of the type's (NODATA). Until 6.3.6 each of these
// was Insecure, and DANE was off for a destination whenever anybody who could
// disturb its DNS chose. TXT lookups keep the old reading: their callers fall
// back to the system resolver whatever this one says short of Bogus.

#pragma once

#include "DaneVerifier.h"
#include "DNSRecord.h"

namespace HM
{
   class DnssecResolver
   {
   public:
      enum class ChainStatus
      {
         Secure,        // RRset validated up to the root trust anchor - or,
                        // for TLSA and MX, its absence proven by the zone
         Insecure,      // an unsigned delegation was proven above the name
                        // - proceed as if no records exist. For TXT also
                        // what any answer without a usable signature is.
         Bogus,         // validation failed - the records MUST NOT be used
                        // and the host should not be contacted (RFC 7672)
         Indeterminate  // TLSA and MX only: the lookup did not complete, or
                        // the name is under a signed zone and the answer
                        // came without its signatures or without a proof of
                        // absence. Whether DANE applies cannot be told, so
                        // the host is not used, as for Bogus (RFC 7672
                        // section 2.1.1)
      };

      // Looks up and validates TLSA records for _<port>._tcp.<host>.
      // records is only populated when the result is Secure; Secure with no
      // records is a signed zone's proof that the host publishes none.
      ChainStatus QueryTlsa(const String &host_name, int port, std::vector<TlsaRecord> &records);

      // Looks up and validates the MX RRset for a domain. exchanges is populated
      // only for a Secure result - a host name learned from an unvalidated answer
      // is precisely what must not be trusted. Required by RFC 7672 section 2.2
      // before DANE may be applied to any host in that RRset. A signed zone's
      // proof that the name holds no MX RRset is Secure with the name itself
      // as the one exchange, the implicit MX of RFC 5321 section 5.1 (after any
      // validated CNAME, as DNSResolver::GetEmailServers names it); a proof that
      // the name does not exist is Secure with none.
      ChainStatus QueryMx(const String &domain, std::vector<AnsiString> &exchanges);

      // Looks up TXT records with DNSSEC validation. Unlike TLSA, the
      // records are returned for both Secure and Insecure results (an
      // unsigned zone is normal for TXT consumers such as SPF/DKIM/DMARC);
      // only a Bogus result withholds the data.
      ChainStatus QueryTxt(const String &name, std::vector<AnsiString> &texts);

      // Why the last QueryTlsa, QueryMx or QueryTxt of this object was Bogus or
      // Indeterminate: the stage and the check that failed - which RRset, which
      // key tag, and whether a signature did not match, had expired or was not
      // yet valid, named no key the zone publishes, or came without the RRSIG
      // records the DNS server was asked for, or a DNSKEY matched no DS record.
      // Empty after any other result. The debug log has the same words when the
      // verdict is reached; the application log line a deferral is reported
      // with carries them too (ExternalDelivery).
      String GetFailureReason() const;

      // A PLAIN, NON-VALIDATING LOOKUP, for the platform that has no Windows DNS
      // client.
      //
      // This class already carries a complete DNS implementation - the wire
      // encoder, a UDP transport with TCP fallback on truncation, /etc/resolv.conf
      // and the record parser - because validating a chain needs all of it. The
      // POSIX build has no DnsQueryEx to fall back on, so DNSResolverWinApi's
      // POSIX arm calls this instead of reimplementing the same thing badly
      // beside it. It answers exactly what the Windows client answers, in the
      // same DNSRecord shape and with the same status numbers, and it does NO
      // validation: everything that wants a validated answer already calls
      // QueryMx, QueryTxt or QueryTlsa above.
      //
      // status carries the resolver's verdict in the numbers DNSResolver tests
      // for: 0 on success, 9003 when the name does not exist, 9501 when the name
      // exists with no record of that type, and 9002 when no server answered.
      // The distinction matters: 9003 is a permanent failure that bounces mail
      // and 9002 is a temporary one that must not.
      static bool QueryRecords(const AnsiString &name, unsigned short query_type,
                               std::vector<DNSRecord> &records, int &status);

   private:

      // Queries a single name/type, following validated CNAME links, and
      // classifies the response. rdatas is populated for Secure and
      // Insecure results; it is left empty for Bogus, Indeterminate or empty
      // answers. strict is the TLSA and MX reading (the class comment); without
      // it a failed or unsigned answer is Insecure, as it always was for TXT.
      // final_name is the name the answer was for, after the CNAMEs followed;
      // nonexistent is true for a Secure empty answer whose proof is that the
      // name does not exist rather than that it holds no record of the type.
      // rdatas are in the canonical form of RFC 4034 section 6.2: the names in
      // them - an MX record's exchange - written out without compression and in
      // lowercase. Sets failure_reason_.
      ChainStatus QueryValidatedRrset_(const AnsiString &query_name, unsigned short query_type,
                                       std::vector<std::vector<unsigned char>> &rdatas,
                                       bool strict, AnsiString &final_name, bool &nonexistent);

      AnsiString failure_reason_;
   };

   // Which name servers the validating resolver asks, from the ones the system
   // lists (a resolv.conf's nameserver lines, as the POSIX build reads them): the
   // first two IPv4 servers, as always, and only when there is none the first
   // two IPv6 servers, the site-local ones Windows lists for an adapter given no
   // IPv6 server passed over. The canonical form of RDATA a signature is
   // checked over (RFC 4034 section 6.2): an MX record's exchange written out
   // in full and in lowercase, whatever compression it arrived with. And the
   // validator against answers captured from the public DNS
   // (DnssecCapturedAnswers.h) on its own thread, with its clock at their
   // capture: the MX RRsets of live.com and cloudflare.com Secure from the
   // root's keys down, which until work item 80 were Bogus, debian.org's under
   // both its signatures, and the same answers expired, tampered with and
   // stripped of their signatures failing with the reason said. No network,
   // configuration or file. DaneTester runs it, inside the service on Windows
   // (ClassTester) and as dane-tests on Linux. Throws std::logic_error naming
   // the first thing that is wrong.
   class DnssecResolverTester
   {
   public:
      void Test();

   private:
      void TestCanonicalForm_();
      void TestCapturedAnswers_();
   };

}
