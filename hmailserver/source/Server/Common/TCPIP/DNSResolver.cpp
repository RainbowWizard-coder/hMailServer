// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "DNSResolver.h"
#include "DNSResolverWinApi.h"
#include "DnssecResolver.h"
#ifdef HM_PLATFORM_POSIX

// iphlpapi.h and windns.h are the Windows IP helper and DNS client headers. This
// file needs nothing at all from the first - the include predates the work that
// moved the querying into DNSResolverWinApi - and from the second it needs only
// the eight numbers below, which is why they are written out here rather than a
// POSIX DNS header being reached for.
//
// The six resource-record types are the values assigned in RFC 1035 and RFC 3596
// and are the same numbers on every platform and on the wire; windns.h spells them
// DNS_TYPE_* and this build agrees with it, so DNSResolverWinApi::Query - whose
// signature takes a plain int - is called with the identical value on both.
//
// The two status values are Win32 codes rather than protocol ones, and they are
// kept because they are what the querying layer hands back: DNSResolverWinApi's
// POSIX side reports these same numbers, so the two tests below keep meaning what
// they say instead of silently never matching.

#define DNS_TYPE_A     0x0001
#define DNS_TYPE_CNAME 0x0005
#define DNS_TYPE_PTR   0x000c
#define DNS_TYPE_MX    0x000f
#define DNS_TYPE_TEXT  0x0010
#define DNS_TYPE_AAAA  0x001c

#define DNS_ERROR_RCODE_NAME_ERROR 9003L
#define DNS_INFO_NO_RECORDS        9501L

#else
#include <iphlpapi.h>
#include <windns.h>
#endif
#include <boost/asio.hpp>

#include <map>
#include <stdexcept>

#include "HostNameAndIpAddress.h"
#include "IPAddress.h"

using boost::asio::ip::tcp;

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   struct DnsRecordWithPreference
   {
      DnsRecordWithPreference(long preference, String value)
      {
         Preference = preference;
         Value = value;
      }

      long Preference;
      String Value;
   };

   bool SortDnsRecordWithPreference(DnsRecordWithPreference first, DnsRecordWithPreference second) { return (first.Preference<second.Preference); }

   namespace
   {
      // ----------------------------------------------------------------
      // DNSResolverTester's DNS
      //
      // Every query this class makes goes through Ask_, which asks
      // DNSResolverWinApi - the Windows DNS client, or the POSIX resolver -
      // unless the tester has put answers of its own on the calling thread.
      // Then the question is answered from them, nothing is sent, and the
      // question is written down, so that the tester can say what was asked
      // and whether it went past the cache. In the service the tester runs
      // beside real deliveries, and every other thread sees nothing of it.
      // ----------------------------------------------------------------

      struct ScriptedDns
      {
         struct Answer
         {
            // False: the query failed, as a SERVFAIL or a timeout fails it.
            bool answered = false;

            // What DNSResolverWinApi::Query reports with it: 0, 9003 for
            // NXDOMAIN, 9501 for NODATA, a failure's own number.
            int status = 0;

            std::vector<DNSRecord> records;
         };

         // By "type/name", the answers given in turn; the last is given again
         // once they run out. A question with none is answered NODATA, as the
         // regression suite's own zone answers one.
         std::map<AnsiString, std::vector<Answer>> answers;

         // Every question, in order: "type/name", and " bypassing the cache"
         // when it was asked so.
         std::vector<AnsiString> asked;

         bool ipv6Available = true;
         bool ipv6Preferred = false;
      };

      thread_local ScriptedDns *scripted_dns = nullptr;

      AnsiString
      QuestionKey_(int resourceType, const String &query)
      {
         AnsiString key;
         key.Format("%d/", resourceType);
         return key + AnsiString(query);
      }

      bool
      Ask_(const String &query, int resourceType, std::vector<DNSRecord> &records, int &status, bool bypassCache)
      {
         if (scripted_dns == nullptr)
         {
            DNSResolverWinApi resolver;
            return resolver.Query(query, resourceType, records, status, bypassCache);
         }

         const AnsiString key = QuestionKey_(resourceType, query);
         size_t askedBefore = 0;
         for (const AnsiString &question : scripted_dns->asked)
         {
            if (question == key || question == key + " bypassing the cache")
               askedBefore++;
         }

         scripted_dns->asked.push_back(bypassCache ? key + " bypassing the cache" : key);

         auto scripted = scripted_dns->answers.find(key);
         if (scripted == scripted_dns->answers.end() || scripted->second.empty())
         {
            status = DNS_INFO_NO_RECORDS;
            return true;
         }

         const ScriptedDns::Answer &answer = scripted->second[(std::min)(askedBefore, scripted->second.size() - 1)];
         status = answer.status;

         // As DNSResolverWinApi does: a failed query adds nothing, and an answer
         // is added to what the caller already holds.
         if (answer.answered)
            records.insert(records.end(), answer.records.begin(), answer.records.end());

         return answer.answered;
      }

      bool
      IPv6Available_()
      {
         if (scripted_dns != nullptr)
            return scripted_dns->ipv6Available;

         return Configuration::Instance()->IsIPv6Available();
      }

      bool
      IPv6Preferred_()
      {
         if (scripted_dns != nullptr)
            return scripted_dns->ipv6Preferred;

         return Configuration::Instance()->GetIPv6Preferred();
      }
   }


   DNSResolver::DNSResolver()
   {

   }

   DNSResolver::~DNSResolver()
   {

   }

   std::vector<String>
   DNSResolver::GetDnsRecordsValues_(std::vector<DNSRecord> records)
   {
      std::vector<String> values;

      for (DNSRecord record : records)
      {
         values.push_back(record.GetValue());
      }

      return values;
   }
   
   bool
   DNSResolver::GetIpAddresses(const String &sDomain, std::vector<String> &vecFoundNames, bool followCnameRecords, bool bypassCache)
   {
      // An address is not a name, so it is not asked about in DNS.
      //
      // Without this, a host configured as "127.0.0.1" - which is what the SpamAssassin,
      // ClamAV and relay settings hold on most installations, and what the shipped default
      // is - becomes a DNS query for an A record NAMED "127.0.0.1". Whether that returns
      // anything is then up to the resolver: the Windows DNS client answers it from its own
      // literal handling in the common case, but that path is not taken when the query is
      // directed at a specific DNS server or has to bypass the cache, and an upstream
      // resolver has no reason to answer at all. When it does not answer, hMailServer
      // reports "The IP address for SpamAssassin could not be resolved" and stops - so a
      // correctly configured server appears to have lost SpamAssassin entirely, which is
      // issue #25.
      //
      // The IPv6 case is not a maybe: a host of "::1" fails here every time, on every
      // machine, because nothing in DNS is named "::1". That is reproducible and was
      // reproduced.
      //
      // Doing this at the top of the public entry point rather than inside the recursive
      // helper is deliberate: it applies to every caller - spam tests, virus scanning,
      // route hosts, relay hosts, external fetching - and it also removes a pointless
      // round trip from the hot path, because the overwhelmingly common configuration for
      // all of those is a literal address.
      IPAddress parsedAddress;
      if (parsedAddress.TryParse(AnsiString(sDomain), false))
      {
         // Normalised through IPAddress rather than echoed back, so that the caller gets
         // the same canonical form it would get from a lookup.
         vecFoundNames.push_back(String(parsedAddress.ToString()));
         return true;
      }

      return GetIpAddressesRecursive_(sDomain, vecFoundNames, 0, followCnameRecords, bypassCache);
   }

   bool
   DNSResolver::GetIpAddressesWithRetry(const String &sDomain, std::vector<String> &saFoundNames, bool followCnameRecords)
   {
      // A lookup whose answer is a verdict - a DNS blacklist's, SURBL's - is asked
      // once more when it did not complete, because a single failed query would
      // otherwise stand for "not listed" (upstream PR 640). Past the DNS cache: the
      // Windows DNS client would otherwise answer the second attempt from what it
      // kept of the first. A lookup that answered, with addresses or without, is
      // not asked again, and nothing a failed attempt left behind is in
      // saFoundNames - a lookup that fails adds no address.
      //
      // A lookup that timed out is asked again too, and so waits for the DNS
      // twice: on Linux the resolver sends one packet to each server and waits
      // for it, so a timeout there is as often one lost packet as a server that
      // is down.
      if (GetIpAddresses(sDomain, saFoundNames, followCnameRecords))
         return true;

      LOG_DEBUG(Formatter::Format(_T("DNS - The lookup of {0} did not complete. Asking once more, past the DNS cache."), sDomain));

      return GetIpAddresses(sDomain, saFoundNames, followCnameRecords, true);
   }

   bool
   DNSResolver::GetIpAddressesRecursive_(const String &hostName, std::vector<String> &addresses, int recursionLevel, bool followCnameRecords, bool bypassCache)
   {
      if (hostName.IsEmpty())
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5516, "DNSResolver::GetIpAddressesRecursive_", "Attempted DNS lookup for empty host name.");
         return false;
      }

      if (recursionLevel > 10)
      {
         String sMessage = Formatter::Format("Too many recursions during IP address lookup. Query: {0}", hostName);
         ErrorManager::Instance()->ReportError(ErrorManager::Low, 4401, "DNSResolver::GetIpAddressesRecursive_", sMessage);

         return false;
      }

      std::vector<DNSRecord> foundRecords;
      int status = 0;

      // Whether every query made here was answered - with records or without. A
      // query answered NXDOMAIN or NODATA says the name holds no address of that
      // family; one that failed - a SERVFAIL, a timeout - says nothing at all.
      // Until upstream PR 640 the two results were joined by OR, so a failed A
      // query beside an AAAA query answered NODATA, which is the answer of every
      // IPv4-only host, made "this host has no addresses": the HELO test scored
      // the message for a name it could not look up, and a DNS blacklist or SURBL
      // lookup read a failure as "not listed".
      bool everyQueryAnswered = true;

      // if IPv6 is preferred first do IPv6 DNS Lookup(s)
      const bool ipv6Available = IPv6Available_();
      if (ipv6Available && IPv6Preferred_())
      {
         if (!Ask_(hostName, DNS_TYPE_AAAA, foundRecords, status, bypassCache))
            everyQueryAnswered = false;

         if (!Ask_(hostName, DNS_TYPE_A, foundRecords, status, bypassCache))
            everyQueryAnswered = false;
      }
      else // Standard
      {
         if (!Ask_(hostName, DNS_TYPE_A, foundRecords, status, bypassCache))
            everyQueryAnswered = false;

         if (ipv6Available && !Ask_(hostName, DNS_TYPE_AAAA, foundRecords, status, bypassCache))
            everyQueryAnswered = false;
      }

      if (foundRecords.size() == 0 && followCnameRecords)
      {
         // The queries for A/AAAA didn't return any records. Attempt to look up via CNAME
         std::vector<DNSRecord> foundCNames;
         bool cnameQueryResult = Ask_(hostName, DNS_TYPE_CNAME, foundCNames, status, bypassCache);

         // A CNAME should only point at a single host name.
         if (cnameQueryResult && foundCNames.size() == 1)
         {
            auto cnameHostName = foundCNames[0].GetValue();
            return GetIpAddressesRecursive_(cnameHostName, addresses, recursionLevel + 1, followCnameRecords, bypassCache);
         }

         // Whether the name is an alias of one that has addresses is not known
         // either, so neither is whether it has any.
         if (!cnameQueryResult)
            everyQueryAnswered = false;
      }

      std::vector<String> foundValues = GetDnsRecordsValues_(foundRecords);
      addresses.insert(addresses.end(), foundValues.begin(), foundValues.end());

      // Addresses found are an answer whatever else failed: a host whose AAAA
      // query fails still has the IPv4 addresses its A query found. Without any,
      // a failed query leaves the answer unknown rather than empty.
      return !foundRecords.empty() || everyQueryAnswered;
   }


   bool 
   DNSResolver::GetTXTRecords(const String &sDomain, std::vector<String> &foundResult)
   {
      // When DNSSEC validation is enabled, TXT records used for sender
      // authentication (SPF/DKIM/DMARC keys and policies) are validated
      // in-process. Forged (bogus) data is treated as a lookup failure.
      std::vector<AnsiString> texts;
      bool answered = false;
      bool bogus = false;

      if (TryValidatedTxtRecords_(sDomain, texts, answered, bogus))
      {
         if (bogus)
            return false;

         if (answered)
         {
            for (const AnsiString &text : texts)
               foundResult.push_back(String(text));

            return true;
         }
      }

      return GetTXTRecordsRecursive_(sDomain, foundResult, 0);
   }

   bool
   DNSResolver::TryValidatedTxtRecords_(const String &sDomain, std::vector<AnsiString> &texts, bool &answered, bool &bogus)
   {
      answered = false;
      bogus = false;

      if (!IniFileSettings::Instance()->GetDnssecValidationEnabled() || sDomain.IsEmpty())
         return false;

      DnssecResolver dnssecResolver;

      DnssecResolver::ChainStatus status = dnssecResolver.QueryTxt(sDomain, texts);

      if (status == DnssecResolver::ChainStatus::Bogus)
      {
         LOG_DEBUG("DNSSEC: TXT records for " + sDomain + " failed DNSSEC validation. Treating the lookup as failed.");

         bogus = true;
         return true;
      }

      if (status == DnssecResolver::ChainStatus::Secure)
      {
         answered = true;
         return true;
      }

      // Insecure: the validating path may have been blocked at the
      // transport level, so fall through to the system resolver to
      // avoid losing records.
      return false;
   }

   bool
   DNSResolver::GetTXTRecordsRecursive_(const String &sDomain, std::vector<String> &foundResult, int recursionLevel)
   {
      std::vector<DNSRecord> records;

      bool result = GetRecordsOfTypeRecursive_(sDomain, DNS_TYPE_TEXT, records, recursionLevel);

      foundResult = GetDnsRecordsValues_(records);

      return result;
   }

   bool
   DNSResolver::GetRecordsOfType(const AnsiString &query, int resourceType, std::vector<AnsiString> &values)
   {
      values.clear();

      // A TXT lookup made here is a lookup of the same records GetTXTRecords returns,
      // so it goes through the same DNSSEC gate. Leaving it out would mean SPF - the
      // one caller of this method - read a forged policy that the DKIM and DMARC
      // lookups beside it would have refused, which is the wrong half of the pair to
      // trust less.
      if (resourceType == DNS_TYPE_TEXT)
      {
         std::vector<AnsiString> texts;
         bool answered = false;
         bool bogus = false;

         if (TryValidatedTxtRecords_(String(query), texts, answered, bogus))
         {
            if (bogus)
               return false;

            if (answered)
            {
               values = texts;
               return true;
            }
         }
      }

      std::vector<DNSRecord> records;

      bool result = GetRecordsOfTypeRecursive_(String(query), resourceType, records, 0);

      for (DNSRecord record : records)
         values.push_back(record.GetValue());

      return result;
   }

   bool
   DNSResolver::GetRecordsOfTypeRecursive_(const String &query, int resourceType, std::vector<DNSRecord> &records, int recursionLevel)
   {
      if (query.IsEmpty())
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5516, "DNSResolver::GetRecordsOfTypeRecursive_", "Attempted DNS lookup for empty host name.");
         return false;
      }

      if (recursionLevel > 10)
      {
         String sMessage = Formatter::Format("Too many recursions during record lookup. Query: {0}, type {1}", query, resourceType);
         ErrorManager::Instance()->ReportError(ErrorManager::Low, 4402, "DNSResolver::GetRecordsOfTypeRecursive_", sMessage);

         return false;
      }

      int status = 0;

      bool result = Ask_(query, resourceType, records, status, false);

      if (records.size() == 0)
      {
         // Nothing of the type asked for. The name may be an alias.
         std::vector<DNSRecord> foundCNames;
         bool cnameQueryResult = Ask_(query, DNS_TYPE_CNAME, foundCNames, status, false);

         // A CNAME should only point at a single host name.
         if (cnameQueryResult && foundCNames.size() == 1)
         {
            auto cnameHostName = foundCNames[0].GetValue();
            return GetRecordsOfTypeRecursive_(cnameHostName, resourceType, records, recursionLevel + 1);
         }

         // Whether the name is an alias of one that holds such records could not
         // be told, so the lookup did not complete - which is not "no records",
         // the answer a DKIM key, a DMARC policy or an SPF record that is not
         // published gets (upstream PR 640). On Windows this is the path of every
         // alias: the DNS client follows the CNAME, and the target's records come
         // back under the target's name, which the query's own name filters out -
         // a DKIM selector that is a CNAME, as Microsoft 365's are, among them.
         if (!cnameQueryResult)
            return false;
      }

      return result;
   }

   bool
   DNSResolver::DomainExists(const String &sDomain, bool &dnsError)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Whether the name exists in the DNS at all - not whether it has an address.
   //
   // Only NXDOMAIN means "does not exist". A NOERROR answer with no records means
   // the name IS there and simply holds nothing of the type asked for, which is an
   // ordinary and common shape: a name carrying only TXT or only MX, or an empty
   // non-terminal that exists purely because something below it does.
   //
   // The distinction is the whole value of this function. Its caller is DMARC's np=
   // tag, which applies a stricter policy to subdomains that do not exist - the
   // shape a phisher uses, because inventing a subdomain of a real company costs
   // nothing. Treating "no A record" as "does not exist" would apply that stricter
   // policy to real subdomains that simply have no address of their own, and the
   // stricter policy is usually reject.
   //
   // dnsError reports a lookup that could not be completed. The caller must not read
   // that as either answer: applying np on a resolver failure would reject mail from
   // a domain that exists, and skipping it would let the failure be worth provoking.
   //---------------------------------------------------------------------------()
   {
      dnsError = false;

      if (sDomain.IsEmpty())
         return false;

      std::vector<DNSRecord> foundRecords;
      int dnsStatus = 0;

      bool queried = Ask_(sDomain, DNS_TYPE_A, foundRecords, dnsStatus, false);

      // Records came back, so the name is plainly there. Checked first and on its own,
      // because the return value below cannot be read as "it worked": Query answers
      // TRUE for NXDOMAIN as well. Every other caller wants that - to them a name that
      // does not exist and a name with nothing useful on it are the same unusable
      // answer, and neither is worth reporting as a failure - but it is the exact
      // distinction this function was added to make, so the status has to be read even
      // on the success path.
      if (queried && !foundRecords.empty())
         return true;

      if (dnsStatus == DNS_ERROR_RCODE_NAME_ERROR)
         return false;

      // DNS_INFO_NO_RECORDS, or a clean answer carrying nothing: the name is there and
      // simply has no A record.
      if (dnsStatus == DNS_INFO_NO_RECORDS || (queried && dnsStatus == 0))
         return true;

      dnsError = true;

      return false;
   }

   bool
   DNSResolver::GetEmailServers(const String &sDomainName, std::vector<HostNameAndIpAddress> &saFoundNames)
   {
      return GetEmailServersRecursive_(sDomainName, saFoundNames, 0);
   }

   bool
   DNSResolver::GetEmailServersRecursive_(const String &sDomainName, std::vector<HostNameAndIpAddress> &saFoundNames, int recursionLevel)
   {
      if (sDomainName.IsEmpty())
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5516, "DNSResolver::GetEmailServersRecursive_", "Attempted DNS lookup for empty host name.");
         return false;
      }

      if (recursionLevel > 10)
      {
         String sMessage = Formatter::Format("Too many recursions during IP address lookup. Query: {0}", sDomainName);
         ErrorManager::Instance()->ReportError(ErrorManager::Low, 4403, "DNSResolver::GetEmailServersRecursive_", sMessage);

         return false;
      }
      
      String message = Formatter::Format("DNS MX lookup: {0}", sDomainName);
      LOG_TCPIP(message);

      std::vector<DNSRecord> foundMxRecords;
      int status = 0;

      if (!Ask_(sDomainName, DNS_TYPE_MX, foundMxRecords, status, false))
      {
         String logMessage;
            logMessage.Format(_T("Failed to resolve email servers (MX lookup). Domain name: %s."), sDomainName.c_str());
         LOG_DEBUG(logMessage);

         return false;
      }

      if (foundMxRecords.empty())
      {
         // The queries for MX didn't return any records. Attempt to look up via CNAME
         std::vector<DNSRecord> foundCNames;
         bool cnameQueryResult = Ask_(sDomainName, DNS_TYPE_CNAME, foundCNames, status, false);

         // A CNAME should only point at a single host name.
         if (cnameQueryResult && foundCNames.size() == 1)
         {
            auto cnameHostName = foundCNames[0].GetValue();
            return GetEmailServersRecursive_(cnameHostName, saFoundNames, recursionLevel + 1);
         }   
         
         /* RFC 2821:
            If no MX records are found, but an A RR is found, the A RR is treated as
            if it was associated with an implicit MX RR, with a preference of 0,
            pointing to that host.  If one or more MX RRs are found for a given
            name, SMTP systems MUST NOT utilize any A RRs associated with that
            name unless they are located using the MX RRs;
            (implemented here)
         */

         std::vector<String> a_records;
         if (!GetIpAddresses(sDomainName, a_records, true))
         {
            String logMessage;
               logMessage.Format(_T("Failed to resolve email servers (A lookup). Domain name: %s."), sDomainName.c_str());
            LOG_DEBUG(logMessage);

            return false;
         }
       
         for(String record : a_records)         
         {
            HostNameAndIpAddress hostAndAddress;
            hostAndAddress.SetHostName(sDomainName);
            hostAndAddress.SetIpAddress(record);

            saFoundNames.push_back(hostAndAddress);
         }
      }
      else
      {
         bool dnsQueryFailure = false;
         for (DNSRecord dnsRecord : foundMxRecords)
         {
            // Null MX, see: rfc7505 
            // https://tools.ietf.org/html/rfc7505
            if (dnsRecord.GetValue() == "." && dnsRecord.GetPreference() == 0)
            {
               return false;
            }

            // Resolve to domain name to IP address and put it in the list.
            std::vector<String> a_records;

            String hostName = dnsRecord.GetValue();
            if (StringParser::IsValidIPAddress(hostName))
            {
               // Okay, this is an invalid MX record. The MX record should always contain 
               // a host name but in this case it appears an IP address. We'll be kind to
               // the domain owner and still deliver the email to him.
               a_records.push_back(hostName);
            }
            else
            {
               if (!GetIpAddresses(hostName, a_records, true))
               {
                  dnsQueryFailure = true;
                  continue;
               }
            }

            for (String record : a_records)
            {
               HostNameAndIpAddress hostAndAddress;
               hostAndAddress.SetHostName(hostName);
               hostAndAddress.SetIpAddress(record);

               saFoundNames.push_back(hostAndAddress);
            }
         }

         if (dnsQueryFailure)
         {
            // All dns queries failed.
            String logMessage;
               logMessage.Format(_T("Failed to resolve email servers (A lookup). Domain name: %s."), sDomainName.c_str());
            LOG_DEBUG(logMessage);

            return false;
         }

      }

      String sLogMsg;
      sLogMsg.Format(_T("DNS - MX Result: %d IP addresses were found."), saFoundNames.size());

      LOG_TCPIP(sLogMsg);

            
      // Remove duplicate names.
      auto iter = saFoundNames.begin();
      std::set<String> duplicateCheck;

      while (iter != saFoundNames.end())
      {
         String name = (*iter).GetIpAddress();
         if (duplicateCheck.find(name) != duplicateCheck.end())
         {
            // We found a duplicate. Remove it.
            iter = saFoundNames.erase(iter);
         }
         else
         {
            // This is not a duplicate. Move to next.
            iter++;

            duplicateCheck.insert(name);
         }
      }

      return true;
   }

   bool
   DNSResolver::GetMXRecords(const String &sDomain, std::vector<String> &vecFoundNames)
   {
      return GetMXRecordsRecursive_(sDomain, vecFoundNames, 0);
   }

   bool
   DNSResolver::GetMXRecordsRecursive_(const String &sDomain, std::vector<String> &vecFoundNames, int recursionLevel)
   {
      if (sDomain.IsEmpty())
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5516, "DNSResolver::GetMXRecordsRecursive_", "Attempted DNS lookup for empty host name.");
         return false;
      }

      if (recursionLevel > 10)
      {
         String sMessage = Formatter::Format("Too many recursions during MX record lookup. Query: {0}", sDomain);
         ErrorManager::Instance()->ReportError(ErrorManager::Low, 4403, "DNSResolver::GetMXRecordsRecursive_", sMessage);

         return false;
      }
      
      std::vector<DNSRecord> foundMxRecords;
      int status = 0;

      bool result = Ask_(sDomain, DNS_TYPE_MX, foundMxRecords, status, false);

      if (foundMxRecords.size() == 0)
      {
         // The queries for MX didn't return any records. Attempt to look up via CNAME
         std::vector<DNSRecord> foundCNames;
         bool cnameQueryResult = Ask_(sDomain, DNS_TYPE_CNAME, foundCNames, status, false);

         // A CNAME should only point at a single host name.
         if (cnameQueryResult && foundCNames.size() == 1)
         {
            auto cnameHostName = foundCNames[0].GetValue();
            return GetMXRecordsRecursive_(cnameHostName, vecFoundNames, recursionLevel + 1);
         }

         // Whether the name is an alias of one with MX records could not be told:
         // the lookup did not complete, which the MX-record spam test reads as a
         // failure and not as a domain with no MX (GetRecordsOfTypeRecursive_).
         if (!cnameQueryResult)
            result = false;
      }

      for (DNSRecord dnsRecord : foundMxRecords)
      {
         // Null MX, see: rfc7505 
         // https://tools.ietf.org/html/rfc7505
         if (dnsRecord.GetValue() == "." && dnsRecord.GetPreference() == 0)
         {
            return false;
         }
      }

      vecFoundNames = GetDnsRecordsValues_(foundMxRecords);

      return result;
   }

   bool 
   DNSResolver::GetPTRRecords(const String &sIP, std::vector<String> &vecFoundNames)
   {
      IPAddress address;
      if (!address.TryParse(AnsiString(sIP), true))
         return false;

      String fullQuery;

      if (address.GetType() == IPAddress::IPV4)
      {
         std::vector<String> vecItems = StringParser::SplitString(sIP, ".");
         reverse(vecItems.begin(), vecItems.end());
         String result = StringParser::JoinVector(vecItems, ".");
         fullQuery = result + ".in-addr.arpa";
      }
      else
      {
         AnsiString long_ipv6 = address.ToLongString();
         long_ipv6.MakeReverse();
         long_ipv6.Remove(':');

         for (int i = long_ipv6.GetLength() - 1; i > 0; i--)
         {
            long_ipv6.insert(i, 1, '.');
         }
         fullQuery = long_ipv6 + ".ip6.arpa";
      }


      std::vector<DNSRecord> foundRecords;
      int status = 0;

      bool result = Ask_(fullQuery, DNS_TYPE_PTR, foundRecords, status, false);

      vecFoundNames = GetDnsRecordsValues_(foundRecords);
      return result;

   }

   void
   DNSResolverTester::Test()
   {
      auto expect = [](bool condition, const std::string &what)
      {
         if (!condition)
            throw std::logic_error(std::string("DNSResolverTester: ") + what);
      };

      // The numbers DNSResolverWinApi reports, on both platforms: 9003 NXDOMAIN
      // and 9501 NODATA, which are answers; 9002 a server failure and 258 the
      // query's own timeout, which are not.
      const int nxdomain = 9003;
      const int nodata = 9501;
      const int servfail = 9002;
      const int timedOut = 258;

      typedef ScriptedDns::Answer Answer;

      auto withRecord = [](int type, const char *value) -> Answer
      {
         Answer answer;
         answer.answered = true;
         answer.status = 0;
         answer.records.push_back(DNSRecord(AnsiString(value), type, 0));
         return answer;
      };

      auto noRecords = [](int status) -> Answer
      {
         Answer answer;
         answer.answered = true;
         answer.status = status;
         return answer;
      };

      auto failed = [](int status) -> Answer
      {
         Answer answer;
         answer.answered = false;
         answer.status = status;
         return answer;
      };

      // Installs a zone on this thread for as long as it lives.
      struct Installed
      {
         explicit Installed(ScriptedDns &zone) { scripted_dns = &zone; }
         ~Installed() { scripted_dns = nullptr; }
      };

      auto joined = [](const std::vector<String> &values) -> std::string
      {
         String text = StringParser::JoinVector(values, _T(","));
         return std::string(AnsiString(text).c_str());
      };

      auto askedOf = [](const ScriptedDns &zone) -> std::string
      {
         std::string text;
         for (const AnsiString &question : zone.asked)
            text += (text.empty() ? "" : ", ") + std::string(question.c_str());
         return text;
      };

      // One address lookup: what it returns, and what it found.
      struct Case
      {
         const char *why;
         std::vector<std::pair<int, Answer>> answers;   // by type, for host.test
         bool followCnames;
         bool expected;
         const char *expectedAddresses;
      };

      const int a = DNS_TYPE_A;
      const int aaaa = DNS_TYPE_AAAA;
      const int cname = DNS_TYPE_CNAME;

      const std::vector<Case> cases =
      {
         // The defect: a failed A query beside an AAAA query answered NODATA,
         // the answer of every IPv4-only host, was "no addresses".
         { "a failed A query, the AAAA query answered NODATA: the lookup failed",
           { { a, failed(servfail) }, { aaaa, noRecords(nodata) } }, false, false, "" },
         { "a failed A query, the AAAA query answered NXDOMAIN: the lookup failed",
           { { a, failed(servfail) }, { aaaa, noRecords(nxdomain) } }, false, false, "" },
         { "the A query timed out, the AAAA query answered NODATA: the lookup failed",
           { { a, failed(timedOut) }, { aaaa, noRecords(nodata) } }, false, false, "" },
         { "the A query answered NODATA, a failed AAAA query: the lookup failed",
           { { a, noRecords(nodata) }, { aaaa, failed(servfail) } }, false, false, "" },
         { "both failed: the lookup failed",
           { { a, failed(servfail) }, { aaaa, failed(servfail) } }, false, false, "" },

         // Answers stay answers.
         { "both answered NODATA: no addresses",
           { { a, noRecords(nodata) }, { aaaa, noRecords(nodata) } }, false, true, "" },
         { "both answered NXDOMAIN: no addresses",
           { { a, noRecords(nxdomain) }, { aaaa, noRecords(nxdomain) } }, false, true, "" },
         { "an A record and a failed AAAA query: the A record",
           { { a, withRecord(a, "192.0.2.1") }, { aaaa, failed(servfail) } }, false, true, "192.0.2.1" },
         { "a failed A query and an AAAA record: the AAAA record",
           { { a, failed(servfail) }, { aaaa, withRecord(aaaa, "2001:db8::1") } }, false, true, "2001:db8::1" },
         { "both records",
           { { a, withRecord(a, "192.0.2.1") }, { aaaa, withRecord(aaaa, "2001:db8::1") } }, false, true, "192.0.2.1,2001:db8::1" },

         // The CNAME query, made when A and AAAA found nothing, is a query too.
         { "NODATA for both and a failed CNAME query: the lookup failed",
           { { a, noRecords(nodata) }, { aaaa, noRecords(nodata) }, { cname, failed(servfail) } }, true, false, "" },
         { "NODATA for both and NODATA for the CNAME: no addresses",
           { { a, noRecords(nodata) }, { aaaa, noRecords(nodata) }, { cname, noRecords(nodata) } }, true, true, "" },
         { "a failed A query and an alias: the alias's target's addresses",
           { { a, failed(servfail) }, { aaaa, noRecords(nodata) }, { cname, withRecord(cname, "target.test") } }, true, true, "192.0.2.9" },
      };

      for (const Case &test : cases)
      {
         ScriptedDns zone;
         for (const auto &answer : test.answers)
            zone.answers[QuestionKey_(answer.first, _T("host.test"))].push_back(answer.second);
         zone.answers[QuestionKey_(a, _T("target.test"))].push_back(withRecord(a, "192.0.2.9"));

         Installed installed(zone);
         DNSResolver resolver;
         std::vector<String> found;
         const bool result = resolver.GetIpAddresses(_T("host.test"), found, test.followCnames);

         expect(result == test.expected, std::string(test.why) + ": answered " + (result ? "true" : "false") + "; asked " + askedOf(zone));
         expect(joined(found) == test.expectedAddresses, std::string(test.why) + ": found \"" + joined(found) + "\", expected \"" + test.expectedAddresses + "\"");
      }

      // With IPv6 preferred the AAAA query goes first, and the rule is the same.
      {
         ScriptedDns zone;
         zone.ipv6Preferred = true;
         zone.answers[QuestionKey_(aaaa, _T("host.test"))].push_back(noRecords(nodata));
         zone.answers[QuestionKey_(a, _T("host.test"))].push_back(failed(servfail));

         Installed installed(zone);
         DNSResolver resolver;
         std::vector<String> found;
         expect(!resolver.GetIpAddresses(_T("host.test"), found, false),
                "IPv6 preferred: NODATA for AAAA, then a failed A query: the lookup failed; asked " + askedOf(zone));
         expect(askedOf(zone) == "28/host.test, 1/host.test", "IPv6 preferred: AAAA asked first; asked " + askedOf(zone));
      }

      // Without IPv6 there is no AAAA query, and a failed A query fails the
      // lookup, as it always did.
      {
         ScriptedDns zone;
         zone.ipv6Available = false;
         zone.answers[QuestionKey_(a, _T("host.test"))].push_back(failed(servfail));

         Installed installed(zone);
         DNSResolver resolver;
         std::vector<String> found;
         expect(!resolver.GetIpAddresses(_T("host.test"), found, false), "no IPv6: a failed A query fails the lookup");
         expect(askedOf(zone) == "1/host.test", "no IPv6: only A is asked; asked " + askedOf(zone));
      }

      // What the caller held is kept, and a failed lookup adds nothing to it.
      {
         ScriptedDns zone;
         zone.answers[QuestionKey_(a, _T("host.test"))].push_back(failed(servfail));

         Installed installed(zone);
         DNSResolver resolver;
         std::vector<String> found;
         found.push_back(_T("198.51.100.1"));
         resolver.GetIpAddresses(_T("host.test"), found, false);
         expect(joined(found) == "198.51.100.1", "a failed lookup adds nothing to what the caller held: \"" + joined(found) + "\"");
      }

      // GetIpAddressesWithRetry: a lookup that did not complete is asked once
      // more, past the cache; the second answer counts.
      {
         ScriptedDns zone;
         zone.answers[QuestionKey_(a, _T("2.0.0.127.list.test"))].push_back(failed(servfail));
         zone.answers[QuestionKey_(a, _T("2.0.0.127.list.test"))].push_back(withRecord(a, "127.0.0.2"));
         zone.answers[QuestionKey_(aaaa, _T("2.0.0.127.list.test"))].push_back(noRecords(nxdomain));

         Installed installed(zone);
         DNSResolver resolver;
         std::vector<String> found;
         expect(resolver.GetIpAddressesWithRetry(_T("2.0.0.127.list.test"), found, false),
                "a lookup that failed and then answered: true; asked " + askedOf(zone));
         expect(joined(found) == "127.0.0.2", "a lookup that failed and then answered: the second answer, once: \"" + joined(found) + "\"");
         expect(askedOf(zone) == "1/2.0.0.127.list.test, 28/2.0.0.127.list.test, "
                                 "1/2.0.0.127.list.test bypassing the cache, 28/2.0.0.127.list.test bypassing the cache",
                "the second attempt, and only it, goes past the cache; asked " + askedOf(zone));
      }

      // A lookup that answered - NXDOMAIN, the answer for an address no list
      // holds - is not asked again.
      {
         ScriptedDns zone;
         zone.answers[QuestionKey_(a, _T("3.0.0.127.list.test"))].push_back(noRecords(nxdomain));
         zone.answers[QuestionKey_(aaaa, _T("3.0.0.127.list.test"))].push_back(noRecords(nxdomain));

         Installed installed(zone);
         DNSResolver resolver;
         std::vector<String> found;
         expect(resolver.GetIpAddressesWithRetry(_T("3.0.0.127.list.test"), found, false) && found.empty(),
                "an answered lookup with no addresses: true, none");
         expect(zone.asked.size() == 2, "an answered lookup is asked once; asked " + askedOf(zone));
      }

      // One that answered with an address is not asked again either.
      {
         ScriptedDns zone;
         zone.answers[QuestionKey_(a, _T("4.0.0.127.list.test"))].push_back(withRecord(a, "127.0.0.4"));
         zone.answers[QuestionKey_(aaaa, _T("4.0.0.127.list.test"))].push_back(failed(servfail));

         Installed installed(zone);
         DNSResolver resolver;
         std::vector<String> found;
         expect(resolver.GetIpAddressesWithRetry(_T("4.0.0.127.list.test"), found, false) && joined(found) == "127.0.0.4",
                "an address and a failed AAAA query: the address, at once");
         expect(zone.asked.size() == 2, "a lookup that found an address is asked once; asked " + askedOf(zone));
      }

      // A lookup that fails twice fails, after two attempts and no more.
      {
         ScriptedDns zone;
         zone.answers[QuestionKey_(a, _T("5.0.0.127.list.test"))].push_back(failed(timedOut));
         zone.answers[QuestionKey_(aaaa, _T("5.0.0.127.list.test"))].push_back(noRecords(nxdomain));

         Installed installed(zone);
         DNSResolver resolver;
         std::vector<String> found;
         expect(!resolver.GetIpAddressesWithRetry(_T("5.0.0.127.list.test"), found, false) && found.empty(),
                "a lookup that fails twice: false, nothing found");
         expect(zone.asked.size() == 4, "a lookup that fails is asked twice, no more; asked " + askedOf(zone));
      }

      // The record lookups that follow an alias: nothing of the type and a
      // failed CNAME query is a lookup that did not complete, and not "no
      // records" - a DKIM key, a DMARC policy or an SPF record not published.
      // GetRecordsOfType is asked for MX here, which shares the path with TXT
      // and has no DNSSEC gate in front of it.
      {
         ScriptedDns zone;
         zone.answers[QuestionKey_(DNS_TYPE_MX, _T("alias.test"))].push_back(noRecords(nodata));
         zone.answers[QuestionKey_(cname, _T("alias.test"))].push_back(failed(servfail));

         Installed installed(zone);
         DNSResolver resolver;
         std::vector<AnsiString> values;
         expect(!resolver.GetRecordsOfType("alias.test", DNS_TYPE_MX, values) && values.empty(),
                "a record lookup that found nothing, with a failed CNAME query: false; asked " + askedOf(zone));

         std::vector<String> names;
         expect(!resolver.GetMXRecords(_T("alias.test"), names) && names.empty(),
                "an MX lookup that found nothing, with a failed CNAME query: false; asked " + askedOf(zone));
      }

      {
         ScriptedDns zone;
         zone.answers[QuestionKey_(DNS_TYPE_MX, _T("plain.test"))].push_back(noRecords(nodata));
         zone.answers[QuestionKey_(cname, _T("plain.test"))].push_back(noRecords(nodata));

         Installed installed(zone);
         DNSResolver resolver;
         std::vector<AnsiString> values;
         expect(resolver.GetRecordsOfType("plain.test", DNS_TYPE_MX, values) && values.empty(),
                "a record lookup answered NODATA, and NODATA for the CNAME: true, none");

         std::vector<String> names;
         expect(resolver.GetMXRecords(_T("plain.test"), names) && names.empty(),
                "an MX lookup answered NODATA, and NODATA for the CNAME: true, none");
      }

      {
         ScriptedDns zone;
         zone.answers[QuestionKey_(DNS_TYPE_MX, _T("alias.test"))].push_back(noRecords(nodata));
         zone.answers[QuestionKey_(cname, _T("alias.test"))].push_back(withRecord(cname, "target.test"));
         zone.answers[QuestionKey_(DNS_TYPE_MX, _T("target.test"))].push_back(withRecord(DNS_TYPE_MX, "mx.target.test"));

         Installed installed(zone);
         DNSResolver resolver;
         std::vector<String> names;
         expect(resolver.GetMXRecords(_T("alias.test"), names) && joined(names) == "mx.target.test",
                "an MX lookup of an alias: the target's exchange; found \"" + joined(names) + "\"");
      }

      expect(scripted_dns == nullptr, "the tester's answers are taken off its thread when it is done.");
   }

}

