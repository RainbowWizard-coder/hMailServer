// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Outbound TLS policy support (MTA-STS, DANE TLSA). See TlsPolicy.h.

#include "stdafx.h"

#include "TlsPolicy.h"

#include "../Common/TCPIP/DNSResolver.h"
#include "../Common/TCPIP/DnssecResolver.h"
#include "../Common/TCPIP/SocketConstants.h"
#include "../Common/Util/HttpsClient.h"
#include "../Common/Util/TlsRptStore.h"

// <ws2tcpip.h> is Winsock's TCP/IP header. Everything this file takes from it -
// the address structures and the address-conversion calls - comes from
// <netinet/in.h>, <arpa/inet.h> and <netdb.h> on POSIX, which the platform layer
// has already included.
#ifdef _MSC_VER
#include <ws2tcpip.h>
#endif
// <iphlpapi.h> declares GetAdaptersAddresses, which is how the nameserver list
// is discovered on Windows. There is no POSIX counterpart; see GetDnsServers_
// below.
#ifdef _MSC_VER
#include <iphlpapi.h>
#endif
// The POSIX nameserver list is read out of /etc/resolv.conf in GetDnsServers_
// below; these are the two standard headers that reading a text file and
// comparing a keyword in it need.
#ifdef HM_PLATFORM_POSIX
#include <cstdio>
#include <cstring>
#endif

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <ctime>
#include <stdexcept>
#include <string>

#ifdef _MSC_VER
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   boost::recursive_mutex TlsPolicy::sts_cache_mutex_;
   std::map<String, TlsPolicy::CachedStsPolicy> TlsPolicy::sts_cache_;

   namespace
   {
      const size_t MaxPolicyBodySize = 64 * 1024;
      const int MaxStsMaxAge = 31557600;     // RFC 8461: max one year.
      const int MinStsCacheSeconds = 300;    // Lower bound for our cache entries.
      const int RevalidateIntervalSeconds = 3600;
      const int NegativeCacheSeconds = 1800; // Cache "no policy" results for 30 minutes.
      const int DnsTimeoutMilliseconds = 5000;
      // The MTA-STS policy fetch, all of it: the lookup, the connection, the
      // handshake and the answer (HttpsClient's deadline).
      const int HttpsTimeoutSeconds = 15;
      const unsigned short DnsTypeTlsa = 52;
      const size_t MaxTlsaRecords = 8;

      // Whether the policy host is a DNS name and nothing more: letters, digits,
      // hyphens, dots and the underscore some names carry. It is
      // mta-sts.<the recipient's domain>, and it is put into a URL, where a colon
      // would name another port, a slash another path and an @ another host; RFC
      // 8461 section 3.3 has the policy fetched from that name on port 443 and
      // nowhere else. The client this replaced resolved the whole text as a name,
      // which such a host never was.
      bool IsPolicyHostName_(const String &host)
      {
         if (host.IsEmpty() || host.GetLength() > 253)
            return false;

         for (int i = 0; i < host.GetLength(); i++)
         {
            const auto c = host[i];
            const bool letterOrDigit = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            if (!letterOrDigit && c != '-' && c != '.' && c != '_')
               return false;
         }

         return true;
      }

      // Everything that is not one of the four enumerators - a cast from
      // another enum, an uninitialized struct member, an out-of-range
      // integer - is treated as Unknown, which means "behave exactly as we
      // did before the section 2.2 check existed". The restrictive branches
      // are then only reachable when a caller passed that exact value.
      TlsPolicy::MxDnssecStatus NormalizeMxDnssecStatus(TlsPolicy::MxDnssecStatus status)
      {
         switch (status)
         {
         case TlsPolicy::MxDnssecStatus::Unknown:
         case TlsPolicy::MxDnssecStatus::Insecure:
         case TlsPolicy::MxDnssecStatus::Secure:
         case TlsPolicy::MxDnssecStatus::Bogus:
            return status;
         }

         return TlsPolicy::MxDnssecStatus::Unknown;
      }
   }

   // RFC 7672 section 2.2 status handling is fail-open by construction.
   // These guards are here so that a later edit to the enum cannot quietly
   // turn a mistyped or default-initialized status into held-up mail.
   static_assert(static_cast<int>(TlsPolicy::MxDnssecStatus::Unknown) == 0,
                 "The zero value of MxDnssecStatus must be the permissive one: a default- or "
                 "zero-initialized status must proceed with delivery, never refuse a host.");
   static_assert(static_cast<int>(TlsPolicy::MxDnssecStatus::Bogus) != 0,
                 "Bogus is the only MxDnssecStatus that can hold up mail; it must never be the "
                 "value a caller gets by accident.");
   static_assert(static_cast<int>(TlsPolicy::MxDnssecStatus::Bogus) >
                    static_cast<int>(TlsPolicy::MxDnssecStatus::Secure) &&
                 static_cast<int>(TlsPolicy::MxDnssecStatus::Bogus) >
                    static_cast<int>(TlsPolicy::MxDnssecStatus::Insecure),
                 "Bogus must be the highest MxDnssecStatus so that no off-by-one or out-of-range "
                 "value can land on the one branch that refuses a host.");
   static_assert(static_cast<int>(DnssecResolver::ChainStatus::Secure) !=
                    static_cast<int>(TlsPolicy::MxDnssecStatus::Bogus) &&
                 static_cast<int>(DnssecResolver::ChainStatus::Insecure) !=
                    static_cast<int>(TlsPolicy::MxDnssecStatus::Bogus) &&
                 static_cast<int>(DnssecResolver::ChainStatus::Bogus) !=
                    static_cast<int>(TlsPolicy::MxDnssecStatus::Bogus),
                 "No DnssecResolver::ChainStatus value may cast onto MxDnssecStatus::Bogus: a "
                 "caller that skips EvaluateMxDnssecStatus and casts must still fail open.");

   TlsPolicy::StsPolicy
   TlsPolicy::GetStsPolicy(const String &domain)
   {
      String cacheKey = domain;
      cacheKey.MakeLower();

      time_t now = time(nullptr);

      // Cache access is done under the lock; network operations (DNS and
      // HTTPS) are always performed outside of it so parallel delivery
      // threads are not serialized behind slow lookups.
      bool haveCachedPolicy = false;
      AnsiString cachedId;
      StsPolicy cachedPolicy;

      {
         boost::lock_guard<boost::recursive_mutex> guard(sts_cache_mutex_);

         auto iter = sts_cache_.find(cacheKey);
         if (iter != sts_cache_.end())
         {
            CachedStsPolicy &cached = iter->second;

            if (now >= cached.expires_at)
            {
               sts_cache_.erase(iter);
            }
            else if (now < cached.revalidate_at)
            {
               return cached.policy;
            }
            else
            {
               // Due for revalidation. Bump the revalidation time so that
               // only this thread performs it; remember a snapshot to work
               // with outside the lock.
               cached.revalidate_at = now + RevalidateIntervalSeconds;

               haveCachedPolicy = true;
               cachedId = cached.id;
               cachedPolicy = cached.policy;
            }
         }
      }

      if (haveCachedPolicy)
      {
         // Check whether the policy id in DNS has changed. On DNS failure
         // or unchanged id, keep using the cached policy (RFC 8461 5.1).
         AnsiString currentId;
         if (!LookupStsDnsRecord_(cacheKey, currentId) || currentId == cachedId)
            return cachedPolicy;

         StsPolicy updatedPolicy;
         int maxAge = 0;
         if (!FetchStsPolicy_(cacheKey, updatedPolicy, maxAge))
            return cachedPolicy; // Fetch failed - continue using cached policy.

         CachedStsPolicy updatedEntry;
         updatedEntry.policy = updatedPolicy;
         updatedEntry.id = currentId;
         updatedEntry.expires_at = now + maxAge;
         updatedEntry.revalidate_at = now + RevalidateIntervalSeconds;

         boost::lock_guard<boost::recursive_mutex> guard(sts_cache_mutex_);
         sts_cache_[cacheKey] = updatedEntry;
         return updatedPolicy;
      }

      // No cached policy. Look for a policy advertisement in DNS.
      CachedStsPolicy newEntry;
      newEntry.revalidate_at = now + RevalidateIntervalSeconds;

      AnsiString policyId;
      if (!LookupStsDnsRecord_(cacheKey, policyId))
      {
         // No MTA-STS record published. Negative-cache the result.
         newEntry.expires_at = now + NegativeCacheSeconds;

         boost::lock_guard<boost::recursive_mutex> guard(sts_cache_mutex_);
         sts_cache_[cacheKey] = newEntry;
         return newEntry.policy;
      }

      StsPolicy fetchedPolicy;
      int maxAge = 0;
      if (!FetchStsPolicy_(cacheKey, fetchedPolicy, maxAge))
      {
         // Policy advertised but not retrievable. Per RFC 8461 this is a
         // potential downgrade attack, but with no previously cached policy
         // we have nothing to enforce. Negative-cache briefly and log.
         String message;
         message.Format(_T("MTA-STS: Domain %s advertises a policy but the policy file could not be fetched."), String(cacheKey).c_str());
         LOG_SMTP_CLIENT(0, "", message);

         TlsRptStore::Instance()->RecordFailure(cacheKey, "sts", "", "sts-policy-fetch-error", "");

         newEntry.expires_at = now + MinStsCacheSeconds;

         boost::lock_guard<boost::recursive_mutex> guard(sts_cache_mutex_);
         sts_cache_[cacheKey] = newEntry;
         return newEntry.policy;
      }

      newEntry.policy = fetchedPolicy;
      newEntry.id = policyId;
      newEntry.expires_at = now + maxAge;

      boost::lock_guard<boost::recursive_mutex> guard(sts_cache_mutex_);
      sts_cache_[cacheKey] = newEntry;

      return fetchedPolicy;
   }

   bool
   TlsPolicy::LookupStsDnsRecord_(const String &domain, AnsiString &id)
   {
      DNSResolver resolver;

      std::vector<String> txtRecords;
      if (!resolver.GetTXTRecords("_mta-sts." + domain, txtRecords))
         return false;

      for (const String &record : txtRecords)
      {
         AnsiString narrow = record;
         narrow.Trim();

         if (!narrow.StartsWith("v=STSv1"))
            continue;

         // Extract the id= field.
         std::vector<AnsiString> parts = StringParser::SplitString(narrow, ";");
         for (AnsiString part : parts)
         {
            part.Trim();
            if (part.StartsWith("id="))
            {
               id = part.Mid(3);
               id.Trim();
               return !id.IsEmpty();
            }
         }
      }

      return false;
   }

   bool
   TlsPolicy::FetchStsPolicy_(const String &domain, StsPolicy &policy, int &max_age)
   {
      max_age = 0;

      AnsiString body;
      String error;
      if (!HttpsGet_("mta-sts." + domain, "/.well-known/mta-sts.txt", body, error))
      {
         LOG_DEBUG("MTA-STS: No policy for " + domain + ": " + error);
         return false;
      }

      if (!ParseStsPolicyBody_(body, policy, max_age))
         return false;

      if (max_age < MinStsCacheSeconds)
         max_age = MinStsCacheSeconds;
      if (max_age > MaxStsMaxAge)
         max_age = MaxStsMaxAge;

      return true;
   }

   bool
   TlsPolicy::ParseStsPolicyBody_(const AnsiString &body, StsPolicy &policy, int &max_age)
   {
      max_age = 0;

      bool versionSeen = false;

      std::vector<AnsiString> lines = StringParser::SplitString(body, "\n");
      for (AnsiString line : lines)
      {
         line.TrimRight("\r");
         line.Trim();

         if (line.IsEmpty())
            continue;

         int separatorPos = line.Find(":");
         if (separatorPos <= 0)
            continue;

         AnsiString key = line.Mid(0, separatorPos);
         AnsiString value = line.Mid(separatorPos + 1);
         key.Trim();
         value.Trim();
         key.MakeLower();

         if (key == "version")
         {
            if (value != "STSv1")
               return false;
            versionSeen = true;
         }
         else if (key == "mode")
         {
            value.MakeLower();
            if (value == "enforce")
               policy.mode = StsEnforce;
            else if (value == "testing")
               policy.mode = StsTesting;
            else
               policy.mode = StsNone;
         }
         else if (key == "mx")
         {
            if (!value.IsEmpty())
               policy.mx_patterns.push_back(String(value));
         }
         else if (key == "max_age")
         {
            if (StringParser::IsNumeric(value))
               max_age = atoi(value);
         }
      }

      if (!versionSeen)
         return false;

      if (policy.mode == StsEnforce && policy.mx_patterns.empty())
      {
         // An enforce policy without mx patterns is invalid - treat as none.
         policy.mode = StsNone;
         return false;
      }

      return true;
   }

   bool
   TlsPolicy::HostMatchesStsPolicy(const String &host_name, const StsPolicy &policy)
   {
      String host = host_name;
      host.MakeLower();

      for (const String &patternConst : policy.mx_patterns)
      {
         String pattern = patternConst;
         pattern.MakeLower();

         if (pattern == host)
            return true;

         if (pattern.StartsWith(_T("*.")))
         {
            // The wildcard matches exactly one leftmost label.
            String suffix = pattern.Mid(1); // ".example.com"

            if (host.GetLength() <= suffix.GetLength())
               continue;

            if (host.Right(suffix.GetLength()) != suffix)
               continue;

            String prefix = host.Mid(0, host.GetLength() - suffix.GetLength());
            if (!prefix.IsEmpty() && prefix.Find(_T(".")) < 0)
               return true;
         }
      }

      return false;
   }

   bool
   TlsPolicy::HttpsGet_(const String &host, const AnsiString &path, AnsiString &response_body, String &error)
   {
      if (!IsPolicyHostName_(host))
      {
         error = _T("The policy host is not a host name.");
         return false;
      }

      // Through HttpsClient, whose deadline bounds the whole fetch - the name
      // lookup, the connection, the handshake, the request and the answer to its
      // end - on every platform. The synchronous client this replaces handed
      // SO_RCVTIMEO a DWORD, which Linux refuses (it takes a timeval), and Asio's
      // synchronous calls on POSIX wait with no timeout anyway, so on Linux a
      // policy host that took the connection and never answered held the delivery
      // thread for as long as it liked. What RFC 8461 section 3.3 asks of the fetch
      // is HttpsClient's too: the certificate is verified against the machine's
      // store with the host name checked, SNI is sent, and no redirect is followed
      // (a 3xx is not a 200, so there is no policy). An answer longer than
      // MaxPolicyBodySize is no policy either, where the old client parsed what
      // it had read by then.
      //
      // Held to public addresses (RequestPublicThroughProxy): the policy host is
      // whatever the recipient's domain names, and a fetch made for a stranger's
      // domain must not be a way to this server's own network - loopback, the
      // private ranges, link-local and the cloud metadata addresses are refused,
      // judged by the addresses the name resolves to, and the connection is made
      // to those addresses. HttpProxy carries it when one is set, asked for a
      // tunnel to an address that was judged rather than to the name.
      //
      // Accept: */* because HttpsClient asks for JSON unless told otherwise, and the
      // policy is text/plain; the old client named no type at all.
      HttpsClient::Response response;
      const std::vector<AnsiString> headers(1, AnsiString("Accept: */*"));
      if (!HttpsClient::RequestPublicThroughProxy("GET", AnsiString("https://") + AnsiString(host) + path, headers, "", "",
                                                  response, error, HttpsTimeoutSeconds, MaxPolicyBodySize))
         return false;

      if (response.status_code != 200)
      {
         error.Format(_T("%s answered the policy request with HTTP %d."), host.c_str(), response.status_code);
         return false;
      }

      response_body = response.body;
      return true;
   }

   TlsPolicy::MxDnssecStatus
   TlsPolicy::EvaluateMxDnssecStatus(const String &host_name, DnssecResolver::ChainStatus mx_chain_status,
                                     const std::vector<String> &validated_mx_hosts)
   {
      switch (mx_chain_status)
      {
      case DnssecResolver::ChainStatus::Bogus:
         // The only status this function returns that can hold up mail.
         // Note what cannot reach it: DnssecResolver reports a transport
         // failure, a timeout, an unsigned delegation, an absent RRset and an
         // unsupported DS digest as Insecure, not Bogus, so a resolver
         // problem or a plain non-DNSSEC destination never gets here.
         return MxDnssecStatus::Bogus;

      case DnssecResolver::ChainStatus::Insecure:
         // No usable DNSSEC chain over the MX RRset. This is the ordinary
         // state of most destinations and must remain an ordinary delivery.
         return MxDnssecStatus::Insecure;

      case DnssecResolver::ChainStatus::Secure:
         if (MxRrsetContainsHost_(host_name, validated_mx_hosts))
            return MxDnssecStatus::Secure;

         // The MX RRset validated, but the host we are about to contact is
         // not one of the names it published, so this host name did not come
         // from an authenticated source after all and its TLSA records mean
         // nothing (RFC 7672 section 2.2). Treat the destination as non-DANE
         // rather than refusing it: the mismatch can also arise from the
         // implicit-MX (A record) fallback or a CNAME-followed lookup, and
         // turning a resolver quirk into a stuck queue would be worse than
         // delivering with opportunistic TLS as we did before.
         //
         // LOG_DEBUG, not LOG_APPLICATION: implicit MX is an ordinary,
         // healthy configuration, so on a default install this would
         // otherwise write a line per delivery attempt about a condition
         // nobody needs to act on.
         LOG_DEBUG("DANE: MX host " + host_name + " is not named by the DNSSEC-validated MX RRset of the recipient domain. Treating the destination as non-DANE (RFC 7672 section 2.2).");
         return MxDnssecStatus::Insecure;
      }

      // Not reachable through the enumerators above, but a ChainStatus that
      // came from a cast or from future code lands here: fail open.
      return MxDnssecStatus::Insecure;
   }

   bool
   TlsPolicy::MxRrsetContainsHost_(const String &host_name, const std::vector<String> &mx_host_names)
   {
      String candidate = host_name;
      candidate.MakeLower();
      candidate.TrimRight(_T('.'));

      if (candidate.IsEmpty())
         return false;

      for (const String &publishedHost : mx_host_names)
      {
         String published = publishedHost;
         published.MakeLower();
         published.TrimRight(_T('.'));

         if (!published.IsEmpty() && published == candidate)
            return true;
      }

      return false;
   }

   std::vector<TlsaRecord>
   TlsPolicy::GetTlsaRecords(const String &host_name, int port, TlsaLookupStatus &status, MxDnssecStatus mx_status)
   {
      // Normalize before anything looks at the value, so that only a
      // deliberately supplied Bogus can reach the branch that refuses a host.
      mx_status = NormalizeMxDnssecStatus(mx_status);

      if (IniFileSettings::Instance()->GetDnssecValidationEnabled())
      {
         // RFC 7672 section 2.2: a TLSA record is only evidence about the
         // intended server if the MX RRset that named that server was itself
         // DNSSEC-validated. Otherwise an attacker who can forge the MX
         // response simply names a host he controls, whose TLSA records
         // validate perfectly, and DANE reports success.
         //
         // With DnssecValidationEnabled=0 the whole DANE path is
         // unvalidated by definition, so the MX status is not consulted.
         switch (mx_status)
         {
         case MxDnssecStatus::Insecure:
            // The overwhelming majority of destinations publish no DNSSEC
            // over their MX RRset. They are not DANE-capable: behave exactly
            // as if no TLSA records existed, which means ordinary
            // opportunistic TLS delivery - no deferral and no bounce. The
            // TLSA lookup is not even attempted, so a bogus TLSA chain under
            // an unsigned MX name cannot hold up mail either.
            LOG_DEBUG("DANE: The MX RRset naming " + host_name + " is not DNSSEC-signed. Treating the destination as non-DANE (RFC 7672 section 2.2).");
            status = TlsaLookupStatus::NoRecords;
            return std::vector<TlsaRecord>();

         case MxDnssecStatus::Bogus:
            // DNSSEC is published over the MX RRset and failed to validate,
            // so the host names themselves are forged or the zone is broken.
            // Handled exactly like a bogus TLSA lookup: this host is not
            // used, and if that leaves no usable host the caller defers the
            // delivery rather than bouncing it. This is the only branch in
            // this function that can hold up a message, which is why it is
            // reachable only from an explicitly supplied MxDnssecStatus::Bogus.
            LOG_APPLICATION("DANE: The MX RRset naming " + host_name + " failed DNSSEC validation. Not using this host (RFC 7672 section 2.2).");
            status = TlsaLookupStatus::Bogus;
            return std::vector<TlsaRecord>();

         case MxDnssecStatus::Secure:
            // The section 2.2 precondition is met. Fall through to the TLSA
            // lookup, whose result now genuinely applies to this host.
            break;

         case MxDnssecStatus::Unknown:
            // Deliberately kept out of the Secure branch above: "we never
            // checked" must never be a synonym for "we checked and it
            // passed", and a future edit that tightens the Secure branch
            // must not silently tighten this one too. Fall through to the
            // TLSA lookup, which is exactly what happened before the
            // section 2.2 check existed.
            break;
         }

         DnssecResolver resolver;

         std::vector<TlsaRecord> rawRecords;
         DnssecResolver::ChainStatus chainStatus = resolver.QueryTlsa(host_name, port, rawRecords);

         switch (chainStatus)
         {
         case DnssecResolver::ChainStatus::Secure:
            {
               std::vector<TlsaRecord> usable;
               status = ClassifyValidatedTlsa(rawRecords, usable);

               if (status == TlsaLookupStatus::DnssecValidated && mx_status == MxDnssecStatus::Unknown)
               {
                  // The caller did not supply the MX RRset's DNSSEC status, so
                  // the section 2.2 check could not be completed. Enforce the
                  // TLSA records as before rather than silently dropping DANE,
                  // and say why in the log.
                  LOG_DEBUG("DANE: Enforcing TLSA records for " + host_name + " without knowing whether the MX RRset that named it was DNSSEC-validated (RFC 7672 section 2.2).");
               }

               if (status == TlsaLookupStatus::Unusable)
                  LOG_DEBUG("DANE: The DNSSEC-validated TLSA records of " + host_name + " include none this server can use. TLS is required, without authentication (RFC 7672 section 2.2).");

               return usable;
            }

         case DnssecResolver::ChainStatus::Bogus:
            status = TlsaLookupStatus::Bogus;
            return std::vector<TlsaRecord>();

         case DnssecResolver::ChainStatus::Insecure:
         default:
            status = TlsaLookupStatus::NoRecords;
            return std::vector<TlsaRecord>();
         }
      }

      // Legacy opportunistic mode (DnssecValidationEnabled=0).
      std::vector<TlsaRecord> records = LookupTlsaOpportunistic_(host_name, port);
      status = records.empty() ? TlsaLookupStatus::NoRecords : TlsaLookupStatus::Unvalidated;
      return records;
   }

   String
   TlsPolicy::PublishedTlsDeferral(const String &host, const String &recipientDomain, bool daneRecords, bool daneUnusable, bool stsEnforced)
   {
      String reason;

      if (daneRecords)
      {
         reason.Format(_T("Delivery deferred: the TLSA records of %s require TLS to it, with a certificate they match (RFC 7672), and that could not be established. The message has NOT been sent in the clear; it will be retried."),
            host.c_str());
      }
      else if (stsEnforced)
      {
         reason.Format(_T("Delivery deferred: the MTA-STS policy of %s requires TLS to %s, with a certificate that verifies (RFC 8461), and that could not be established. The message has NOT been sent in the clear; it will be retried."),
            recipientDomain.c_str(), host.c_str());
      }
      else if (daneUnusable)
      {
         reason.Format(_T("Delivery deferred: the DNSSEC-validated TLSA records of %s require TLS to it (RFC 7672), and it could not be established. The message has NOT been sent in the clear; it will be retried."),
            host.c_str());
      }

      return reason;
   }

   TlsPolicy::TlsaLookupStatus
   TlsPolicy::ClassifyValidatedTlsa(const std::vector<TlsaRecord> &records, std::vector<TlsaRecord> &usable)
   {
      usable = FilterUsableTlsaRecords_(records);

      if (!usable.empty())
         return TlsaLookupStatus::DnssecValidated;

      // RFC 7672 section 2.2: a secure, non-empty RRset none of whose records
      // is usable still says the host does TLS, and any connection to it MUST
      // be made over TLS, unauthenticated. Until 6.3.6 it was taken for no
      // records at all, and an attacker who took the STARTTLS offer away had
      // the message in the clear - for every domain publishing DANE-TA records
      // only, which this server did not use before 6.3.6 either.
      return records.empty() ? TlsaLookupStatus::NoRecords : TlsaLookupStatus::Unusable;
   }

   std::vector<TlsaRecord>
   TlsPolicy::FilterUsableTlsaRecords_(const std::vector<TlsaRecord> &records)
   {
      // Keep only records we can act on: DANE-TA (usage 2) and DANE-EE
      // (usage 3) with a supported selector and matching type and data
      // OpenSSL takes for them (DaneVerifier::IsUsable). Until 6.3.6, DANE-EE
      // alone.
      std::vector<TlsaRecord> result;

      for (const TlsaRecord &record : records)
      {
         if (!DaneVerifier::IsUsable(record))
            continue;

         result.push_back(record);

         if (result.size() >= MaxTlsaRecords)
            break;
      }

      return result;
   }

   std::vector<TlsaRecord>
   TlsPolicy::LookupTlsaOpportunistic_(const String &host_name, int port)
   {
      std::vector<TlsaRecord> result;

      AnsiString queryName;
      AnsiString narrowHost = host_name;
      narrowHost.MakeLower();
      queryName.Format("_%d._tcp.%hs", port, narrowHost.c_str());

      std::vector<sockaddr_in> dnsServers;
      if (!GetDnsServers_(dnsServers))
         return result;

      for (const sockaddr_in &server : dnsServers)
      {
         std::vector<unsigned char> response;
         if (!RunDnsQuery_(server, queryName, DnsTypeTlsa, response))
            continue;

         std::vector<TlsaRecord> records;
         if (!ParseTlsaResponse_(response, records))
            continue;

         result = FilterUsableTlsaRecords_(records);
         return result;
      }

      return result;
   }

   bool
   TlsPolicy::GetDnsServers_(std::vector<sockaddr_in> &servers)
   {
      // If an explicit DNS server is configured in hMailServer.ini, use it.
      String configuredServer = IniFileSettings::Instance()->GetDNSServer();
      if (!configuredServer.IsEmpty())
      {
         AnsiString narrow = configuredServer;

         sockaddr_in address = {};
         address.sin_family = AF_INET;
         address.sin_port = htons(53);

         if (inet_pton(AF_INET, narrow.c_str(), &address.sin_addr) == 1)
         {
            servers.push_back(address);
            return true;
         }
      }

      // Otherwise, pick up the system-configured DNS servers.
#ifdef _MSC_VER
      ULONG bufferSize = 16 * 1024;
      std::vector<unsigned char> buffer(bufferSize);

      ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_FRIENDLY_NAME | GAA_FLAG_SKIP_UNICAST;

      ULONG queryResult = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bufferSize);

      if (queryResult == ERROR_BUFFER_OVERFLOW)
      {
         buffer.resize(bufferSize);
         queryResult = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bufferSize);
      }

      if (queryResult != NO_ERROR)
         return false;

      for (IP_ADAPTER_ADDRESSES *adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
           adapter != nullptr;
           adapter = adapter->Next)
      {
         if (adapter->OperStatus != IfOperStatusUp)
            continue;

         for (IP_ADAPTER_DNS_SERVER_ADDRESS *dns = adapter->FirstDnsServerAddress;
              dns != nullptr;
              dns = dns->Next)
         {
            if (dns->Address.lpSockaddr == nullptr || dns->Address.lpSockaddr->sa_family != AF_INET)
               continue;

            sockaddr_in address = {};
            memcpy(&address, dns->Address.lpSockaddr, sizeof(sockaddr_in));
            address.sin_port = htons(53);

            servers.push_back(address);

            if (servers.size() >= 2)
               return true;
         }
      }
#else
      // /etc/resolv.conf is where a POSIX system records the nameservers it uses,
      // so reading it is the counterpart of GetAdaptersAddresses above and answers
      // with the same thing: the first two IPv4 servers, on port 53, in the order
      // the file lists them. Leaving it unwritten would have been worse than it
      // looks - MTA-STS and DANE would have found no server, read no TLSA record
      // and delivered without either, on a build that reported no error at all.
      FILE *resolv_conf = ::fopen("/etc/resolv.conf", "r");

      if (resolv_conf != nullptr)
      {
         char line[512];

         while (::fgets(line, sizeof(line), resolv_conf) != nullptr)
         {
            const char *cursor = line;

            while (*cursor == ' ' || *cursor == '\t')
               cursor++;

            // "nameserver" has to be the whole keyword rather than the start of a
            // longer one, so what follows it must be white space.
            if (::strncmp(cursor, "nameserver", 10) != 0)
               continue;

            cursor += 10;

            if (*cursor != ' ' && *cursor != '\t')
               continue;

            while (*cursor == ' ' || *cursor == '\t')
               cursor++;

            char address_text[64];
            size_t length = 0;

            while (length + 1 < sizeof(address_text) &&
                   *cursor != '\0' && *cursor != ' ' && *cursor != '\t' &&
                   *cursor != '\r' && *cursor != '\n' && *cursor != '#' && *cursor != ';')
            {
               address_text[length] = *cursor;
               length++;
               cursor++;
            }

            address_text[length] = '\0';

            sockaddr_in address = {};
            address.sin_family = AF_INET;
            address.sin_port = htons(53);

            // An IPv6 nameserver is passed over here, exactly as the Windows branch
            // passes over a server whose family is not AF_INET.
            if (inet_pton(AF_INET, address_text, &address.sin_addr) != 1)
               continue;

            servers.push_back(address);

            if (servers.size() >= 2)
               break;
         }

         ::fclose(resolv_conf);
      }
#endif

      return !servers.empty();
   }

   bool
   TlsPolicy::RunDnsQuery_(const sockaddr_in &server, const AnsiString &name, unsigned short query_type, std::vector<unsigned char> &response)
   {
      // Build the query packet.
      unsigned char transactionId[2];
      if (RAND_bytes(transactionId, sizeof(transactionId)) != 1)
      {
         transactionId[0] = static_cast<unsigned char>(GetTickCount() & 0xFF);
         transactionId[1] = static_cast<unsigned char>((GetTickCount() >> 8) & 0xFF);
      }

      std::vector<unsigned char> query;
      query.reserve(64 + name.GetLength());

      // Header
      query.push_back(transactionId[0]);
      query.push_back(transactionId[1]);
      query.push_back(0x01); // RD
      query.push_back(0x00);
      query.push_back(0x00); query.push_back(0x01); // QDCOUNT 1
      query.push_back(0x00); query.push_back(0x00); // ANCOUNT 0
      query.push_back(0x00); query.push_back(0x00); // NSCOUNT 0
      query.push_back(0x00); query.push_back(0x01); // ARCOUNT 1 (EDNS0 OPT)

      // Question name
      std::vector<AnsiString> labels = StringParser::SplitString(name, ".");
      for (const AnsiString &label : labels)
      {
         if (label.IsEmpty() || label.GetLength() > 63)
            return false;

         query.push_back(static_cast<unsigned char>(label.GetLength()));
         for (int i = 0; i < label.GetLength(); i++)
            query.push_back(static_cast<unsigned char>(label[i]));
      }
      query.push_back(0x00);

      // QTYPE / QCLASS
      query.push_back(static_cast<unsigned char>(query_type >> 8));
      query.push_back(static_cast<unsigned char>(query_type & 0xFF));
      query.push_back(0x00); query.push_back(0x01); // IN

      // EDNS0 OPT pseudo-record: root name, type 41, class = UDP payload size 1232.
      query.push_back(0x00);                        // root
      query.push_back(0x00); query.push_back(41);   // TYPE OPT
      query.push_back(0x04); query.push_back(0xD0); // CLASS 1232
      query.push_back(0x00); query.push_back(0x00); query.push_back(0x00); query.push_back(0x00); // TTL
      query.push_back(0x00); query.push_back(0x00); // RDLEN 0

      SOCKET udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (udpSocket == INVALID_SOCKET)
         return false;

      // The deadline in the form each platform takes, as DnssecResolver gives it
      // for its own queries: Winsock a DWORD of milliseconds, POSIX a struct
      // timeval. Until 6.3.6 Linux was handed the DWORD too, refused it with
      // EINVAL, and a reply that never came held the delivery thread in recvfrom
      // for good.
#ifdef HM_PLATFORM_POSIX
      struct timeval timeout = {};
      timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(DnsTimeoutMilliseconds / 1000);
      timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(DnsTimeoutMilliseconds % 1000) * 1000;
#else
      const DWORD timeout = DnsTimeoutMilliseconds;
#endif
      setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
      setsockopt(udpSocket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

      bool succeeded = false;

      if (sendto(udpSocket, reinterpret_cast<const char*>(query.data()), static_cast<int>(query.size()), 0,
                 reinterpret_cast<const sockaddr*>(&server), sizeof(server)) == static_cast<int>(query.size()))
      {
         unsigned char receiveBuffer[4096];

         sockaddr_in fromAddress = {};
         // recvfrom's last argument is an int* on Winsock and a socklen_t* on
         // POSIX, and they are not the same type even where they are the same
         // width, so the variable is declared as whatever the platform's recvfrom
         // is going to write through. The Windows declaration is the one this file
         // has always had.
#ifdef HM_PLATFORM_POSIX
         socklen_t fromLength = sizeof(fromAddress);
#else
         int fromLength = sizeof(fromAddress);
#endif

         int bytesReceived = recvfrom(udpSocket, reinterpret_cast<char*>(receiveBuffer), sizeof(receiveBuffer), 0,
                                      reinterpret_cast<sockaddr*>(&fromAddress), &fromLength);

         if (bytesReceived >= 12 &&
             receiveBuffer[0] == transactionId[0] &&
             receiveBuffer[1] == transactionId[1] &&
             fromAddress.sin_addr.s_addr == server.sin_addr.s_addr)
         {
            response.assign(receiveBuffer, receiveBuffer + bytesReceived);
            succeeded = true;
         }
      }

      closesocket(udpSocket);
      return succeeded;
   }

   bool
   TlsPolicy::SkipDnsName_(const std::vector<unsigned char> &data, size_t &offset)
   {
      size_t safetyCounter = 0;

      while (offset < data.size())
      {
         if (++safetyCounter > 128)
            return false;

         unsigned char length = data[offset];

         if (length == 0)
         {
            offset += 1;
            return true;
         }

         if ((length & 0xC0) == 0xC0)
         {
            // Compression pointer: two bytes, then the name ends here.
            offset += 2;
            return offset <= data.size();
         }

         if (length > 63)
            return false;

         offset += static_cast<size_t>(length) + 1;
      }

      return false;
   }

   bool
   TlsPolicy::ParseTlsaResponse_(const std::vector<unsigned char> &response, std::vector<TlsaRecord> &records)
   {
      if (response.size() < 12)
         return false;

      unsigned char flags1 = response[2];
      unsigned char flags2 = response[3];

      bool isResponse = (flags1 & 0x80) != 0;
      bool truncated = (flags1 & 0x02) != 0;
      int responseCode = flags2 & 0x0F;

      if (!isResponse || responseCode != 0)
         return false;

      if (truncated)
      {
         LOG_DEBUG("DANE: TLSA response truncated; skipping DANE for this host.");
         return false;
      }

      size_t questionCount = (static_cast<size_t>(response[4]) << 8) | response[5];
      size_t answerCount = (static_cast<size_t>(response[6]) << 8) | response[7];

      size_t offset = 12;

      // Skip questions.
      for (size_t i = 0; i < questionCount; i++)
      {
         if (!SkipDnsName_(response, offset))
            return false;

         offset += 4; // QTYPE + QCLASS
         if (offset > response.size())
            return false;
      }

      // Parse answers.
      for (size_t i = 0; i < answerCount; i++)
      {
         if (!SkipDnsName_(response, offset))
            return false;

         if (offset + 10 > response.size())
            return false;

         unsigned short recordType = (static_cast<unsigned short>(response[offset]) << 8) | response[offset + 1];
         size_t rdataLength = (static_cast<size_t>(response[offset + 8]) << 8) | response[offset + 9];

         offset += 10;

         if (offset + rdataLength > response.size())
            return false;

         if (recordType == DnsTypeTlsa && rdataLength >= 4)
         {
            TlsaRecord record;
            record.usage = response[offset];
            record.selector = response[offset + 1];
            record.matching_type = response[offset + 2];
            record.data.assign(reinterpret_cast<const char*>(response.data() + offset + 3), rdataLength - 3);

            records.push_back(record);
         }

         offset += rdataLength;
      }

      return true;
   }
   // ---- the tester --------------------------------------------------------

   namespace
   {
      void ExpectDane_(bool condition, const std::string &message)
      {
         if (!condition)
         {
            ERR_clear_error();
            throw std::logic_error("DaneTester: " + message);
         }
      }

      struct KeyFree_ { void operator()(EVP_PKEY *key) const { EVP_PKEY_free(key); } };
      struct CertificateFree_ { void operator()(X509 *certificate) const { X509_free(certificate); } };
      typedef std::unique_ptr<EVP_PKEY, KeyFree_> TestKey_;
      typedef std::unique_ptr<X509, CertificateFree_> TestCertificate_;

      TestKey_ NewKey_()
      {
         TestKey_ key(EVP_EC_gen("prime256v1"));
         ExpectDane_(key != nullptr, "OpenSSL made no P-256 key");
         return key;
      }

      void AddExtension_(X509 *certificate, X509 *issuer, int nid, const char *value)
      {
         X509V3_CTX context;
         X509V3_set_ctx_nodb(&context);
         X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);

         X509_EXTENSION *extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value);
         ExpectDane_(extension != nullptr, std::string("the extension ") + value + " was not made");
         const bool added = X509_add_ext(certificate, extension, -1) == 1;
         X509_EXTENSION_free(extension);
         ExpectDane_(added, std::string("the extension ") + value + " was not added");
      }

      // A certificate for key, issued by issuer with issuer_key (itself when
      // issuer is null): an authority, or a server certificate naming dns_name,
      // valid from an hour ago for valid_days days (a negative number: expired).
      TestCertificate_ NewCertificate_(const char *common_name, EVP_PKEY *key, X509 *issuer, EVP_PKEY *issuer_key,
                                       bool authority, const char *dns_name, long valid_days)
      {
         TestCertificate_ certificate(X509_new());
         ExpectDane_(certificate != nullptr, "OpenSSL made no certificate");

         static long serial = 1;
         X509_set_version(certificate.get(), X509_VERSION_3);
         ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), serial++);

         // A name of its own, set as the subject: OpenSSL 4 hands back the
         // certificate's own as const.
         X509_NAME *name = X509_NAME_new();
         ExpectDane_(name != nullptr, "OpenSSL made no name");
         const bool named = X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>(common_name), -1, -1, 0) == 1 &&
                            X509_set_subject_name(certificate.get(), name) == 1 &&
                            X509_set_issuer_name(certificate.get(), issuer != nullptr ? X509_get_subject_name(issuer) : name) == 1;
         X509_NAME_free(name);
         ExpectDane_(named, std::string("the certificate ") + common_name + " was not named");

         if (valid_days >= 0)
         {
            X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -3600);
            X509_gmtime_adj(X509_getm_notAfter(certificate.get()), valid_days * 86400);
         }
         else
         {
            X509_gmtime_adj(X509_getm_notBefore(certificate.get()), (valid_days - 30) * 86400);
            X509_gmtime_adj(X509_getm_notAfter(certificate.get()), valid_days * 86400);
         }

         X509_set_pubkey(certificate.get(), key);

         X509 *signer = issuer != nullptr ? issuer : certificate.get();
         if (authority)
         {
            AddExtension_(certificate.get(), signer, NID_basic_constraints, "critical,CA:TRUE");
            AddExtension_(certificate.get(), signer, NID_key_usage, "critical,keyCertSign,cRLSign");
         }
         else
         {
            AddExtension_(certificate.get(), signer, NID_basic_constraints, "critical,CA:FALSE");
            AddExtension_(certificate.get(), signer, NID_ext_key_usage, "serverAuth");
            std::string alternative = std::string("DNS:") + dns_name;
            AddExtension_(certificate.get(), signer, NID_subject_alt_name, alternative.c_str());
         }

         ExpectDane_(X509_sign(certificate.get(), issuer_key != nullptr ? issuer_key : key, EVP_sha256()) > 0,
            std::string("the certificate ") + common_name + " was not signed");
         return certificate;
      }

      std::string Sha256_(const std::string &data)
      {
         unsigned char digest[EVP_MAX_MD_SIZE];
         unsigned int length = 0;
         ExpectDane_(EVP_Digest(data.data(), data.size(), digest, &length, EVP_sha256(), nullptr) == 1, "SHA-256 failed");
         return std::string(reinterpret_cast<const char *>(digest), length);
      }

      std::string Sha512_(const std::string &data)
      {
         unsigned char digest[EVP_MAX_MD_SIZE];
         unsigned int length = 0;
         ExpectDane_(EVP_Digest(data.data(), data.size(), digest, &length, EVP_sha512(), nullptr) == 1, "SHA-512 failed");
         return std::string(reinterpret_cast<const char *>(digest), length);
      }

      std::string CertificateDer_(X509 *certificate)
      {
         const int length = i2d_X509(certificate, nullptr);
         ExpectDane_(length > 0, "a certificate had no DER form");
         std::string der(static_cast<size_t>(length), '\0');
         unsigned char *cursor = reinterpret_cast<unsigned char *>(&der[0]);
         i2d_X509(certificate, &cursor);
         return der;
      }

      std::string SpkiDer_(EVP_PKEY *key)
      {
         const int length = i2d_PUBKEY(key, nullptr);
         ExpectDane_(length > 0, "a key had no SubjectPublicKeyInfo");
         std::string der(static_cast<size_t>(length), '\0');
         unsigned char *cursor = reinterpret_cast<unsigned char *>(&der[0]);
         i2d_PUBKEY(key, &cursor);
         return der;
      }

      TlsaRecord Record_(int usage, int selector, int matching_type, const std::string &data)
      {
         TlsaRecord record;
         record.usage = usage;
         record.selector = selector;
         record.matching_type = matching_type;
         record.data = data;
         return record;
      }

      // The verify callback a connection gets, through DaneVerifier itself.
      int VerifyThroughDaneVerifier_(int preverified, X509_STORE_CTX *store)
      {
         boost::asio::ssl::verify_context context(store);
         return DaneVerifier(0)(preverified != 0, context) ? 1 : 0;
      }

      // One TLS handshake in memory: a server presenting leaf and chain, a
      // client armed with records for host_name as TCPConnection arms an
      // outbound connection. True when the client finished it with the
      // certificate verified; usage and depth are those of the record that
      // authenticated it (SSL_get0_dane_tlsa), -1 when none did.
      bool Handshake_(X509 *leaf, EVP_PKEY *leaf_key, const std::vector<X509 *> &chain, const char *host_name,
                      const std::vector<TlsaRecord> &records, int &usage, int &depth)
      {
         usage = -1;
         depth = -1;

         SSL_CTX *serverContext = SSL_CTX_new(TLS_server_method());
         SSL_CTX *clientContext = SSL_CTX_new(TLS_client_method());
         ExpectDane_(serverContext != nullptr && clientContext != nullptr, "OpenSSL made no context");

         bool ready = SSL_CTX_use_certificate(serverContext, leaf) == 1 &&
                      SSL_CTX_use_PrivateKey(serverContext, leaf_key) == 1 &&
                      SSL_CTX_dane_enable(clientContext) > 0;
         for (X509 *certificate : chain)
            ready = ready && SSL_CTX_add1_chain_cert(serverContext, certificate) == 1;

         SSL *server = ready ? SSL_new(serverContext) : nullptr;
         SSL *client = ready ? SSL_new(clientContext) : nullptr;
         ready = ready && server != nullptr && client != nullptr;

         bool finished = false;
         if (ready)
         {
            SSL_set_verify(client, SSL_VERIFY_PEER, &VerifyThroughDaneVerifier_);

            if (DaneVerifier::Arm(client, host_name, records, 0) > 0)
            {
               BIO *clientEnd = nullptr;
               BIO *serverEnd = nullptr;
               ExpectDane_(BIO_new_bio_pair(&clientEnd, 0, &serverEnd, 0) == 1, "OpenSSL made no BIO pair");
               SSL_set_bio(client, clientEnd, clientEnd);
               SSL_set_bio(server, serverEnd, serverEnd);
               SSL_set_connect_state(client);
               SSL_set_accept_state(server);

               bool clientDone = false;
               bool serverDone = false;
               bool failed = false;
               for (int round = 0; round < 64 && !failed && !(clientDone && serverDone); round++)
               {
                  if (!clientDone)
                  {
                     const int result = SSL_do_handshake(client);
                     const int error = result == 1 ? SSL_ERROR_NONE : SSL_get_error(client, result);
                     clientDone = result == 1;
                     failed = !clientDone && error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE;
                  }

                  if (!serverDone && !failed)
                  {
                     const int result = SSL_do_handshake(server);
                     const int error = result == 1 ? SSL_ERROR_NONE : SSL_get_error(server, result);
                     serverDone = result == 1;
                     // The server's own failure is the client's refusal, read
                     // on the next round; it ends the loop either way.
                     if (!serverDone && error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
                        break;
                  }
               }

               finished = clientDone && !failed && SSL_get_verify_result(client) == X509_V_OK;

               uint8_t matchedUsage = 0;
               const int matchedDepth = SSL_get0_dane_tlsa(client, &matchedUsage, nullptr, nullptr, nullptr, nullptr);
               if (finished && matchedDepth >= 0)
               {
                  usage = matchedUsage;
                  depth = matchedDepth;
               }
            }
         }

         SSL_free(client);
         SSL_free(server);
         SSL_CTX_free(clientContext);
         SSL_CTX_free(serverContext);
         ERR_clear_error();

         ExpectDane_(ready, "the handshake's contexts could not be set up");
         return finished;
      }
   }

   void
   DaneTester::Test()
   {
      // The keys and certificates: an authority, an intermediate it issued
      // (the DANE-TA records' anchor), server certificates for mx.dane.test,
      // for another name and out of date, a self-signed certificate for
      // DANE-EE, and a key nothing is issued to.
      TestKey_ rootKey = NewKey_();
      TestKey_ intermediateKey = NewKey_();
      TestKey_ leafKey = NewKey_();
      TestKey_ strangerKey = NewKey_();

      TestCertificate_ root = NewCertificate_("DANE test root", rootKey.get(), nullptr, nullptr, true, nullptr, 365);
      TestCertificate_ intermediate = NewCertificate_("DANE test intermediate", intermediateKey.get(), root.get(), rootKey.get(), true, nullptr, 365);
      TestCertificate_ leaf = NewCertificate_("mx.dane.test", leafKey.get(), intermediate.get(), intermediateKey.get(), false, "mx.dane.test", 30);
      TestCertificate_ otherName = NewCertificate_("other.example", leafKey.get(), intermediate.get(), intermediateKey.get(), false, "other.example", 30);
      TestCertificate_ expired = NewCertificate_("mx.dane.test", leafKey.get(), intermediate.get(), intermediateKey.get(), false, "mx.dane.test", -1);
      TestCertificate_ selfSigned = NewCertificate_("unrelated.example", leafKey.get(), nullptr, nullptr, false, "unrelated.example", 30);

      const std::string intermediateSpki = SpkiDer_(intermediateKey.get());
      const TlsaRecord taIntermediate = Record_(2, 1, 1, Sha256_(intermediateSpki));
      const TlsaRecord taStranger = Record_(2, 1, 1, Sha256_(SpkiDer_(strangerKey.get())));
      const TlsaRecord taRootCertificate = Record_(2, 0, 1, Sha256_(CertificateDer_(root.get())));
      const TlsaRecord eeSelfSigned = Record_(3, 1, 1, Sha256_(SpkiDer_(leafKey.get())));
      const TlsaRecord eeStranger = Record_(3, 1, 1, Sha256_(SpkiDer_(strangerKey.get())));

      // Which records are usable (RFC 7672 sections 3.1 and 2.2).
      {
         struct UsableCase
         {
            TlsaRecord record;
            bool usable = false;
            const char *why = nullptr;
         };

         const UsableCase cases[] =
         {
            { taIntermediate, true, "DANE-TA, the key's SHA-256 (2 1 1)" },
            { eeStranger, true, "DANE-EE, the key's SHA-256 (3 1 1)" },
            { Record_(2, 0, 2, Sha512_(CertificateDer_(intermediate.get()))), true, "DANE-TA, the certificate's SHA-512 (2 0 2)" },
            { Record_(2, 0, 0, CertificateDer_(intermediate.get())), true, "DANE-TA, the whole certificate (2 0 0)" },
            { Record_(2, 1, 0, intermediateSpki), true, "DANE-TA, the whole key (2 1 0)" },
            { Record_(0, 1, 1, taIntermediate.data), false, "PKIX-TA, which SMTP does not use (0 1 1)" },
            { Record_(1, 1, 1, taIntermediate.data), false, "PKIX-EE, which SMTP does not use (1 1 1)" },
            { Record_(4, 1, 1, taIntermediate.data), false, "a usage that does not exist (4 1 1)" },
            { Record_(2, 2, 1, taIntermediate.data), false, "a selector that does not exist (2 2 1)" },
            { Record_(2, 1, 3, taIntermediate.data), false, "a matching type that does not exist (2 1 3)" },
            { Record_(2, 1, 1, taIntermediate.data.substr(1)), false, "a SHA-256 an octet short" },
            { Record_(2, 1, 2, taIntermediate.data), false, "a SHA-512 the length of a SHA-256" },
            { Record_(2, 1, 1, std::string()), false, "no data" },
            { Record_(2, 0, 0, std::string("\x30\x03\x02\x01\x00", 5)), false, "a whole certificate that does not parse" },
            { Record_(2, 1, 0, intermediateSpki + std::string(1, '\0')), false, "a whole key with an octet after it" },
         };

         for (const UsableCase &test : cases)
            ExpectDane_(DaneVerifier::IsUsable(test.record) == test.usable,
               std::string(test.why) + (test.usable ? " was taken as unusable" : " was taken as usable"));
      }

      // What a DNSSEC-validated RRset means for the connection (section 2.2).
      // Until 6.3.6 every RRset without a DANE-EE record was NoRecords: no
      // TLS required, and the message in the clear when STARTTLS was taken
      // away; DANE-TA records were not used at all.
      {
         std::vector<TlsaRecord> usable;
         ExpectDane_(TlsPolicy::ClassifyValidatedTlsa(std::vector<TlsaRecord>(), usable) == TlsPolicy::TlsaLookupStatus::NoRecords && usable.empty(),
            "an empty RRset was not taken for no records");

         std::vector<TlsaRecord> unusable;
         unusable.push_back(Record_(0, 1, 1, taIntermediate.data));
         unusable.push_back(Record_(1, 1, 1, taIntermediate.data));
         unusable.push_back(Record_(2, 1, 1, taIntermediate.data.substr(1)));
         ExpectDane_(TlsPolicy::ClassifyValidatedTlsa(unusable, usable) == TlsPolicy::TlsaLookupStatus::Unusable && usable.empty(),
            "an RRset of PKIX and malformed records was not taken for one that requires TLS without authentication");

         std::vector<TlsaRecord> trustAnchorsOnly(1, taIntermediate);
         ExpectDane_(TlsPolicy::ClassifyValidatedTlsa(trustAnchorsOnly, usable) == TlsPolicy::TlsaLookupStatus::DnssecValidated && usable.size() == 1,
            "an RRset of one DANE-TA record (2 1 1) was not taken for a usable one");

         std::vector<TlsaRecord> mixed(unusable);
         mixed.push_back(eeStranger);
         ExpectDane_(TlsPolicy::ClassifyValidatedTlsa(mixed, usable) == TlsPolicy::TlsaLookupStatus::DnssecValidated &&
                     usable.size() == 1 && usable[0].usage == 3,
            "the one usable record of a mixed RRset was not the one kept");
      }

      // Arming a connection: the records OpenSSL took, none when none is usable.
      {
         SSL_CTX *context = SSL_CTX_new(TLS_client_method());
         ExpectDane_(context != nullptr && SSL_CTX_dane_enable(context) > 0, "a client context could not have DANE");
         SSL *ssl = SSL_new(context);
         ExpectDane_(ssl != nullptr, "OpenSSL made no connection");

         std::vector<TlsaRecord> unusable;
         unusable.push_back(Record_(0, 1, 1, taIntermediate.data));
         unusable.push_back(Record_(1, 0, 1, taIntermediate.data));
         const int armedWithNone = DaneVerifier::Arm(ssl, "mx.dane.test", unusable, 0);
         SSL_free(ssl);

         ssl = SSL_new(context);
         std::vector<TlsaRecord> two;
         two.push_back(taIntermediate);
         two.push_back(eeStranger);
         const int armedWithTwo = DaneVerifier::Arm(ssl, "mx.dane.test", two, 0);
         SSL_free(ssl);
         SSL_CTX_free(context);
         ERR_clear_error();

         ExpectDane_(armedWithNone == 0, "a connection was armed with records none of which is usable");
         ExpectDane_(armedWithTwo == 2, "a connection was armed with " + std::to_string(armedWithTwo) + " of two usable records");
      }

      std::vector<X509 *> chain(1, intermediate.get());
      int usage = -1;
      int depth = -1;

      // DANE-TA: the server's chain carries the anchor the record names, and
      // its certificate names the host. Until 6.3.6 such a record was not used.
      ExpectDane_(Handshake_(leaf.get(), leafKey.get(), chain, "mx.dane.test", std::vector<TlsaRecord>(1, taIntermediate), usage, depth),
         "a server whose chain carries the DANE-TA record's anchor was refused");
      ExpectDane_(usage == 2 && depth == 1,
         "the DANE-TA handshake was authenticated by usage " + std::to_string(usage) + " at depth " + std::to_string(depth) + ", not by the anchor at depth 1");

      // The same with a DANE-EE record that does not match beside it: the
      // DANE-TA record still authenticates.
      {
         std::vector<TlsaRecord> both;
         both.push_back(eeStranger);
         both.push_back(taIntermediate);
         ExpectDane_(Handshake_(leaf.get(), leafKey.get(), chain, "mx.dane.test", both, usage, depth) && usage == 2,
            "a matching DANE-TA record did not authenticate beside a DANE-EE record that does not match");
      }

      // A DANE-TA record for a key nobody issued with: refused.
      ExpectDane_(!Handshake_(leaf.get(), leafKey.get(), chain, "mx.dane.test", std::vector<TlsaRecord>(1, taStranger), usage, depth),
         "a server was accepted on a DANE-TA record that names another anchor");

      // The anchor's digest with the anchor not in the chain: nothing to match.
      ExpectDane_(!Handshake_(leaf.get(), leafKey.get(), std::vector<X509 *>(), "mx.dane.test", std::vector<TlsaRecord>(1, taIntermediate), usage, depth),
         "a server that did not send the DANE-TA record's anchor was accepted");

      // An anchor above the chain the server sent, by digest: not in the chain either.
      ExpectDane_(!Handshake_(leaf.get(), leafKey.get(), chain, "mx.dane.test", std::vector<TlsaRecord>(1, taRootCertificate), usage, depth),
         "a server was accepted on the digest of a root it did not send");

      // DANE-TA is a trust anchor and nothing more: the certificate must name
      // the host (section 3.2.2) and be in date (section 3.1.2).
      ExpectDane_(!Handshake_(otherName.get(), leafKey.get(), chain, "mx.dane.test", std::vector<TlsaRecord>(1, taIntermediate), usage, depth),
         "a certificate for another name was accepted under a DANE-TA record");
      ExpectDane_(!Handshake_(expired.get(), leafKey.get(), chain, "mx.dane.test", std::vector<TlsaRecord>(1, taIntermediate), usage, depth),
         "an expired certificate was accepted under a DANE-TA record");

      // DANE-EE: the key itself is the anchor, whatever the certificate names
      // and whoever signed it (section 3.1.1).
      ExpectDane_(Handshake_(selfSigned.get(), leafKey.get(), std::vector<X509 *>(), "mx.dane.test", std::vector<TlsaRecord>(1, eeSelfSigned), usage, depth),
         "a self-signed certificate for another name was refused under the DANE-EE record of its key");
      ExpectDane_(usage == 3 && depth == 0,
         "the DANE-EE handshake was authenticated by usage " + std::to_string(usage) + " at depth " + std::to_string(depth));

      ExpectDane_(!Handshake_(selfSigned.get(), leafKey.get(), std::vector<X509 *>(), "mx.dane.test", std::vector<TlsaRecord>(1, eeStranger), usage, depth),
         "a server was accepted on a DANE-EE record of another key");

      // What a host whose TLS the destination's own records require is deferred
      // with when TLS cannot be had: usable TLSA records, an MTA-STS policy in
      // enforce mode and a validated RRset of none usable alike - retried, never
      // the permanent 5.7.0 a missing STARTTLS gave the first two until 6.3.6 -
      // and nothing when the destination published none of the three.
      {
         const String host = _T("mx.dane.test");
         const String domain = _T("dane.test");

         const String usable = TlsPolicy::PublishedTlsDeferral(host, domain, true, false, false);
         ExpectDane_(usable.Find(_T("Delivery deferred: the TLSA records of mx.dane.test require TLS")) == 0 && usable.Find(_T("will be retried")) > 0,
            "a host with usable TLSA records is not deferred with their sentence");

         const String sts = TlsPolicy::PublishedTlsDeferral(host, domain, false, false, true);
         ExpectDane_(sts.Find(_T("Delivery deferred: the MTA-STS policy of dane.test requires TLS to mx.dane.test")) == 0 && sts.Find(_T("will be retried")) > 0,
            "a host under an MTA-STS policy in enforce mode is not deferred with its sentence");

         const String unusable = TlsPolicy::PublishedTlsDeferral(host, domain, false, true, false);
         ExpectDane_(unusable.Find(_T("Delivery deferred: the DNSSEC-validated TLSA records of mx.dane.test require TLS")) == 0,
            "a host whose validated TLSA records are none of them usable is not deferred with their sentence");

         ExpectDane_(TlsPolicy::PublishedTlsDeferral(host, domain, true, false, true) == usable,
            "DANE was not put before MTA-STS");
         ExpectDane_(TlsPolicy::PublishedTlsDeferral(host, domain, false, true, true) == sts,
            "a policy that verifies the certificate was not put before records that do not");
         ExpectDane_(TlsPolicy::PublishedTlsDeferral(host, domain, false, false, false).IsEmpty(),
            "a host the destination published nothing for was given a deferral of its own");
      }
   }
}
