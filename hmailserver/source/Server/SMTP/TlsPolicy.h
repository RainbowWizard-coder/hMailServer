// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Outbound TLS policy support:
//   - MTA-STS (RFC 8461): SMTP MTA Strict Transport Security
//   - DANE TLSA lookups (RFC 6698 / RFC 7672) for outbound SMTP
//
// DANE lookups are DNSSEC-validated in-process (see DnssecResolver):
// the chain of trust is verified from the TLSA RRset up to the IANA
// root trust anchors. Validation results follow RFC 7672:
//   secure        -> TLSA records are enforced; a secure RRset none of whose
//                    records is usable still requires TLS, unauthenticated
//                    (section 2.2); a signed zone's proof of none is no records
//   insecure      -> treated as if no TLSA records exist: the name is proven
//                    to sit below an unsigned delegation
//   bogus         -> the MX host must not be used
//   indeterminate -> the lookup did not complete, or came without the
//                    signatures or the proof its signed zone owes: the MX host
//                    must not be used either (section 2.1.1). Until 6.3.6 this
//                    was insecure, and DANE could be switched off for a
//                    destination by anybody who could disturb its DNS.
//
// RFC 7672 section 2.2 requires more than a validated TLSA RRset: the
// MX RRset that produced the host name must be DNSSEC-validated too.
// A TLSA record found under a host name learned from a forged MX
// response proves nothing, because the attacker chose the host - its
// own TLSA record then validates perfectly and DANE reports success.
// GetTlsaRecords therefore takes the MX RRset's validation status as
// an input (MxDnssecStatus); EvaluateMxDnssecStatus derives it, and
// also checks that the host about to be contacted is one of the names
// the validated MX RRset actually published.
//
// Exactly one MxDnssecStatus value can stop a delivery (Bogus), it is the
// value furthest from zero, and every other value - including anything a
// caller produces by accident - lands on the ordinary delivery path. See
// the invariant on MxDnssecStatus below. Since 6.3.6 Bogus is reached from
// three results of the validated MX lookup, not one: an RRset that failed
// validation, a lookup that was indeterminate (section 2.1.1), and a secure
// RRset that does not name the host about to be contacted (section 2.2.1 -
// the host came from the plain, unvalidated lookup). Until then the last two
// were Insecure, and a destination's DANE was switched off by a dropped or a
// forged DNS answer.
//
// PLUMBED 13 August 2026. DnssecResolver::QueryMx supplies the validated
// MX RRset, ExternalDelivery looks it up once per recipient domain and
// EvaluateMxDnssecStatus derives the per-host status from it.
//
// One deliberate narrowing, because it decides what happens to real mail:
// QueryMx parses the exchange name out of the rdata and REFUSES a
// compression pointer rather than following one, since it does not hold
// the surrounding packet. That is not merely conservative - an MX RRset
// can only have validated if the canonical, uncompressed form of its
// rdata is what was signed (RFC 4034 section 6.2) - but where a resolver
// does hand back compressed rdata, the effect is a name this code will
// not parse, hence no validated MX host, hence Insecure, hence delivery
// exactly as it was before.
//
// Setting DnssecValidationEnabled=0 reverts to opportunistic
// (unvalidated) TLSA usage.

#pragma once

#include "../Common/TCPIP/DaneVerifier.h"
#include "../Common/TCPIP/DnssecResolver.h"
#include "../Common/TCPIP/SocketConstants.h"

namespace HM
{
   class TlsPolicy
   {
   public:

      enum StsMode
      {
         StsNone = 0,
         StsTesting = 1,
         StsEnforce = 2
      };

      struct StsPolicy
      {
         StsMode mode = StsNone;
         std::vector<String> mx_patterns;
      };

      // Returns the MTA-STS policy for a recipient domain. Policies are
      // cached in-process according to the policy max_age.
      static StsPolicy GetStsPolicy(const String &domain);

      // True if the given MX host name matches one of the mx patterns in
      // the policy (RFC 8461 section 4.1; "*." matches one leftmost label).
      static bool HostMatchesStsPolicy(const String &host_name, const StsPolicy &policy);

      enum class TlsaLookupStatus
      {
         DnssecValidated,  // records returned and DNSSEC-validated
         Unvalidated,      // records returned without validation
                           // (DnssecValidationEnabled=0)
         NoRecords,        // no records, or not DNSSEC-validated - deliver
                           // without DANE
         Unusable,         // a DNSSEC-validated RRset of records none of
                           // which this server can use (RFC 7672 section
                           // 2.2): TLS is required, authentication is not
         Bogus,            // DNSSEC validation failed - do not use host
         Indeterminate     // the lookup did not complete, or came without
                           // the signatures or the proof its signed zone
                           // owes - do not use host (RFC 7672 section 2.1.1)
      };

      // The DNSSEC status of the MX RRset that named the host now being
      // considered for delivery (RFC 7672 section 2.2).
      //
      // The values are assigned deliberately and must not be reordered.
      // Zero is the permissive value, because zero is what a caller
      // produces by accident: a default- or zero-initialized variable, a
      // value-initialized struct member, a memset() buffer. The one value
      // that can stop a delivery is the highest, so no accident and no
      // off-by-one lands on it. TlsPolicy.cpp static_asserts this.
      //
      // The numbering deliberately does NOT line up with
      // DnssecResolver::ChainStatus (Secure=0, Insecure=1, Bogus=2,
      // Indeterminate=3), so a caller that takes the shortcut of
      // static_cast-ing a ChainStatus into this type cannot produce Bogus
      // from a validation that failed: 0 becomes Unknown, 1 becomes Insecure,
      // 2 becomes Secure. Indeterminate becomes Bogus, which is what it means
      // here as well. EvaluateMxDnssecStatus is the only sanctioned conversion.
      enum class MxDnssecStatus
      {
         Unknown = 0,   // the caller did not determine the MX status, so the
                        // section 2.2 check cannot be performed. Preserves the
                        // earlier behaviour in which the TLSA lookup alone
                        // decides. This is what a caller that has not been
                        // taught to do a validated MX lookup gets, and it is
                        // what every caller gets today.
         Insecure = 1,  // the MX RRset is not covered by DNSSEC at all - which
                        // is true of most of the internet. The destination is
                        // not DANE-capable; delivery proceeds exactly as it
                        // would with no TLSA records published. Never a
                        // deferral and never a bounce.
         Secure = 2,    // the MX RRset was DNSSEC-validated and named this host,
                        // so DANE genuinely applies to it
         Bogus = 3      // the host must not be used: DNSSEC is published for
                        // the MX RRset and failed to validate, or the lookup was
                        // indeterminate (RFC 7672 section 2.1.1), or the RRset
                        // validated and does not name this host (section
                        // 2.2.1). Handled like a bogus TLSA lookup - the host is
                        // not used, which defers the delivery if no host
                        // remains. The only value here that can hold up mail,
                        // and reachable only when a caller passes it explicitly.
      };

      // Derives the value above for one candidate MX host from the result of
      // a DNSSEC-validated MX lookup for the recipient domain.
      // validated_mx_hosts are the host names that lookup returned, and are
      // only consulted when mx_chain_status is Secure: a validated MX RRset
      // makes this particular host DANE-capable only if the RRset is where
      // the host name came from, and a host it does not name is not used
      // (Bogus) - mail goes to the hosts the domain signed for, or waits. An
      // Indeterminate lookup is Bogus too. A function of its arguments
      // (DaneTester).
      static MxDnssecStatus EvaluateMxDnssecStatus(const String &host_name,
                                                   DnssecResolver::ChainStatus mx_chain_status,
                                                   const std::vector<String> &validated_mx_hosts);

      // Returns the usable TLSA records - DANE-TA (usage 2) and DANE-EE
      // (usage 3), DaneVerifier::IsUsable - published for the given MX host
      // and port. An empty result means no usable records exist; status
      // describes the DNSSEC validation outcome, and is Unusable when a
      // validated RRset was there and nothing in it could be used. mx_status carries
      // the RFC 7672 section 2.2 status of the MX RRset that named the host;
      // it is trailing and defaulted so that a caller which cannot yet
      // supply it keeps the previous behaviour rather than losing DANE.
      // The default is both the permissive value and the zero value of the
      // enum, so forgetting the argument, zero-initializing it or casting
      // the wrong enum into it all give the same fail-open result.
      static std::vector<TlsaRecord> GetTlsaRecords(const String &host_name, int port, TlsaLookupStatus &status,
                                                    MxDnssecStatus mx_status = MxDnssecStatus::Unknown);

      // What a DNSSEC-validated (secure) TLSA RRset means for the connection
      // (RFC 7672 section 2.2), with usable set to the records in it this
      // server can authenticate a host with: DnssecValidated when there is
      // one, Unusable when the RRset holds records and none of them is - TLS
      // is still required, without authentication - and NoRecords when it is
      // empty. A function of its argument alone (DaneTester).
      static TlsaLookupStatus ClassifyValidatedTlsa(const std::vector<TlsaRecord> &records, std::vector<TlsaRecord> &usable);

      // The sentence a delivery to host is deferred with - 450 4.7.0, kept in
      // the queue and retried - when what the destination published requires
      // TLS of it and TLS could not be had: no STARTTLS, a refused one, a
      // certificate that does not match. daneRecords: the host's usable TLSA
      // records (RFC 7672 section 2.2); daneUnusable: a validated TLSA RRset of
      // which none is usable, which requires TLS without authentication;
      // stsEnforced: recipientDomain's MTA-STS policy in enforce mode (RFC 8461
      // section 5, which forbids failing permanently before the policy has been
      // looked up again). DANE before MTA-STS, as RFC 8461 section 2 ranks them.
      // Empty when none applies. Until 6.3.6 only daneUnusable deferred; the
      // other two were bounced at once by the permanent 5.7.0 of a missing
      // STARTTLS. A function of its arguments (DaneTester).
      static String PublishedTlsDeferral(const String &host, const String &recipientDomain, bool daneRecords, bool daneUnusable, bool stsEnforced);

      // Whether a connection whose TLS a validated TLSA RRset of no usable
      // record requires (daneUnusable; RFC 7672 section 2.2 asks for TLS and
      // not for authentication) has its certificate check waived, as
      // opportunistic STARTTLS's is. Never when anything else requires TLS: an
      // MTA-STS policy in enforce mode, a remote domain policy, a rule - or
      // this server's own connection security, the administrator's or a
      // route's, at SSL or STARTTLS (required), where VerifyRemoteSslCertificate
      // decides as it always has. 6.3.6 before its release waived it there
      // too. A function of its arguments (DaneTester).
      static bool WaivesCertificateCheck(bool daneUnusable, bool stsEnforced, bool policyRequiresTls, bool ruleRequiresTls,
                                         ConnectionSecurity connectionSecurity);

      // The two network steps of the lookups above, each bounded by its deadline:
      // the policy fetch (https://host + path, 15 seconds for all of it, the body
      // only for a 200, error saying why not otherwise; a host that is not a DNS
      // name, or that leads to an address that is not public, is refused) and one
      // DNS query over UDP to one server (5 seconds for the reply). Public so that
      // network-deadline-tests can hold each to its deadline against a peer that
      // never answers - on Linux both waited for good until 6.3.6 - and the fetch
      // to the addresses it may reach, rather than trusting that a constant is used.
      static bool HttpsGet_(const String &host, const AnsiString &path, AnsiString &response_body, String &error);
      static bool RunDnsQuery_(const sockaddr_in &server, const AnsiString &name, unsigned short query_type, std::vector<unsigned char> &response);

   private:

      struct CachedStsPolicy
      {
         StsPolicy policy;
         AnsiString id;
         time_t expires_at = 0;
         time_t revalidate_at = 0;
      };

      static bool LookupStsDnsRecord_(const String &domain, AnsiString &id);
      static bool FetchStsPolicy_(const String &domain, StsPolicy &policy, int &max_age);
      static bool ParseStsPolicyBody_(const AnsiString &body, StsPolicy &policy, int &max_age);

      static bool GetDnsServers_(std::vector<sockaddr_in> &servers);
      static bool ParseTlsaResponse_(const std::vector<unsigned char> &response, std::vector<TlsaRecord> &records);
      static bool SkipDnsName_(const std::vector<unsigned char> &data, size_t &offset);
      static std::vector<TlsaRecord> LookupTlsaOpportunistic_(const String &host_name, int port);
      static std::vector<TlsaRecord> FilterUsableTlsaRecords_(const std::vector<TlsaRecord> &records);
      static bool MxRrsetContainsHost_(const String &host_name, const std::vector<String> &mx_host_names);

      static boost::recursive_mutex sts_cache_mutex_;
      static std::map<String, CachedStsPolicy> sts_cache_;
   };

   // DANE as the outbound connection does it, in memory: which TLSA records are
   // usable (DaneVerifier::IsUsable), what a DNSSEC-validated RRset means for the
   // connection (TlsPolicy::ClassifyValidatedTlsa - TLS required without
   // authentication when none of its records is usable, RFC 7672 section 2.2),
   // how many records a connection is armed with (DaneVerifier::Arm), and TLS
   // handshakes over a BIO pair against certificates made for the purpose:
   // DANE-TA (2 1 1) authenticating a server whose chain carries the anchor, and
   // refusing another anchor, an anchor the server did not send, a certificate
   // for another name or out of date; DANE-EE (3 1 1) authenticating a
   // self-signed certificate for another name, and refusing another key. Until
   // 6.3.6 only DANE-EE was used, and an RRset without it was taken for none.
   // And what the validated MX lookup makes of each host (EvaluateMxDnssecStatus:
   // a host the secure RRset does not name, and any host after an indeterminate
   // lookup, is not used), and whose TLS waives the certificate check
   // (WaivesCertificateCheck: never the server's own SSL or STARTTLS (required)).
   // No database, configuration, file or network. Runs inside the service with
   // the other class tests (ClassTester) and on Linux as dane-tests. Throws
   // std::logic_error naming the first thing that is wrong.
   class DaneTester
   {
   public:
      void Test();
   };
}
