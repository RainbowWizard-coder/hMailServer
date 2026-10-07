// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Validating DNSSEC stub resolver. See DnssecResolver.h.
//
// Failure policy:
//   - Received data that fails cryptographic validation is Bogus: the
//     host is not used (RFC 7672 section 2.1.3).
//   - A lookup that does not complete (no resolver reachable, a timeout,
//     SERVFAIL, a reply that does not parse) is Indeterminate inside the
//     chain, and so is a zone whose keys could not be fetched. For TLSA and
//     MX it stays Indeterminate unless the name is proven to sit below an
//     unsigned delegation, and the host is not used (RFC 7672 section
//     2.1.1); for TXT it reads as Insecure, and the caller falls back to the
//     system resolver as it always has. Until 6.3.6 every one of these read
//     as Insecure, so dropping a packet switched DANE off.

#include "StdAfx.h"

#include "DnssecResolver.h"
#include "DnssecCapturedAnswers.h"

// <ws2tcpip.h> is Winsock's TCP/IP header. Everything this file takes from it -
// the address structures and the address-conversion calls - comes from
// <netinet/in.h>, <arpa/inet.h> and <netdb.h> on POSIX, which the platform layer
// has already included.
#ifdef _MSC_VER
#include <ws2tcpip.h>
#endif
// <iphlpapi.h> declares GetAdaptersAddresses, which is how the nameserver list
// is discovered on Windows. There is no POSIX counterpart; see GetDnsServers
// below.
#ifdef _MSC_VER
#include <iphlpapi.h>
#endif
// The POSIX nameserver list is read out of /etc/resolv.conf in GetDnsServers
// below; these are the two standard headers that reading a text file and
// comparing a keyword in it need.
#ifdef HM_PLATFORM_POSIX
#include <cstdio>
#include <cstring>
#endif

#include <openssl/evp.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/param_build.h>
#include <openssl/core_names.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <ctime>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

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
   namespace
   {
      const unsigned short DnsTypeCname = 5;
      const unsigned short DnsTypeMx = 15;
      const unsigned short DnsTypeTxt = 16;
      const unsigned short DnsTypeDs = 43;
      const unsigned short DnsTypeRrsig = 46;
      const unsigned short DnsTypeDnskey = 48;
      const unsigned short DnsTypeTlsa = 52;
      const unsigned short DnsTypeNs = 2;
      const unsigned short DnsTypeSoa = 6;
      const unsigned short DnsTypeNsec = 47;
      const unsigned short DnsTypeNsec3 = 50;

      const unsigned short DnsTypeDname = 39;

      const DWORD DnsTimeoutMilliseconds = 5000;
      const int MaxChainDepth = 16;
      const int MaxCnameHops = 3;

      // The most labels the walk from the root asks a DS for (FindEnclosingZone).
      // A TLSA name is the host's labels and two more; a name past this is
      // answered Indeterminate rather than costing a query per label.
      const size_t MaxWalkLabels = 32;

      // How long the walk remembers what a label is: a proven unsigned
      // delegation or a name that is no zone cut, as long as an Insecure zone
      // is remembered; a signed cut as long as its keys.
      const int CutCacheSeconds = 300;
      const int SignedCutCacheSeconds = 3600;

      enum class InternalStatus
      {
         Secure,
         Insecure,
         Bogus,
         // A lookup on the way did not complete, or a zone's keys could not be
         // fetched: whether the data should have been signed cannot be told.
         Indeterminate
      };

      struct ParsedRr
      {
         AnsiString owner;             // lowercase, dotted, "" = root
         unsigned short type = 0;
         unsigned short rr_class = 0;
         unsigned long ttl = 0;
         std::vector<unsigned char> rdata;
         size_t rdata_offset = 0;      // offset of rdata within the packet
      };

      struct DnsResponse
      {
         std::vector<unsigned char> packet;
         int rcode = -1;
         std::vector<ParsedRr> answers;
         // The AUTHORITY section: where a negative answer carries the NSEC or NSEC3
         // records, and their RRSIGs, that prove the absence.
         std::vector<ParsedRr> authority;
      };

      struct RrsigInfo
      {
         unsigned short type_covered = 0;
         unsigned char algorithm = 0;
         unsigned char labels = 0;
         unsigned long original_ttl = 0;
         unsigned long expiration = 0;
         unsigned long inception = 0;
         unsigned short key_tag = 0;
         AnsiString signer;            // lowercase dotted
         std::vector<unsigned char> signature;
      };

      struct DsInfo
      {
         unsigned short key_tag = 0;
         unsigned char algorithm = 0;
         unsigned char digest_type = 0;
         std::vector<unsigned char> digest;
      };

      struct ZoneEntry
      {
         InternalStatus status = InternalStatus::Bogus;
         std::vector<std::vector<unsigned char>> dnskey_rdatas;
         time_t expires = 0;
         // Why the keys did not validate, for a status other than Secure: the
         // words a later failure under this zone is reported in while the entry
         // is remembered.
         AnsiString reason;
      };

      boost::recursive_mutex zone_cache_mutex;
      std::map<AnsiString, ZoneEntry> zone_cache;

      // What the walk from the root found a label to be (FindEnclosingZone).
      enum class CutKind
      {
         Signed,     // a zone cut with a DS, whose keys validated
         Unsigned,   // a delegation the zone above proves has no DS, or an
                     // NSEC3 Opt-Out span that may hold one
         NotACut     // a name inside the zone above, or not there at all
      };

      struct CutEntry
      {
         CutKind kind = CutKind::NotACut;
         time_t expires = 0;
      };

      boost::recursive_mutex cut_cache_mutex;
      std::map<AnsiString, CutEntry> cut_cache;

      // ----------------------------------------------------------------
      // The tester's stand-ins (DnssecResolverTester)
      //
      // Answers captured from the public DNS, keyed by the name and type a
      // query asks, and a clock fixed at the moment they were captured. While
      // one is set on a thread, that thread's queries are answered from it and
      // nothing is sent; its signatures are judged by that clock; the root's
      // keys are checked against the built-in IANA anchors whatever
      // DnssecTrustAnchors says; and its validations neither read nor fill the
      // zone and cut caches. In the service the tester runs beside real
      // deliveries, under the regression suite's own trust anchor and with
      // the suite's keys for the root in the cache, and every other thread
      // sees nothing of it.
      // ----------------------------------------------------------------

      struct TestEnvironment
      {
         std::map<std::pair<AnsiString, unsigned short>, std::vector<unsigned char>> answers;
         unsigned long now = 0;
      };

      thread_local const TestEnvironment *test_environment = nullptr;

      // Seconds since 1970, as signatures are judged: the tester's moment
      // while one is set on this thread, the system clock otherwise.
      unsigned long ValidationNow()
      {
         if (test_environment != nullptr)
            return test_environment->now;

         return static_cast<unsigned long>(time(nullptr) & 0xFFFFFFFF);
      }

      // ----------------------------------------------------------------
      // Small helpers
      // ----------------------------------------------------------------

      void Push16(std::vector<unsigned char> &out, unsigned short value)
      {
         out.push_back(static_cast<unsigned char>(value >> 8));
         out.push_back(static_cast<unsigned char>(value & 0xFF));
      }

      void Push32(std::vector<unsigned char> &out, unsigned long value)
      {
         out.push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
         out.push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
         out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
         out.push_back(static_cast<unsigned char>(value & 0xFF));
      }

      unsigned long Read32(const unsigned char *data)
      {
         return (static_cast<unsigned long>(data[0]) << 24) |
                (static_cast<unsigned long>(data[1]) << 16) |
                (static_cast<unsigned long>(data[2]) << 8) |
                 static_cast<unsigned long>(data[3]);
      }

      int CountLabels(const AnsiString &name)
      {
         if (name.IsEmpty())
            return 0;

         AnsiString copy = name;
         std::vector<AnsiString> labels = StringParser::SplitString(copy, ".");
         return static_cast<int>(labels.size());
      }

      AnsiString RightmostLabels(const AnsiString &name, int count)
      {
         AnsiString copy = name;
         std::vector<AnsiString> labels = StringParser::SplitString(copy, ".");

         AnsiString result;
         for (size_t i = labels.size() - static_cast<size_t>(count); i < labels.size(); i++)
         {
            if (!result.IsEmpty())
               result += ".";
            result += labels[i];
         }

         return result;
      }

      bool IsSubdomainOrEqual(const AnsiString &name, const AnsiString &zone)
      {
         if (zone.IsEmpty())
            return true;

         if (name == zone)
            return true;

         AnsiString suffix = ".";
         suffix += zone;

         if (name.GetLength() <= suffix.GetLength())
            return false;

         return name.Mid(name.GetLength() - suffix.GetLength()) == suffix;
      }

      // RFC 1982 serial-number time comparison: a signature's inception still to
      // come, and its expiration passed.
      bool SignatureNotYetValid(unsigned long inception)
      {
         return static_cast<long>(ValidationNow() - inception) < 0;
      }

      bool SignatureExpired(unsigned long expiration)
      {
         return static_cast<long>(expiration - ValidationNow()) < 0;
      }

      bool SignatureTimeValid(unsigned long inception, unsigned long expiration)
      {
         return !SignatureNotYetValid(inception) && !SignatureExpired(expiration);
      }

      // ----------------------------------------------------------------
      // The words a failure is logged in
      // ----------------------------------------------------------------

      AnsiString Number(unsigned long value)
      {
         return AnsiString(std::to_string(value).c_str());
      }

      // A signature time (seconds since 1970) as "2026-10-08 18:04:45 UTC".
      AnsiString DescribeTime(unsigned long seconds)
      {
         const time_t value = static_cast<time_t>(seconds);
         struct tm parts = {};
#ifdef _MSC_VER
         if (gmtime_s(&parts, &value) != 0)
            return Number(seconds);
#else
         if (gmtime_r(&value, &parts) == nullptr)
            return Number(seconds);
#endif
         char text[32] = {};
         if (strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S UTC", &parts) == 0)
            return Number(seconds);

         return AnsiString(text);
      }

      AnsiString ZoneText(const AnsiString &zone)
      {
         return zone.IsEmpty() ? AnsiString("the root") : zone;
      }

      AnsiString NameText(const AnsiString &name)
      {
         return name.IsEmpty() ? AnsiString(".") : name;
      }

      AnsiString TypeText(unsigned short type)
      {
         switch (type)
         {
         case 5: return "CNAME";
         case 15: return "MX";
         case 16: return "TXT";
         case 43: return "DS";
         case 47: return "NSEC";
         case 48: return "DNSKEY";
         case 50: return "NSEC3";
         case 52: return "TLSA";
         default: return AnsiString("TYPE") + Number(type);
         }
      }

      bool HexToBytes(const AnsiString &hex, std::vector<unsigned char> &out)
      {
         out.clear();

         if (hex.GetLength() % 2 != 0)
            return false;

         for (int i = 0; i < hex.GetLength(); i += 2)
         {
            int value = 0;

            for (int j = 0; j < 2; j++)
            {
               char c = hex[i + j];
               value <<= 4;

               if (c >= '0' && c <= '9')
                  value |= c - '0';
               else if (c >= 'a' && c <= 'f')
                  value |= c - 'a' + 10;
               else if (c >= 'A' && c <= 'F')
                  value |= c - 'A' + 10;
               else
                  return false;
            }

            out.push_back(static_cast<unsigned char>(value));
         }

         return true;
      }

      // ----------------------------------------------------------------
      // Wire format
      // ----------------------------------------------------------------

      bool AppendName(std::vector<unsigned char> &out, const AnsiString &dottedName)
      {
         if (!dottedName.IsEmpty())
         {
            AnsiString copy = dottedName;
            std::vector<AnsiString> labels = StringParser::SplitString(copy, ".");

            for (const AnsiString &label : labels)
            {
               if (label.IsEmpty() || label.GetLength() > 63)
                  return false;

               out.push_back(static_cast<unsigned char>(label.GetLength()));

               for (int i = 0; i < label.GetLength(); i++)
               {
                  char c = label[i];
                  if (c >= 'A' && c <= 'Z')
                     c = static_cast<char>(c - 'A' + 'a');

                  out.push_back(static_cast<unsigned char>(c));
               }
            }
         }

         out.push_back(0);
         return true;
      }

      // Reads a possibly compressed name. offset is advanced past the
      // name's wire representation at its original location.
      bool ReadName(const std::vector<unsigned char> &packet, size_t &offset, AnsiString &name)
      {
         name = "";

         size_t position = offset;
         bool jumped = false;
         int safety = 0;

         for (;;)
         {
            if (++safety > 128 || position >= packet.size())
               return false;

            unsigned char length = packet[position];

            if (length == 0)
            {
               if (!jumped)
                  offset = position + 1;
               return true;
            }

            if ((length & 0xC0) == 0xC0)
            {
               if (position + 1 >= packet.size())
                  return false;

               size_t target = (static_cast<size_t>(length & 0x3F) << 8) | packet[position + 1];

               if (!jumped)
                  offset = position + 2;

               if (target >= position)
                  return false; // pointers must point backwards

               position = target;
               jumped = true;
               continue;
            }

            if (length > 63 || position + 1 + length > packet.size())
               return false;

            if (!name.IsEmpty())
               name += ".";

            for (size_t i = 0; i < length; i++)
            {
               char c = static_cast<char>(packet[position + 1 + i]);
               if (c >= 'A' && c <= 'Z')
                  c = static_cast<char>(c - 'A' + 'a');
               name += c;
            }

            position += 1 + static_cast<size_t>(length);
         }
      }

      bool ParseResponse(const std::vector<unsigned char> &packet, DnsResponse &out)
      {
         if (packet.size() < 12)
            return false;

         if ((packet[2] & 0x80) == 0)
            return false; // not a response

         out.packet = packet;
         out.rcode = packet[3] & 0x0F;
         out.answers.clear();
         out.authority.clear();

         size_t questionCount = (static_cast<size_t>(packet[4]) << 8) | packet[5];
         size_t answerCount = (static_cast<size_t>(packet[6]) << 8) | packet[7];
         size_t authorityCount = (static_cast<size_t>(packet[8]) << 8) | packet[9];

         size_t offset = 12;

         AnsiString scratch;
         for (size_t i = 0; i < questionCount; i++)
         {
            if (!ReadName(packet, offset, scratch))
               return false;

            offset += 4;
            if (offset > packet.size())
               return false;
         }

         // The answer section, then the authority section: the same record shape,
         // read the same way, kept apart because they mean different things.
         for (size_t section = 0; section < 2; section++)
         {
            size_t count = section == 0 ? answerCount : authorityCount;
            std::vector<ParsedRr> &records = section == 0 ? out.answers : out.authority;
            for (size_t i = 0; i < count; i++)
            {
               ParsedRr record;

               if (!ReadName(packet, offset, record.owner))
                  return false;

               if (offset + 10 > packet.size())
                  return false;

               record.type = (static_cast<unsigned short>(packet[offset]) << 8) | packet[offset + 1];
               record.rr_class = (static_cast<unsigned short>(packet[offset + 2]) << 8) | packet[offset + 3];
               record.ttl = Read32(packet.data() + offset + 4);

               size_t rdataLength = (static_cast<size_t>(packet[offset + 8]) << 8) | packet[offset + 9];

               offset += 10;

               if (offset + rdataLength > packet.size())
                  return false;

               record.rdata_offset = offset;
               record.rdata.assign(packet.begin() + offset, packet.begin() + offset + rdataLength);

               offset += rdataLength;

            records.push_back(record);
            }
         }

         return true;
      }

      bool ParseRrsig(const DnsResponse &response, const ParsedRr &record, RrsigInfo &out)
      {
         if (record.rdata.size() < 20)
            return false;

         const unsigned char *data = record.rdata.data();

         out.type_covered = (static_cast<unsigned short>(data[0]) << 8) | data[1];
         out.algorithm = data[2];
         out.labels = data[3];
         out.original_ttl = Read32(data + 4);
         out.expiration = Read32(data + 8);
         out.inception = Read32(data + 12);
         out.key_tag = (static_cast<unsigned short>(data[16]) << 8) | data[17];

         size_t nameOffset = record.rdata_offset + 18;
         if (!ReadName(response.packet, nameOffset, out.signer))
            return false;

         size_t signatureStart = nameOffset - record.rdata_offset;
         if (signatureStart >= record.rdata.size())
            return false;

         out.signature.assign(record.rdata.begin() + signatureStart, record.rdata.end());
         return true;
      }

      // Appends the name at offset in packet to out in canonical form (RFC 4034
      // section 6.2): its labels written out wherever a pointer led, and the
      // letters A to Z in them made lowercase - octet for octet, nothing else
      // touched, so that a label holding a dot or any other octet stays the
      // label it was. offset is advanced past the name as it sits in its own
      // place, as ReadName advances it, and pointers must point backwards, as
      // there.
      bool AppendCanonicalName(const std::vector<unsigned char> &packet, size_t &offset, std::vector<unsigned char> &out)
      {
         size_t position = offset;
         bool jumped = false;
         size_t written = 0;
         int safety = 0;

         for (;;)
         {
            if (++safety > 128 || position >= packet.size())
               return false;

            const unsigned char length = packet[position];

            if ((length & 0xC0) == 0xC0)
            {
               if (position + 1 >= packet.size())
                  return false;

               const size_t target = (static_cast<size_t>(length & 0x3F) << 8) | packet[position + 1];

               if (!jumped)
                  offset = position + 2;

               if (target >= position)
                  return false;

               position = target;
               jumped = true;
               continue;
            }

            // The other label types (RFC 6891 section 5) are not names this
            // resolver reads, and a label past the end is no label.
            if ((length & 0xC0) != 0 || position + 1 + length > packet.size())
               return false;

            // RFC 1035 section 2.3.4: 255 octets at most, the root label counted.
            written += 1 + static_cast<size_t>(length);
            if (written > 255)
               return false;

            out.push_back(length);

            if (length == 0)
            {
               if (!jumped)
                  offset = position + 1;
               return true;
            }

            for (size_t i = 0; i < length; i++)
            {
               unsigned char c = packet[position + 1 + i];
               if (c >= 'A' && c <= 'Z')
                  c = static_cast<unsigned char>(c - 'A' + 'a');
               out.push_back(c);
            }

            position += 1 + static_cast<size_t>(length);
         }
      }

      // The RDATA of a record in the canonical form of RFC 4034 section 6.2,
      // which is what an RRSIG signs: every domain name in it written out in
      // full, with no compression pointer, and in lowercase.
      //
      // RFC 1035 section 4.1.4 lets a server compress the names in the RDATA of
      // the types it defined - NS, CNAME, SOA, PTR, MX and the rest - and RFC
      // 3597 section 4 keeps that for them. Every resolver measured does it for
      // MX: live.com's exchange, live-com.olc.protection.outlook.com, arrives as
      // its first four labels and a pointer to the "com" of the question, and
      // cloudflare.com's later exchanges as a label or two and a pointer into
      // the first one's RDATA. Until work item 80 only a CNAME's target was
      // written out here, so the RRSIG over a compressed MX RRset was checked
      // against octets nobody signed, and every signed domain whose MX answer
      // came compressed - nearly every one - was Bogus: with DANE enforcement
      // on, its mail was deferred and no host was tried.
      //
      // The types section 6.2 lists are the ones RFC 6840 section 5.1 leaves
      // there: NSEC is not among them (its next name is never compressed, and
      // not lowercased), nor are the types with no name in their RDATA - A,
      // TXT, DS, DNSKEY, NSEC3, TLSA - whose RDATA is taken as received. NAPTR,
      // whose name follows three character-strings, is never validated here.
      bool GetCanonicalRdata(const DnsResponse &response, const ParsedRr &record, std::vector<unsigned char> &out)
      {
         // The octets ahead of the first name, copied as they are; the names,
         // one after another; and whatever follows them, copied as it is.
         size_t before = 0;
         int names = 0;

         switch (record.type)
         {
         case 2:  // NS
         case 3:  // MD
         case 4:  // MF
         case 5:  // CNAME
         case 7:  // MB
         case 8:  // MG
         case 9:  // MR
         case 12: // PTR
         case 39: // DNAME
            names = 1;
            break;
         case 6:  // SOA: two names, then five 32-bit numbers
         case 14: // MINFO
         case 17: // RP
            names = 2;
            break;
         case 15: // MX: a 16-bit preference, then the exchange
         case 18: // AFSDB
         case 21: // RT
         case 36: // KX
            before = 2;
            names = 1;
            break;
         case 26: // PX: a preference, then two names
            before = 2;
            names = 2;
            break;
         case 33: // SRV: priority, weight and port, then the target
            before = 6;
            names = 1;
            break;
         default:
            out = record.rdata;
            return true;
         }

         const size_t end = record.rdata_offset + record.rdata.size();
         if (record.rdata.size() < before || end > response.packet.size())
            return false;

         out.assign(record.rdata.begin(), record.rdata.begin() + static_cast<std::ptrdiff_t>(before));

         size_t offset = record.rdata_offset + before;
         for (int i = 0; i < names; i++)
         {
            // A name starts inside the RDATA and ends there; a pointer in it may
            // lead anywhere earlier in the packet.
            if (offset >= end || !AppendCanonicalName(response.packet, offset, out) || offset > end)
               return false;
         }

         out.insert(out.end(), response.packet.begin() + static_cast<std::ptrdiff_t>(offset),
                    response.packet.begin() + static_cast<std::ptrdiff_t>(end));
         return true;
      }

      // ----------------------------------------------------------------
      // Transport
      // ----------------------------------------------------------------

      // A name server this resolver asks: an address of either family, port 53.
      struct NameServer
      {
         sockaddr_storage address = {};
         int length = 0;
      };

      // text - an IPv4 or IPv6 address, an IPv6 one with its %zone - as a name
      // server on port 53; false for anything that is not an address. Numeric
      // only (AI_NUMERICHOST): nothing is looked up.
      bool NameServerFromText(const std::string &text, NameServer &server)
      {
         if (text.empty() || text.size() > 63)
            return false;

         addrinfo hints = {};
         hints.ai_family = AF_UNSPEC;
         hints.ai_socktype = SOCK_DGRAM;
         hints.ai_flags = AI_NUMERICHOST;

         addrinfo *found = nullptr;
         if (getaddrinfo(text.c_str(), "53", &hints, &found) != 0 || found == nullptr)
            return false;

         bool usable = false;
         if ((found->ai_family == AF_INET || found->ai_family == AF_INET6) &&
             found->ai_addr != nullptr && found->ai_addrlen <= sizeof(server.address))
         {
            server = NameServer();
            memcpy(&server.address, found->ai_addr, found->ai_addrlen);
            server.length = static_cast<int>(found->ai_addrlen);
            usable = true;
         }

         freeaddrinfo(found);
         return usable;
      }

      // The 16 octets of an IPv6 name server's address.
      const unsigned char *IPv6Octets(const NameServer &server)
      {
         return reinterpret_cast<const unsigned char *>(&reinterpret_cast<const sockaddr_in6 *>(&server.address)->sin6_addr);
      }

      // An IPv4 address written as IPv6 (::ffff:a.b.c.d) as the IPv4 address it
      // is: an IPv6 socket reaches one only where IPV6_V6ONLY is off, which on
      // Windows it is not.
      void UnmapIPv4(NameServer &server)
      {
         if (server.address.ss_family != AF_INET6)
            return;

         static const unsigned char mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
         const unsigned char *octets = IPv6Octets(server);
         if (memcmp(octets, mapped, sizeof(mapped)) != 0)
            return;

         sockaddr_in v4 = {};
         v4.sin_family = AF_INET;
         v4.sin_port = reinterpret_cast<const sockaddr_in6 *>(&server.address)->sin6_port;
         memcpy(&v4.sin_addr, octets + 12, 4);

         server = NameServer();
         memcpy(&server.address, &v4, sizeof(v4));
         server.length = static_cast<int>(sizeof(v4));
      }

      // The site-local addresses (fec0::/10, deprecated by RFC 3879) that Windows
      // lists as the DNS servers of an adapter given no IPv6 one -
      // fec0:0:0:ffff::1, ::2 and ::3 - and that nothing answers on.
      bool IsSiteLocal(const NameServer &server)
      {
         if (server.address.ss_family != AF_INET6)
            return false;

         const unsigned char *octets = IPv6Octets(server);
         return octets[0] == 0xfe && (octets[1] & 0xc0) == 0xc0;
      }

      // The servers to ask, from the ones the system lists, in its order: its
      // first two IPv4 servers, as this resolver has always asked; and only when
      // it lists none, its first two IPv6 servers. Until 6.3.6 an IPv6 server was
      // never asked. That cost the Windows service nothing while a lookup that
      // reached no server read as unsigned, and since 6.3.6 such a lookup is
      // indeterminate and holds the mail up - on a host whose DNS servers are
      // IPv6 alone, every message; and on Linux, where the plain lookups go
      // through this transport too (QueryRecords), such a host had no DNS at
      // all. A host that lists an IPv4 server asks what it asked before.
      std::vector<NameServer> ChooseNameServers(const std::vector<NameServer> &listed)
      {
         std::vector<NameServer> chosen;
         const int families[] = { AF_INET, AF_INET6 };

         for (int family : families)
         {
            for (NameServer server : listed)
            {
               UnmapIPv4(server);
               if (server.address.ss_family != family || IsSiteLocal(server))
                  continue;

               chosen.push_back(server);
               if (chosen.size() >= 2)
                  return chosen;
            }

            if (!chosen.empty())
               return chosen;
         }

         return chosen;
      }

      // The name servers /etc/resolv.conf lists, in its order. "nameserver" has
      // to be the whole keyword rather than the start of a longer one, so what
      // follows it must be white space, and the address ends at white space or
      // at a comment. A line that does not name an address is passed over.
      std::vector<NameServer> ReadResolvConf(const std::string &text)
      {
         std::vector<NameServer> listed;

         size_t lineStart = 0;
         while (lineStart < text.size())
         {
            size_t lineEnd = text.find('\n', lineStart);
            if (lineEnd == std::string::npos)
               lineEnd = text.size();

            const std::string line = text.substr(lineStart, lineEnd - lineStart);
            lineStart = lineEnd + 1;

            size_t cursor = line.find_first_not_of(" \t");
            if (cursor == std::string::npos || line.compare(cursor, 10, "nameserver") != 0)
               continue;

            cursor += 10;
            if (cursor >= line.size() || (line[cursor] != ' ' && line[cursor] != '\t'))
               continue;

            cursor = line.find_first_not_of(" \t", cursor);
            if (cursor == std::string::npos)
               continue;

            const size_t addressEnd = line.find_first_of(" \t\r#;", cursor);
            NameServer server;
            if (NameServerFromText(line.substr(cursor, addressEnd == std::string::npos ? std::string::npos : addressEnd - cursor), server))
               listed.push_back(server);
         }

         return listed;
      }

      bool GetDnsServers(std::vector<NameServer> &servers)
      {
         servers.clear();

         // DNSServer names an IPv4 address, as the system resolver's side reads
         // it too (DNSResolverWinApi).
         String configuredServer = IniFileSettings::Instance()->GetDNSServer();
         if (!configuredServer.IsEmpty())
         {
            AnsiString narrow = configuredServer;

            sockaddr_in address = {};
            address.sin_family = AF_INET;
            address.sin_port = htons(53);

            if (inet_pton(AF_INET, narrow.c_str(), &address.sin_addr) == 1)
            {
               NameServer server;
               memcpy(&server.address, &address, sizeof(address));
               server.length = static_cast<int>(sizeof(address));
               servers.push_back(server);
               return true;
            }
         }

         std::vector<NameServer> listed;

#ifdef _MSC_VER
         ULONG bufferSize = 16 * 1024;
         std::vector<unsigned char> buffer(bufferSize);

         ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_FRIENDLY_NAME | GAA_FLAG_SKIP_UNICAST;

         // Both families: an adapter's list holds its IPv4 and its IPv6 servers,
         // and ChooseNameServers takes the IPv4 ones first.
         ULONG queryResult = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bufferSize);

         if (queryResult == ERROR_BUFFER_OVERFLOW)
         {
            buffer.resize(bufferSize);
            queryResult = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bufferSize);
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
               const sockaddr *address = dns->Address.lpSockaddr;
               if (address == nullptr || dns->Address.iSockaddrLength <= 0 ||
                   static_cast<size_t>(dns->Address.iSockaddrLength) > sizeof(sockaddr_storage))
                  continue;

               NameServer server;
               if (address->sa_family == AF_INET && static_cast<size_t>(dns->Address.iSockaddrLength) >= sizeof(sockaddr_in))
               {
                  memcpy(&server.address, address, sizeof(sockaddr_in));
                  reinterpret_cast<sockaddr_in *>(&server.address)->sin_port = htons(53);
                  server.length = static_cast<int>(sizeof(sockaddr_in));
               }
               else if (address->sa_family == AF_INET6 && static_cast<size_t>(dns->Address.iSockaddrLength) >= sizeof(sockaddr_in6))
               {
                  memcpy(&server.address, address, sizeof(sockaddr_in6));
                  reinterpret_cast<sockaddr_in6 *>(&server.address)->sin6_port = htons(53);
                  server.length = static_cast<int>(sizeof(sockaddr_in6));
               }
               else
               {
                  continue;
               }

               listed.push_back(server);
            }
         }
#else
         // /etc/resolv.conf is where a POSIX system records the nameservers it
         // uses, so reading it is the counterpart of GetAdaptersAddresses above.
         // Leaving it unwritten would have been worse than it looks - every
         // DNSSEC query would have failed for want of a server, and a validator
         // that never runs is indistinguishable from one that always passes.
         FILE *resolv_conf = ::fopen("/etc/resolv.conf", "r");

         if (resolv_conf != nullptr)
         {
            std::string text;
            char chunk[4096];

            // A resolv.conf is a few hundred octets; past 64 KB the rest is not
            // read. A short read is the end of the file, or an error: either way
            // the stream is not read again.
            for (;;)
            {
               const size_t read = ::fread(chunk, 1, sizeof(chunk), resolv_conf);
               text.append(chunk, read);

               if (read < sizeof(chunk) || text.size() >= 65536)
                  break;
            }

            ::fclose(resolv_conf);
            listed = ReadResolvConf(text);
         }
#endif

         servers = ChooseNameServers(listed);
         return !servers.empty();
      }

      bool BuildQuery(const AnsiString &name, unsigned short queryType, std::vector<unsigned char> &query,
                      unsigned char transactionId[2])
      {
         if (RAND_bytes(transactionId, 2) != 1)
         {
            transactionId[0] = static_cast<unsigned char>(GetTickCount() & 0xFF);
            transactionId[1] = static_cast<unsigned char>((GetTickCount() >> 8) & 0xFF);
         }

         query.clear();
         query.reserve(64 + name.GetLength());

         query.push_back(transactionId[0]);
         query.push_back(transactionId[1]);
         query.push_back(0x01); // RD
         query.push_back(0x10); // CD - we validate ourselves
         query.push_back(0x00); query.push_back(0x01); // QDCOUNT 1
         query.push_back(0x00); query.push_back(0x00); // ANCOUNT 0
         query.push_back(0x00); query.push_back(0x00); // NSCOUNT 0
         query.push_back(0x00); query.push_back(0x01); // ARCOUNT 1 (OPT)

         if (!AppendName(query, name))
            return false;

         Push16(query, queryType);
         Push16(query, 1); // IN

         // EDNS0 OPT with DO bit, UDP payload size 1232.
         query.push_back(0x00);                        // root name
         Push16(query, 41);                            // TYPE OPT
         Push16(query, 1232);                          // CLASS = UDP size
         query.push_back(0x00);                        // extended RCODE
         query.push_back(0x00);                        // version
         query.push_back(0x80); query.push_back(0x00); // flags: DO
         Push16(query, 0);                             // RDLEN

         return true;
      }

      // The receive and send deadlines of a query socket. Winsock takes the
      // milliseconds as a DWORD; POSIX takes a timeval, and refuses the DWORD
      // with EINVAL because it is too short - which left the socket with no
      // deadline at all, and a name server that did not answer held the lookup
      // thread for ever. That is what the first Linux run of the suite's fake
      // zone found: every lookup queue thread stuck in recvfrom, StopServers
      // waiting on them at each reinitialise, and the SMTP banner behind them.
      void SetSocketTimeouts(SOCKET querySocket)
      {
#ifdef HM_PLATFORM_POSIX
         struct timeval timeout;
         timeout.tv_sec = DnsTimeoutMilliseconds / 1000;
         timeout.tv_usec = (DnsTimeoutMilliseconds % 1000) * 1000;
#else
         DWORD timeout = DnsTimeoutMilliseconds;
#endif
         setsockopt(querySocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
         setsockopt(querySocket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
      }

      // Whether a reply came from the server it was sent to.
      bool FromServer(const sockaddr_storage &from, const NameServer &server)
      {
         if (from.ss_family != server.address.ss_family)
            return false;

         if (from.ss_family == AF_INET)
            return reinterpret_cast<const sockaddr_in *>(&from)->sin_addr.s_addr ==
                   reinterpret_cast<const sockaddr_in *>(&server.address)->sin_addr.s_addr;

         if (from.ss_family == AF_INET6)
            return memcmp(&reinterpret_cast<const sockaddr_in6 *>(&from)->sin6_addr,
                          &reinterpret_cast<const sockaddr_in6 *>(&server.address)->sin6_addr, 16) == 0;

         return false;
      }

      bool RunUdpQuery(const NameServer &server, const std::vector<unsigned char> &query,
                       std::vector<unsigned char> &response, bool &truncated)
      {
         truncated = false;

         SOCKET udpSocket = socket(server.address.ss_family, SOCK_DGRAM, IPPROTO_UDP);
         if (udpSocket == INVALID_SOCKET)
            return false;

         SetSocketTimeouts(udpSocket);

         bool succeeded = false;

         if (sendto(udpSocket, reinterpret_cast<const char*>(query.data()), static_cast<int>(query.size()), 0,
                    reinterpret_cast<const sockaddr*>(&server.address), server.length) == static_cast<int>(query.size()))
         {
            unsigned char receiveBuffer[2048];

            sockaddr_storage fromAddress = {};
            // recvfrom's last argument is an int* on Winsock and a socklen_t* on
            // POSIX, and they are not the same type even where they are the same
            // width, so the variable is declared as whatever the platform's
            // recvfrom is going to write through. The Windows declaration is the
            // one this file has always had.
#ifdef HM_PLATFORM_POSIX
            socklen_t fromLength = sizeof(fromAddress);
#else
            int fromLength = sizeof(fromAddress);
#endif

            int bytesReceived = recvfrom(udpSocket, reinterpret_cast<char*>(receiveBuffer), sizeof(receiveBuffer), 0,
                                         reinterpret_cast<sockaddr*>(&fromAddress), &fromLength);

            if (bytesReceived >= 12 &&
                receiveBuffer[0] == query[0] &&
                receiveBuffer[1] == query[1] &&
                FromServer(fromAddress, server))
            {
               truncated = (receiveBuffer[2] & 0x02) != 0;
               response.assign(receiveBuffer, receiveBuffer + bytesReceived);
               succeeded = true;
            }
         }

         closesocket(udpSocket);
         return succeeded;
      }

      bool RunTcpQuery(const NameServer &server, const std::vector<unsigned char> &query,
                       std::vector<unsigned char> &response)
      {
         SOCKET tcpSocket = socket(server.address.ss_family, SOCK_STREAM, IPPROTO_TCP);
         if (tcpSocket == INVALID_SOCKET)
            return false;

         SetSocketTimeouts(tcpSocket);

         bool succeeded = false;

         if (connect(tcpSocket, reinterpret_cast<const sockaddr*>(&server.address), server.length) == 0)
         {
            std::vector<unsigned char> framed;
            framed.reserve(query.size() + 2);
            Push16(framed, static_cast<unsigned short>(query.size()));
            framed.insert(framed.end(), query.begin(), query.end());

            if (send(tcpSocket, reinterpret_cast<const char*>(framed.data()), static_cast<int>(framed.size()), 0) ==
                static_cast<int>(framed.size()))
            {
               unsigned char lengthBytes[2];
               int received = recv(tcpSocket, reinterpret_cast<char*>(lengthBytes), 2, 0);

               if (received == 2)
               {
                  size_t expected = (static_cast<size_t>(lengthBytes[0]) << 8) | lengthBytes[1];

                  if (expected >= 12 && expected <= 65535)
                  {
                     response.resize(expected);

                     size_t total = 0;
                     while (total < expected)
                     {
                        int chunk = recv(tcpSocket, reinterpret_cast<char*>(response.data() + total),
                                         static_cast<int>(expected - total), 0);
                        if (chunk <= 0)
                           break;

                        total += static_cast<size_t>(chunk);
                     }

                     if (total == expected &&
                         response[0] == query[0] &&
                         response[1] == query[1])
                     {
                        succeeded = true;
                     }
                  }
               }
            }
         }

         closesocket(tcpSocket);
         return succeeded;
      }

      // Returns true with a parsed NOERROR/NXDOMAIN response; false on
      // any transport-level failure.
      bool RunQuery(const AnsiString &name, unsigned short queryType, DnsResponse &out)
      {
         if (test_environment != nullptr)
         {
            // The tester's captured answers: one the tester has not got is a
            // lookup that did not complete, as no answer from a server is.
            auto captured = test_environment->answers.find(std::make_pair(name, queryType));
            if (captured == test_environment->answers.end())
               return false;

            DnsResponse response;
            if (!ParseResponse(captured->second, response) || (response.rcode != 0 && response.rcode != 3))
               return false;

            out = response;
            return true;
         }

         std::vector<unsigned char> query;
         unsigned char transactionId[2];

         if (!BuildQuery(name, queryType, query, transactionId))
            return false;

         std::vector<NameServer> servers;
         if (!GetDnsServers(servers))
            return false;

         for (const NameServer &server : servers)
         {
            std::vector<unsigned char> packet;
            bool truncated = false;

            bool received = RunUdpQuery(server, query, packet, truncated);

            if (received && truncated)
               received = RunTcpQuery(server, query, packet);

            if (!received)
               continue;

            DnsResponse response;
            if (!ParseResponse(packet, response))
               continue;

            if (response.rcode != 0 && response.rcode != 3)
               continue;

            out = response;
            return true;
         }

         return false;
      }

      // ----------------------------------------------------------------
      // Cryptography
      // ----------------------------------------------------------------

      unsigned short ComputeKeyTag(const std::vector<unsigned char> &rdata)
      {
         unsigned long accumulator = 0;

         for (size_t i = 0; i < rdata.size(); i++)
            accumulator += (i & 1) ? rdata[i] : (static_cast<unsigned long>(rdata[i]) << 8);

         accumulator += (accumulator >> 16) & 0xFFFF;

         return static_cast<unsigned short>(accumulator & 0xFFFF);
      }

      EVP_PKEY* BuildKeyFromParams(const char *keyType, OSSL_PARAM *params)
      {
         EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_from_name(nullptr, keyType, nullptr);
         if (context == nullptr)
            return nullptr;

         EVP_PKEY *key = nullptr;

         if (EVP_PKEY_fromdata_init(context) != 1 ||
             EVP_PKEY_fromdata(context, &key, EVP_PKEY_PUBLIC_KEY, params) != 1)
         {
            key = nullptr;
         }

         EVP_PKEY_CTX_free(context);
         return key;
      }

      EVP_PKEY* BuildRsaKey(const unsigned char *keyData, size_t keyLength)
      {
         if (keyLength < 3)
            return nullptr;

         size_t exponentLength = keyData[0];
         size_t offset = 1;

         if (exponentLength == 0)
         {
            if (keyLength < 4)
               return nullptr;

            exponentLength = (static_cast<size_t>(keyData[1]) << 8) | keyData[2];
            offset = 3;
         }

         if (exponentLength == 0 || offset + exponentLength >= keyLength)
            return nullptr;

         BIGNUM *exponent = BN_bin2bn(keyData + offset, static_cast<int>(exponentLength), nullptr);
         BIGNUM *modulus = BN_bin2bn(keyData + offset + exponentLength,
                                     static_cast<int>(keyLength - offset - exponentLength), nullptr);

         EVP_PKEY *key = nullptr;

         OSSL_PARAM_BLD *builder = OSSL_PARAM_BLD_new();
         if (builder != nullptr && exponent != nullptr && modulus != nullptr &&
             OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N, modulus) == 1 &&
             OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_E, exponent) == 1)
         {
            OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(builder);
            if (params != nullptr)
            {
               key = BuildKeyFromParams("RSA", params);
               OSSL_PARAM_free(params);
            }
         }

         if (builder != nullptr)
            OSSL_PARAM_BLD_free(builder);
         if (modulus != nullptr)
            BN_free(modulus);
         if (exponent != nullptr)
            BN_free(exponent);

         return key;
      }

      EVP_PKEY* BuildEcKey(const unsigned char *keyData, size_t keyLength, const char *groupName, size_t coordinateSize)
      {
         if (keyLength != coordinateSize * 2)
            return nullptr;

         std::vector<unsigned char> point;
         point.reserve(1 + keyLength);
         point.push_back(0x04);
         point.insert(point.end(), keyData, keyData + keyLength);

         EVP_PKEY *key = nullptr;

         OSSL_PARAM_BLD *builder = OSSL_PARAM_BLD_new();
         if (builder != nullptr &&
             OSSL_PARAM_BLD_push_utf8_string(builder, OSSL_PKEY_PARAM_GROUP_NAME, groupName, 0) == 1 &&
             OSSL_PARAM_BLD_push_octet_string(builder, OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size()) == 1)
         {
            OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(builder);
            if (params != nullptr)
            {
               key = BuildKeyFromParams("EC", params);
               OSSL_PARAM_free(params);
            }
         }

         if (builder != nullptr)
            OSSL_PARAM_BLD_free(builder);

         return key;
      }

      bool EcdsaRawToDer(const std::vector<unsigned char> &raw, std::vector<unsigned char> &der)
      {
         if (raw.empty() || raw.size() % 2 != 0)
            return false;

         size_t half = raw.size() / 2;

         ECDSA_SIG *signature = ECDSA_SIG_new();
         if (signature == nullptr)
            return false;

         BIGNUM *r = BN_bin2bn(raw.data(), static_cast<int>(half), nullptr);
         BIGNUM *s = BN_bin2bn(raw.data() + half, static_cast<int>(half), nullptr);

         bool succeeded = false;

         if (r != nullptr && s != nullptr && ECDSA_SIG_set0(signature, r, s) == 1)
         {
            // signature owns r and s now.
            int derLength = i2d_ECDSA_SIG(signature, nullptr);
            if (derLength > 0)
            {
               der.resize(static_cast<size_t>(derLength));
               unsigned char *writePointer = der.data();
               i2d_ECDSA_SIG(signature, &writePointer);
               succeeded = true;
            }
         }
         else
         {
            if (r != nullptr)
               BN_free(r);
            if (s != nullptr)
               BN_free(s);
         }

         ECDSA_SIG_free(signature);
         return succeeded;
      }

      bool VerifySignature(unsigned char algorithm, const unsigned char *keyData, size_t keyLength,
                           const std::vector<unsigned char> &signedData,
                           const std::vector<unsigned char> &signature)
      {
         EVP_PKEY *key = nullptr;
         const EVP_MD *digest = nullptr;
         std::vector<unsigned char> derSignature;
         const unsigned char *signatureData = signature.data();
         size_t signatureLength = signature.size();

         switch (algorithm)
         {
         case 8: // RSA/SHA-256
            key = BuildRsaKey(keyData, keyLength);
            digest = EVP_sha256();
            break;

         case 10: // RSA/SHA-512
            key = BuildRsaKey(keyData, keyLength);
            digest = EVP_sha512();
            break;

         case 13: // ECDSA P-256 / SHA-256
            key = BuildEcKey(keyData, keyLength, "prime256v1", 32);
            digest = EVP_sha256();
            if (!EcdsaRawToDer(signature, derSignature))
               key = nullptr;
            signatureData = derSignature.data();
            signatureLength = derSignature.size();
            break;

         case 14: // ECDSA P-384 / SHA-384
            key = BuildEcKey(keyData, keyLength, "secp384r1", 48);
            digest = EVP_sha384();
            if (!EcdsaRawToDer(signature, derSignature))
               key = nullptr;
            signatureData = derSignature.data();
            signatureLength = derSignature.size();
            break;

         case 15: // Ed25519
            if (keyLength != 32 || signature.size() != 64)
               return false;
            key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, keyData, keyLength);
            digest = nullptr;
            break;

         default:
            return false; // unsupported algorithm
         }

         if (key == nullptr)
            return false;

         bool verified = false;

         EVP_MD_CTX *context = EVP_MD_CTX_new();
         if (context != nullptr)
         {
            if (EVP_DigestVerifyInit(context, nullptr, digest, nullptr, key) == 1)
            {
               verified = EVP_DigestVerify(context, signatureData, signatureLength,
                                           signedData.data(), signedData.size()) == 1;
            }

            EVP_MD_CTX_free(context);
         }

         EVP_PKEY_free(key);
         return verified;
      }

      // What checking one RRSIG over an RRset with one key found.
      enum class SignatureCheck
      {
         Verified,
         NotYetValid,     // its inception is still to come by this server's clock
         Expired,         // its expiration has passed by this server's clock
         TooManyLabels,   // it counts more labels than the RRset's owner has
         Unreadable,      // a record of the RRset, or the key, could not be read
         Mismatch         // the signature does not match the RRset in canonical form under that key
      };

      // Checks one RRSIG over an RRset (RFC 4034 section 3.1.8.1) with one key.
      SignatureCheck CheckRrsigOverSet(const DnsResponse &response, const std::vector<ParsedRr> &rrset,
                                       const RrsigInfo &sig, const std::vector<unsigned char> &dnskeyRdata)
      {
         if (rrset.empty() || dnskeyRdata.size() < 5)
            return SignatureCheck::Unreadable;

         if (SignatureNotYetValid(sig.inception))
            return SignatureCheck::NotYetValid;

         if (SignatureExpired(sig.expiration))
            return SignatureCheck::Expired;

         const AnsiString &owner = rrset[0].owner;
         int ownerLabels = CountLabels(owner);

         if (sig.labels > ownerLabels)
            return SignatureCheck::TooManyLabels;

         AnsiString canonicalOwner = owner;
         if (sig.labels < ownerLabels)
         {
            canonicalOwner = "*.";
            canonicalOwner += RightmostLabels(owner, sig.labels);
         }

         // Signed data: RRSIG RDATA (sans signature, signer canonical)
         // followed by the canonical, sorted RRs.
         std::vector<unsigned char> signedData;
         signedData.reserve(512);

         Push16(signedData, sig.type_covered);
         signedData.push_back(sig.algorithm);
         signedData.push_back(sig.labels);
         Push32(signedData, sig.original_ttl);
         Push32(signedData, sig.expiration);
         Push32(signedData, sig.inception);
         Push16(signedData, sig.key_tag);

         if (!AppendName(signedData, sig.signer))
            return SignatureCheck::Unreadable;

         std::vector<std::vector<unsigned char>> canonicalRdatas;
         for (const ParsedRr &record : rrset)
         {
            std::vector<unsigned char> rdata;
            if (!GetCanonicalRdata(response, record, rdata))
               return SignatureCheck::Unreadable;

            canonicalRdatas.push_back(rdata);
         }

         // RFC 4034 section 6.3: in canonical order, and a record that is there
         // twice once the names in it are written out and lowercased counted
         // once, as the signer counted it.
         std::sort(canonicalRdatas.begin(), canonicalRdatas.end());
         canonicalRdatas.erase(std::unique(canonicalRdatas.begin(), canonicalRdatas.end()), canonicalRdatas.end());

         for (const std::vector<unsigned char> &rdata : canonicalRdatas)
         {
            if (!AppendName(signedData, canonicalOwner))
               return SignatureCheck::Unreadable;

            Push16(signedData, rrset[0].type);
            Push16(signedData, rrset[0].rr_class);
            Push32(signedData, sig.original_ttl);
            Push16(signedData, static_cast<unsigned short>(rdata.size()));
            signedData.insert(signedData.end(), rdata.begin(), rdata.end());
         }

         return VerifySignature(sig.algorithm, dnskeyRdata.data() + 4, dnskeyRdata.size() - 4,
                                signedData, sig.signature) ? SignatureCheck::Verified : SignatureCheck::Mismatch;
      }

      bool VerifyRrsigOverSet(const DnsResponse &response, const std::vector<ParsedRr> &rrset,
                              const RrsigInfo &sig, const std::vector<unsigned char> &dnskeyRdata)
      {
         return CheckRrsigOverSet(response, rrset, sig, dnskeyRdata) == SignatureCheck::Verified;
      }

      // "the RRSIG by live.com with key tag 44331 (algorithm 13)"
      AnsiString DescribeSignature(const RrsigInfo &sig)
      {
         return AnsiString("the RRSIG by ") + ZoneText(sig.signer) + " with key tag " + Number(sig.key_tag) +
                " (algorithm " + Number(sig.algorithm) + ")";
      }

      // What a failed check found, to follow DescribeSignature.
      AnsiString DescribeCheck(SignatureCheck check, const RrsigInfo &sig)
      {
         switch (check)
         {
         case SignatureCheck::Verified:
            return "verifies";
         case SignatureCheck::NotYetValid:
            return AnsiString("is not valid until ") + DescribeTime(sig.inception) + ", and this server's clock says " +
                   DescribeTime(ValidationNow());
         case SignatureCheck::Expired:
            return AnsiString("expired at ") + DescribeTime(sig.expiration) + ", and this server's clock says " +
                   DescribeTime(ValidationNow());
         case SignatureCheck::TooManyLabels:
            return AnsiString("counts ") + Number(sig.labels) + " labels, more than the name it covers has";
         case SignatureCheck::Unreadable:
            return "covers a record, or names a key, that could not be read";
         case SignatureCheck::Mismatch:
            return "does not match the records as received, in canonical form (RFC 4034 section 6.2)";
         }

         return "failed";
      }

      bool DnskeyMatchesDs(const AnsiString &zone, const std::vector<unsigned char> &dnskeyRdata, const DsInfo &ds)
      {
         std::vector<unsigned char> input;
         input.reserve(zone.GetLength() + 2 + dnskeyRdata.size());

         if (!AppendName(input, zone))
            return false;

         input.insert(input.end(), dnskeyRdata.begin(), dnskeyRdata.end());

         unsigned char computed[SHA512_DIGEST_LENGTH];
         size_t digestLength = 0;

         if (ds.digest_type == 2)
         {
            SHA256(input.data(), input.size(), computed);
            digestLength = SHA256_DIGEST_LENGTH;
         }
         else if (ds.digest_type == 4)
         {
            SHA384(input.data(), input.size(), computed);
            digestLength = SHA384_DIGEST_LENGTH;
         }
         else
         {
            return false; // unsupported digest (including SHA-1)
         }

         if (ds.digest.size() != digestLength)
            return false;

         return memcmp(ds.digest.data(), computed, digestLength) == 0;
      }

      // ----------------------------------------------------------------
      // Trust anchors
      // ----------------------------------------------------------------

      std::vector<DsInfo> GetRootTrustAnchors()
      {
         std::vector<DsInfo> anchors;

         // Optional override / extension from hMailServer.ini, format:
         // DnssecTrustAnchors=20326 8 2 E06D...;38696 8 2 683D...
         // Not for the tester's thread, whose captured answers are the real
         // root's (TestEnvironment).
         AnsiString configured = test_environment != nullptr ? AnsiString() : AnsiString(IniFileSettings::Instance()->GetDnssecTrustAnchors());
         if (!configured.IsEmpty())
         {
            std::vector<AnsiString> entries = StringParser::SplitString(configured, ";");
            for (AnsiString entry : entries)
            {
               entry.Trim();
               if (entry.IsEmpty())
                  continue;

               std::vector<AnsiString> fields = StringParser::SplitString(entry, " ");

               std::vector<AnsiString> cleanFields;
               for (AnsiString field : fields)
               {
                  field.Trim();
                  if (!field.IsEmpty())
                     cleanFields.push_back(field);
               }

               if (cleanFields.size() != 4)
                  continue;

               DsInfo anchor;
               anchor.key_tag = static_cast<unsigned short>(atoi(cleanFields[0].c_str()));
               anchor.algorithm = static_cast<unsigned char>(atoi(cleanFields[1].c_str()));
               anchor.digest_type = static_cast<unsigned char>(atoi(cleanFields[2].c_str()));

               if (HexToBytes(cleanFields[3], anchor.digest))
                  anchors.push_back(anchor);
            }
         }

         if (!anchors.empty())
            return anchors;

         // Built-in IANA root key signing keys (root-anchors.xml).
         struct BuiltinAnchor { unsigned short tag; const char *digest; };
         const BuiltinAnchor builtin[] =
         {
            { 20326, "E06D44B80B8F1D39A95C0B0D7C65D08458E880409BBC683457104237C7F8EC8D" }, // KSK-2017
            { 38696, "683D2D0ACB8C9B712A1948B27F741219298D0A450D612C483AF444A4C0FB2B16" }  // KSK-2024
         };

         for (const BuiltinAnchor &entry : builtin)
         {
            DsInfo anchor;
            anchor.key_tag = entry.tag;
            anchor.algorithm = 8;     // RSA/SHA-256
            anchor.digest_type = 2;   // SHA-256

            if (HexToBytes(AnsiString(entry.digest), anchor.digest))
               anchors.push_back(anchor);
         }

         return anchors;
      }

      // ----------------------------------------------------------------
      // Chain validation
      // ----------------------------------------------------------------

      // why, when given, is set to why the keys did not validate (a status
      // other than Secure), in the words the debug log has for it.
      InternalStatus ValidateZoneKeys(const AnsiString &zone, int depth, std::vector<std::vector<unsigned char>> &keys,
                                      AnsiString *why = nullptr);

      // ----------------------------------------------------------------
      // Denial of existence (RFC 4035 section 5.2, RFC 5155 section 8)
      //
      // A DS RRset that is absent has to be proved absent by the parent zone
      // before "no DS" may be read as "an unsigned delegation": an NSEC or an
      // NSEC3 in the authority section, signed by the parent, whose owner is
      // the delegation name (or its hash) with NS set and DS clear - or, for
      // NSEC3, a record with the Opt-Out flag covering the hashed name. Without
      // the proof, "no DS" is exactly what a resolver would be shown by anyone
      // who took the record out of the answer, and the whole point of DNSSEC
      // is that the record cannot be taken out unnoticed. So the delegation is
      // then Bogus, not Insecure.
      // ----------------------------------------------------------------

      // RFC 4034 4.1.2: is a type present in the window-block bitmap that starts
      // at rdata[start]?
      bool TypeBitmapHas(const std::vector<unsigned char> &rdata, size_t start, unsigned short type)
      {
         size_t offset = start;
         while (offset + 2 <= rdata.size())
         {
            unsigned char window = rdata[offset];
            unsigned char length = rdata[offset + 1];
            offset += 2;
            if (length == 0 || length > 32 || offset + length > rdata.size())
               return false;
            if (window == static_cast<unsigned char>(type >> 8))
            {
               unsigned char low = static_cast<unsigned char>(type & 0xFF);
               size_t byteIndex = low >> 3;
               if (byteIndex >= length)
                  return false;
               return (rdata[offset + byteIndex] & (0x80 >> (low & 7))) != 0;
            }
            offset += length;
         }
         return false;
      }

      // RFC 4034 6.1: canonical ordering of names - label by label from the
      // right, as octets, a name that is a proper suffix of another first.
      // Both names are lowercase and dotted, "" the root.
      int CompareCanonicalNames(const AnsiString &left, const AnsiString &right)
      {
         std::vector<AnsiString> leftLabels;
         std::vector<AnsiString> rightLabels;
         for (const AnsiString &label : StringParser::SplitString(left, "."))
            if (!label.IsEmpty())
               leftLabels.push_back(label);
         for (const AnsiString &label : StringParser::SplitString(right, "."))
            if (!label.IsEmpty())
               rightLabels.push_back(label);

         int leftIndex = static_cast<int>(leftLabels.size()) - 1;
         int rightIndex = static_cast<int>(rightLabels.size()) - 1;
         while (leftIndex >= 0 && rightIndex >= 0)
         {
            int order = leftLabels[leftIndex].Compare(rightLabels[rightIndex]);
            if (order != 0)
               return order < 0 ? -1 : 1;
            leftIndex--;
            rightIndex--;
         }
         if (leftIndex < 0 && rightIndex < 0)
            return 0;
         return leftIndex < 0 ? -1 : 1;
      }

      // RFC 5155 5: SHA-1 over the canonical name and the salt, iterated. More
      // iterations than a resolver is asked to tolerate (RFC 9276) is no proof.
      bool ComputeNsec3Hash(const AnsiString &name, unsigned char algorithm, unsigned short iterations,
                            const std::vector<unsigned char> &salt, std::vector<unsigned char> &out)
      {
         if (algorithm != 1 || iterations > 150)
            return false;

         std::vector<unsigned char> input;
         if (!AppendName(input, name))
            return false;
         input.insert(input.end(), salt.begin(), salt.end());

         unsigned char digest[SHA_DIGEST_LENGTH];
         SHA1(input.data(), input.size(), digest);
         for (unsigned short i = 0; i < iterations; i++)
         {
            std::vector<unsigned char> again(digest, digest + SHA_DIGEST_LENGTH);
            again.insert(again.end(), salt.begin(), salt.end());
            SHA1(again.data(), again.size(), digest);
         }
         out.assign(digest, digest + SHA_DIGEST_LENGTH);
         return true;
      }

      // RFC 4648 base32hex, uppercase, no padding - the spelling of an NSEC3
      // owner label, which sorts exactly as the hash's octets do.
      AnsiString Base32Hex(const std::vector<unsigned char> &data)
      {
         static const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
         AnsiString result;
         unsigned long buffer = 0;
         int bits = 0;
         for (unsigned char byte : data)
         {
            buffer = (buffer << 8) | byte;
            bits += 8;
            while (bits >= 5)
            {
               result += alphabet[(buffer >> (bits - 5)) & 31];
               bits -= 5;
            }
         }
         if (bits > 0)
            result += alphabet[(buffer << (5 - bits)) & 31];
         return result;
      }

      enum class DsDenial
      {
         NoProof,             // nothing in the authority section speaks to it
         UnsignedDelegation,  // a verified record: the name is a delegation without a DS
         NotADelegation,      // a verified record: the name exists but is not a zone cut
         Nonexistent,         // a verified record: the name does not exist
         Invalid,             // records were there, but none verified or none applied
         Indeterminate        // none verified, and a signer's keys could not be fetched
      };

      // What one verified NSEC or NSEC3 record says about a DS RRset at the name.
      DsDenial ReadDsDenial(const AnsiString &name, const ParsedRr &record, const DnsResponse &response)
      {
         if (record.type == DnsTypeNsec)
         {
            AnsiString next;
            size_t offset = record.rdata_offset;
            if (!ReadName(response.packet, offset, next))
               return DsDenial::Invalid;
            size_t bitmapStart = offset - record.rdata_offset;

            if (record.owner == name)
            {
               // The parent's own apex is not a delegation, and a bitmap that has DS in
               // it says the record exists - which contradicts the answer it came with.
               if (TypeBitmapHas(record.rdata, bitmapStart, DnsTypeDs) || TypeBitmapHas(record.rdata, bitmapStart, DnsTypeSoa))
                  return DsDenial::Invalid;
               return TypeBitmapHas(record.rdata, bitmapStart, DnsTypeNs) ? DsDenial::UnsignedDelegation : DsDenial::NotADelegation;
            }

            // Covering: owner before the name and the name before next - or the next
            // name wrapping round to the apex, in which case everything after the
            // owner is covered.
            bool ownerBefore = CompareCanonicalNames(record.owner, name) < 0;
            bool nameBeforeNext = CompareCanonicalNames(name, next) < 0;
            bool wraps = CompareCanonicalNames(next, record.owner) <= 0;
            if (ownerBefore && (nameBeforeNext || wraps))
               return DsDenial::Nonexistent;
            return DsDenial::Invalid;
         }

         // NSEC3: hash algorithm, flags, iterations, salt, the next hashed owner,
         // then the bitmap.
         const std::vector<unsigned char> &rdata = record.rdata;
         if (rdata.size() < 6)
            return DsDenial::Invalid;
         unsigned char algorithm = rdata[0];
         unsigned char flags = rdata[1];
         unsigned short iterations = (static_cast<unsigned short>(rdata[2]) << 8) | rdata[3];
         size_t saltLength = rdata[4];
         if (rdata.size() < 5 + saltLength + 1)
            return DsDenial::Invalid;
         std::vector<unsigned char> salt(rdata.begin() + 5, rdata.begin() + 5 + saltLength);
         size_t hashLengthOffset = 5 + saltLength;
         size_t hashLength = rdata[hashLengthOffset];
         if (rdata.size() < hashLengthOffset + 1 + hashLength)
            return DsDenial::Invalid;
         std::vector<unsigned char> nextHash(rdata.begin() + hashLengthOffset + 1, rdata.begin() + hashLengthOffset + 1 + hashLength);
         size_t bitmapStart = hashLengthOffset + 1 + hashLength;

         std::vector<unsigned char> hash;
         if (!ComputeNsec3Hash(name, algorithm, iterations, salt, hash))
            return DsDenial::Invalid;

         int dot = record.owner.Find('.');
         AnsiString ownerLabel = dot >= 0 ? record.owner.Left(dot) : record.owner;
         ownerLabel.MakeUpper();
         AnsiString hashed = Base32Hex(hash);
         AnsiString nextLabel = Base32Hex(nextHash);

         if (ownerLabel == hashed)
         {
            if (TypeBitmapHas(rdata, bitmapStart, DnsTypeDs) || TypeBitmapHas(rdata, bitmapStart, DnsTypeSoa))
               return DsDenial::Invalid;
            return TypeBitmapHas(rdata, bitmapStart, DnsTypeNs) ? DsDenial::UnsignedDelegation : DsDenial::NotADelegation;
         }

         // Covering: strictly between the owner's hash and the next one, the last
         // record's span wrapping round past the end to the first. Until 6.3.6 the
         // part of that span before the first hash was not read as covered.
         bool ownerBefore = ownerLabel.Compare(hashed) < 0;
         bool hashBeforeNext = hashed.Compare(nextLabel) < 0;
         bool wraps = nextLabel.Compare(ownerLabel) <= 0;
         if (!(wraps ? (ownerBefore || hashBeforeNext) : (ownerBefore && hashBeforeNext)))
            return DsDenial::Invalid;

         // Covered. With Opt-Out the span may hold unsigned delegations, and this is
         // read as one (RFC 5155 8.9); without it the name does not exist.
         return (flags & 0x01) != 0 ? DsDenial::UnsignedDelegation : DsDenial::Nonexistent;
      }

      // The authority section's NSEC and NSEC3 records, each verified against its
      // signer's keys - the signer has to be a proper ancestor of the name, and
      // required_signer when one is given - and read for what they say about a DS
      // RRset at the name.
      DsDenial EvaluateDsDenial(const AnsiString &name, const DnsResponse &response, int depth,
                                const AnsiString *required_signer = nullptr)
      {
         std::vector<ParsedRr> proofs;
         std::vector<RrsigInfo> signatures;
         for (const ParsedRr &record : response.authority)
         {
            if (record.type == DnsTypeNsec || record.type == DnsTypeNsec3)
            {
               proofs.push_back(record);
            }
            else if (record.type == DnsTypeRrsig)
            {
               RrsigInfo info;
               if (ParseRrsig(response, record, info) &&
                   (info.type_covered == DnsTypeNsec || info.type_covered == DnsTypeNsec3))
               {
                  signatures.push_back(info);
               }
            }
         }
         if (proofs.empty())
            return DsDenial::NoProof;

         bool sawIndeterminateSigner = false;

         for (const ParsedRr &proof : proofs)
         {
            // The RRset the record belongs to - every record of its type under its
            // owner - is what its RRSIG covers.
            std::vector<ParsedRr> rrset;
            for (const ParsedRr &candidate : proofs)
               if (candidate.type == proof.type && candidate.owner == proof.owner)
                  rrset.push_back(candidate);

            bool verified = false;
            for (const RrsigInfo &sig : signatures)
            {
               if (verified)
                  break;
               if (sig.type_covered != proof.type)
                  continue;
               if (sig.signer == name || !IsSubdomainOrEqual(name, sig.signer) || !IsSubdomainOrEqual(proof.owner, sig.signer))
                  continue;
               if (required_signer != nullptr && sig.signer != *required_signer)
                  continue;

               std::vector<std::vector<unsigned char>> keys;
               InternalStatus signerStatus = ValidateZoneKeys(sig.signer, depth + 1, keys);
               if (signerStatus == InternalStatus::Insecure)
               {
                  // The parent itself is unsigned; nothing below it can be signed
                  // as far as this resolver is concerned.
                  return DsDenial::UnsignedDelegation;
               }
               if (signerStatus == InternalStatus::Indeterminate)
                  sawIndeterminateSigner = true;
               if (signerStatus != InternalStatus::Secure)
                  continue;

               for (const std::vector<unsigned char> &key : keys)
               {
                  if (key.size() < 5 || key[3] != sig.algorithm || ComputeKeyTag(key) != sig.key_tag)
                     continue;
                  if (VerifyRrsigOverSet(response, rrset, sig, key))
                  {
                     verified = true;
                     break;
                  }
               }
            }
            if (!verified)
               continue;

            DsDenial reading = ReadDsDenial(name, proof, response);
            if (reading != DsDenial::Invalid)
               return reading;
         }
         return sawIndeterminateSigner ? DsDenial::Indeterminate : DsDenial::Invalid;
      }

      // "No DS" with a proof: Insecure. "No DS" without one: what the parent is
      // decides - unsigned above means unsigned here, and a signed zone above that
      // offered no proof means the record was taken away, which is Bogus.
      InternalStatus ProveNoDs(const AnsiString &zone, const DnsResponse &response, int depth)
      {
         switch (EvaluateDsDenial(zone, response, depth))
         {
         case DsDenial::UnsignedDelegation:
            return InternalStatus::Insecure;
         case DsDenial::NotADelegation:
         case DsDenial::Nonexistent:
            // Records were signed in the name of a zone the parent, signed, says is
            // not a zone cut - or not a name at all.
            LOG_DEBUG("DNSSEC: the parent of " + String(zone) + " proves it is not a delegation, yet records are signed as its zone; treating the delegation as bogus.");
            return InternalStatus::Bogus;
         case DsDenial::Invalid:
            LOG_DEBUG("DNSSEC: " + String(zone) + " has no DS and the denial that came with the answer does not verify or does not apply; treating the delegation as bogus.");
            return InternalStatus::Bogus;
         case DsDenial::Indeterminate:
            return InternalStatus::Indeterminate;
         case DsDenial::NoProof:
            break;
         }

         AnsiString ancestor = zone;
         for (int hop = 0; hop < MaxChainDepth; hop++)
         {
            if (ancestor.IsEmpty())
               break;
            int dot = ancestor.Find('.');
            ancestor = dot >= 0 ? ancestor.Mid(dot + 1) : AnsiString("");

            if (ancestor.IsEmpty())
            {
               // The root is signed by definition - the trust anchors - and nothing
               // between it and the zone proved an unsigned cut.
               LOG_DEBUG("DNSSEC: " + String(zone) + " has no DS and no NSEC or NSEC3 proof that none exists, all the way to the root; treating the delegation as bogus.");
               return InternalStatus::Bogus;
            }

            DnsResponse dsResponse;
            if (!RunQuery(ancestor, DnsTypeDs, dsResponse))
               return InternalStatus::Indeterminate; // until 6.3.6 Insecure, and a dropped packet a downgrade

            bool hasDs = false;
            for (const ParsedRr &record : dsResponse.answers)
            {
               if (record.owner == ancestor && record.type == DnsTypeDs)
               {
                  hasDs = true;
                  break;
               }
            }

            if (hasDs)
            {
               // A signed delegation above, with nothing between it and the zone
               // proved unsigned: that zone should have proved the DS absent.
               std::vector<std::vector<unsigned char>> keys;
               InternalStatus status = ValidateZoneKeys(ancestor, depth + 1, keys);
               if (status == InternalStatus::Secure)
               {
                  LOG_DEBUG("DNSSEC: " + String(zone) + " has no DS, and " + String(ancestor) + " is signed but gave no NSEC or NSEC3 proof that none exists; treating the delegation as bogus.");
                  return InternalStatus::Bogus;
               }
               return status;
            }

            switch (EvaluateDsDenial(ancestor, dsResponse, depth + 1))
            {
            case DsDenial::UnsignedDelegation:
               return InternalStatus::Insecure;
            case DsDenial::Invalid:
               LOG_DEBUG("DNSSEC: " + String(ancestor) + ", above " + String(zone) + ", has no DS and a denial that does not verify; treating the delegation as bogus.");
               return InternalStatus::Bogus;
            case DsDenial::Indeterminate:
               return InternalStatus::Indeterminate;
            default:
               break; // not a delegation, or nothing said: further up
            }
         }

         return InternalStatus::Bogus;
      }

      // The DNSKEY algorithms VerifySignature knows and the DS digests
      // DnskeyMatchesDs computes.
      bool IsSupportedDs(const DsInfo &ds)
      {
         const bool algorithm = ds.algorithm == 8 || ds.algorithm == 10 || ds.algorithm == 13 ||
                                ds.algorithm == 14 || ds.algorithm == 15;
         const bool digest = ds.digest_type == 2 || ds.digest_type == 4;
         return algorithm && digest;
      }

      // Fetches and validates the DS RRset for a zone (served by the
      // parent). Secure fills dsRecords; Insecure means an unsigned
      // delegation was found, or a validated DS RRset none of whose records
      // this resolver can follow (RFC 4035 section 5.2) - until 6.3.6 such a
      // zone was Bogus, and its mail held up.
      // why is set as ValidateZoneKeys sets it.
      InternalStatus FetchDsRecords(const AnsiString &zone, int depth, std::vector<DsInfo> &dsRecords, AnsiString &why)
      {
         DnsResponse response;
         if (!RunQuery(zone, DnsTypeDs, response))
         {
            why = AnsiString("the DS lookup for ") + zone + " did not complete: no DNS server answered, or it answered SERVFAIL or a reply that does not parse";
            return InternalStatus::Indeterminate; // until 6.3.6 Insecure, and a dropped packet a downgrade
         }

         std::vector<ParsedRr> dsRrset;
         std::vector<RrsigInfo> signatures;

         for (const ParsedRr &record : response.answers)
         {
            if (record.owner != zone)
               continue;

            if (record.type == DnsTypeDs)
            {
               dsRrset.push_back(record);
            }
            else if (record.type == DnsTypeRrsig)
            {
               RrsigInfo info;
               if (ParseRrsig(response, record, info) && info.type_covered == DnsTypeDs)
                  signatures.push_back(info);
            }
         }

         if (dsRrset.empty())
         {
            const InternalStatus proven = ProveNoDs(zone, response, depth);
            if (proven == InternalStatus::Bogus)
               why = ZoneText(zone) + " has no DS record, and no NSEC or NSEC3 record that verifies proves that none exists";
            else if (proven == InternalStatus::Indeterminate)
               why = AnsiString("whether ") + ZoneText(zone) + " is a signed delegation could not be told: a lookup on the way did not complete";
            return proven;
         }

         bool sawInsecureParent = false;
         bool sawIndeterminateParent = false;

         // Why each signature failed, in order, for the reason a Bogus result is given.
         std::vector<AnsiString> failures;

         if (signatures.empty())
            failures.push_back(AnsiString("the DS RRset of ") + zone + " came without an RRSIG: the DNS server did not return the DNSSEC records asked for (the DO bit), or something on the way removed them");

         for (const RrsigInfo &sig : signatures)
         {
            // The DS RRset is signed by the parent zone, which must be a
            // proper ancestor of the child zone.
            if (sig.signer == zone || !IsSubdomainOrEqual(zone, sig.signer))
            {
               failures.push_back(DescribeSignature(sig) + " over the DS RRset of " + zone + " is not by a zone above it");
               continue;
            }

            std::vector<std::vector<unsigned char>> parentKeys;
            AnsiString parentWhy;
            InternalStatus parentStatus = ValidateZoneKeys(sig.signer, depth + 1, parentKeys, &parentWhy);

            if (parentStatus == InternalStatus::Insecure)
            {
               sawInsecureParent = true;
               continue;
            }

            if (parentStatus == InternalStatus::Indeterminate)
            {
               sawIndeterminateParent = true;
               failures.push_back(parentWhy);
               continue;
            }

            if (parentStatus == InternalStatus::Bogus)
            {
               failures.push_back(parentWhy);
               continue;
            }

            bool keyFound = false;

            for (const std::vector<unsigned char> &keyRdata : parentKeys)
            {
               if (keyRdata.size() < 5)
                  continue;

               if (keyRdata[3] != sig.algorithm || ComputeKeyTag(keyRdata) != sig.key_tag)
                  continue;

               keyFound = true;
               const SignatureCheck check = CheckRrsigOverSet(response, dsRrset, sig, keyRdata);
               if (check != SignatureCheck::Verified)
               {
                  failures.push_back(DescribeSignature(sig) + " over the DS RRset of " + zone + " " + DescribeCheck(check, sig));
                  continue;
               }

               for (const ParsedRr &record : dsRrset)
               {
                  if (record.rdata.size() < 5)
                     continue;

                  DsInfo info;
                  info.key_tag = (static_cast<unsigned short>(record.rdata[0]) << 8) | record.rdata[1];
                  info.algorithm = record.rdata[2];
                  info.digest_type = record.rdata[3];
                  info.digest.assign(record.rdata.begin() + 4, record.rdata.end());

                  dsRecords.push_back(info);
               }

               if (dsRecords.empty())
                  return InternalStatus::Insecure;

               if (std::none_of(dsRecords.begin(), dsRecords.end(), IsSupportedDs))
               {
                  LOG_DEBUG("DNSSEC: the validated DS RRset of " + String(zone) + " names no algorithm and digest this resolver supports; treating the zone as unsigned (RFC 4035 section 5.2).");
                  dsRecords.clear();
                  return InternalStatus::Insecure;
               }

               return InternalStatus::Secure;
            }

            if (!keyFound)
               failures.push_back(DescribeSignature(sig) + " over the DS RRset of " + zone + " names no key " + ZoneText(sig.signer) + " publishes");
         }

         if (sawInsecureParent)
            return InternalStatus::Insecure;

         why = AnsiString("the DS RRset of ") + zone + (sawIndeterminateParent ? " could not be validated" : " did not validate");
         for (size_t i = 0; i < failures.size(); i++)
            why += (i == 0 ? AnsiString(": ") : AnsiString("; ")) + failures[i];

         return sawIndeterminateParent ? InternalStatus::Indeterminate : InternalStatus::Bogus;
      }

      // Returns the validated DNSKEY RRset of a zone, walking up to the
      // root trust anchors.
      InternalStatus ValidateZoneKeys(const AnsiString &zone, int depth, std::vector<std::vector<unsigned char>> &keys,
                                      AnsiString *why)
      {
         if (depth > MaxChainDepth)
         {
            if (why != nullptr)
               *why = AnsiString("the chain of trust to ") + ZoneText(zone) + " is longer than this resolver follows";
            return InternalStatus::Bogus;
         }

         // Not on the tester's thread (TestEnvironment).
         if (test_environment == nullptr)
         {
            boost::lock_guard<boost::recursive_mutex> guard(zone_cache_mutex);

            auto cached = zone_cache.find(zone);
            if (cached != zone_cache.end() && cached->second.expires > time(nullptr))
            {
               keys = cached->second.dnskey_rdatas;
               if (why != nullptr)
                  *why = cached->second.reason;
               return cached->second.status;
            }
         }

         InternalStatus result = InternalStatus::Bogus;
         AnsiString reason;
         keys.clear();

         // Establish the trust anchors for this zone: the root uses the
         // built-in anchors, everything else a validated DS RRset.
         std::vector<DsInfo> anchors;

         if (zone.IsEmpty())
         {
            anchors = GetRootTrustAnchors();
         }
         else
         {
            InternalStatus dsStatus = FetchDsRecords(zone, depth, anchors, reason);
            if (dsStatus != InternalStatus::Secure)
               result = dsStatus;
         }

         if (!anchors.empty())
         {
            DnsResponse response;
            if (RunQuery(zone, DnsTypeDnskey, response))
            {
               std::vector<ParsedRr> dnskeyRrset;
               std::vector<RrsigInfo> signatures;

               for (const ParsedRr &record : response.answers)
               {
                  if (record.owner != zone)
                     continue;

                  if (record.type == DnsTypeDnskey)
                  {
                     dnskeyRrset.push_back(record);
                  }
                  else if (record.type == DnsTypeRrsig)
                  {
                     RrsigInfo info;
                     if (ParseRrsig(response, record, info) &&
                         info.type_covered == DnsTypeDnskey && info.signer == zone)
                     {
                        signatures.push_back(info);
                     }
                  }
               }

               // Find a key that matches a trust anchor and signs the
               // DNSKEY RRset; that proves the whole RRset.
               bool trusted = false;

               // What the search found when it found nothing: whether any key
               // matched an anchor, and why each signature by one did not verify.
               bool anchorMatched = false;
               std::vector<AnsiString> failures;

               for (const DsInfo &anchor : anchors)
               {
                  if (trusted)
                     break;

                  for (const ParsedRr &keyRecord : dnskeyRrset)
                  {
                     if (trusted)
                        break;

                     const std::vector<unsigned char> &rdata = keyRecord.rdata;
                     if (rdata.size() < 5)
                        continue;

                     unsigned short keyFlags = (static_cast<unsigned short>(rdata[0]) << 8) | rdata[1];

                     if ((keyFlags & 0x0100) == 0)  // ZONE flag required
                        continue;
                     if ((keyFlags & 0x0080) != 0)  // revoked (RFC 5011)
                        continue;
                     if (rdata[2] != 3)             // protocol
                        continue;
                     if (rdata[3] != anchor.algorithm)
                        continue;
                     if (ComputeKeyTag(rdata) != anchor.key_tag)
                        continue;
                     if (!DnskeyMatchesDs(zone, rdata, anchor))
                        continue;

                     anchorMatched = true;
                     bool signedByIt = false;

                     for (const RrsigInfo &sig : signatures)
                     {
                        if (sig.key_tag != anchor.key_tag || sig.algorithm != anchor.algorithm)
                           continue;

                        signedByIt = true;
                        const SignatureCheck check = CheckRrsigOverSet(response, dnskeyRrset, sig, rdata);
                        if (check == SignatureCheck::Verified)
                        {
                           trusted = true;
                           break;
                        }

                        failures.push_back(DescribeSignature(sig) + " over the DNSKEY RRset of " + ZoneText(zone) + " " + DescribeCheck(check, sig));
                     }

                     if (!signedByIt)
                        failures.push_back(AnsiString("no RRSIG over the DNSKEY RRset of ") + ZoneText(zone) + " is by key tag " + Number(anchor.key_tag) +
                                           ", the key its " + (zone.IsEmpty() ? "trust anchor" : "DS record") + " names");
                  }
               }

               if (!trusted && result != InternalStatus::Insecure)
               {
                  if (dnskeyRrset.empty())
                  {
                     reason = AnsiString("the DNSKEY answer for ") + ZoneText(zone) + " holds no DNSKEY records";
                  }
                  else if (signatures.empty())
                  {
                     reason = AnsiString("the DNSKEY RRset of ") + ZoneText(zone) + " came without an RRSIG by the zone: the DNS server did not return the DNSSEC records asked for (the DO bit), or something on the way removed them";
                  }
                  else if (!anchorMatched)
                  {
                     // A DS mismatch: none of the zone's keys is the one its parent
                     // (or, for the root, the trust anchors) vouches for.
                     reason = AnsiString("no DNSKEY of ") + ZoneText(zone) + " matches " + (zone.IsEmpty() ? "the trust anchors" : "its DS records");
                     for (size_t i = 0; i < anchors.size(); i++)
                     {
                        reason += (i == 0 ? AnsiString(" (") : AnsiString(", ")) + "key tag " + Number(anchors[i].key_tag) +
                                  " algorithm " + Number(anchors[i].algorithm) + " digest type " + Number(anchors[i].digest_type);
                     }
                     reason += anchors.empty() ? "" : ")";
                  }
                  else
                  {
                     reason = AnsiString("the DNSKEY RRset of ") + ZoneText(zone) + " did not validate";
                     for (size_t i = 0; i < failures.size(); i++)
                        reason += (i == 0 ? AnsiString(": ") : AnsiString("; ")) + failures[i];
                  }
               }

               if (trusted)
               {
                  for (const ParsedRr &keyRecord : dnskeyRrset)
                  {
                     const std::vector<unsigned char> &rdata = keyRecord.rdata;
                     if (rdata.size() < 5)
                        continue;

                     unsigned short keyFlags = (static_cast<unsigned short>(rdata[0]) << 8) | rdata[1];

                     if ((keyFlags & 0x0100) == 0 || (keyFlags & 0x0080) != 0 || rdata[2] != 3)
                        continue;

                     keys.push_back(rdata);
                  }

                  result = keys.empty() ? InternalStatus::Bogus : InternalStatus::Secure;
                  if (keys.empty())
                     reason = AnsiString("the DNSKEY RRset of ") + ZoneText(zone) + " holds no zone key";
               }
               else if (result != InternalStatus::Insecure)
               {
                  result = InternalStatus::Bogus;
               }
            }
            else
            {
               // The keys could not be fetched. Until 6.3.6 this was Insecure,
               // and a dropped DNSKEY answer took the zone out of DNSSEC.
               result = InternalStatus::Indeterminate;
               reason = AnsiString("the DNSKEY lookup for ") + ZoneText(zone) + " did not complete: no DNS server answered, or it answered SERVFAIL or a reply that does not parse";
            }
         }

         // Said once each time the verdict is reached, which the cache below
         // keeps to once a minute or less for each zone.
         if (result == InternalStatus::Bogus || result == InternalStatus::Indeterminate)
         {
            LOG_DEBUG("DNSSEC: the keys of " + String(ZoneText(zone)) + (result == InternalStatus::Bogus ? String(" are bogus: ") : String(" could not be validated: ")) +
                      String(reason) + ".");
         }

         if (why != nullptr)
            *why = reason;

         // The tester's verdicts are its own (TestEnvironment).
         if (test_environment != nullptr)
            return result;

         // Cache the outcome. A lookup that did not complete is held briefly,
         // so that one validation does not wait on the same dead server twice
         // and the next attempt asks again.
         int cacheSeconds = 60;
         if (result == InternalStatus::Secure)
            cacheSeconds = 3600;
         else if (result == InternalStatus::Insecure)
            cacheSeconds = 300;
         else if (result == InternalStatus::Indeterminate)
            cacheSeconds = 30;

         {
            boost::lock_guard<boost::recursive_mutex> guard(zone_cache_mutex);

            ZoneEntry entry;
            entry.status = result;
            entry.dnskey_rdatas = keys;
            entry.expires = time(nullptr) + cacheSeconds;
            entry.reason = reason;

            zone_cache[zone] = entry;

            // Keep the cache bounded.
            if (zone_cache.size() > 512)
               zone_cache.clear();
         }

         return result;
      }

      // What the signatures over an answer RRset say.
      enum class RrsetVerdict
      {
         Secure,          // a signature verified under a validated key
         Bogus,           // signatures applied, and none verified
         Unsigned,        // no signature applies to it
         InsecureZone,    // a signature names a zone proven unsigned
         Indeterminate    // a signature names a zone whose keys could not be fetched
      };

      // Validates an answer RRset against its RRSIGs. why, when given, is set
      // to what each applicable signature failed on, for a verdict of Bogus or
      // Indeterminate, and to the want of one for Unsigned.
      RrsetVerdict ValidateRrset(const DnsResponse &response, const std::vector<ParsedRr> &rrset,
                                 const std::vector<RrsigInfo> &signatures, AnsiString *why = nullptr)
      {
         if (rrset.empty())
            return RrsetVerdict::Unsigned;

         bool sawInsecureZone = false;
         bool sawIndeterminateZone = false;
         bool sawApplicableSignature = false;
         std::vector<AnsiString> failures;

         for (const RrsigInfo &sig : signatures)
         {
            if (sig.type_covered != rrset[0].type)
               continue;

            if (!IsSubdomainOrEqual(rrset[0].owner, sig.signer))
               continue;

            sawApplicableSignature = true;

            std::vector<std::vector<unsigned char>> zoneKeys;
            AnsiString zoneWhy;
            InternalStatus zoneStatus = ValidateZoneKeys(sig.signer, 0, zoneKeys, &zoneWhy);

            if (zoneStatus == InternalStatus::Insecure)
            {
               sawInsecureZone = true;
               continue;
            }

            if (zoneStatus == InternalStatus::Indeterminate)
            {
               sawIndeterminateZone = true;
               failures.push_back(zoneWhy);
               continue;
            }

            if (zoneStatus == InternalStatus::Bogus)
            {
               failures.push_back(zoneWhy);
               continue;
            }

            bool keyFound = false;

            for (const std::vector<unsigned char> &keyRdata : zoneKeys)
            {
               if (keyRdata.size() < 5)
                  continue;

               if (keyRdata[3] != sig.algorithm || ComputeKeyTag(keyRdata) != sig.key_tag)
                  continue;

               keyFound = true;
               const SignatureCheck check = CheckRrsigOverSet(response, rrset, sig, keyRdata);
               if (check == SignatureCheck::Verified)
                  return RrsetVerdict::Secure;

               failures.push_back(DescribeSignature(sig) + " " + DescribeCheck(check, sig));
            }

            if (!keyFound)
               failures.push_back(DescribeSignature(sig) + " names no key " + ZoneText(sig.signer) + " publishes (no DNSKEY with that key tag and algorithm)");
         }

         if (why != nullptr)
         {
            if (!sawApplicableSignature)
            {
               *why = "no RRSIG covers it";
            }
            else
            {
               why->Empty();
               for (size_t i = 0; i < failures.size(); i++)
                  *why += (i == 0 ? AnsiString() : AnsiString("; ")) + failures[i];
            }
         }

         if (!sawApplicableSignature)
            return RrsetVerdict::Unsigned;

         if (sawInsecureZone)
            return RrsetVerdict::InsecureZone;

         return sawIndeterminateZone ? RrsetVerdict::Indeterminate : RrsetVerdict::Bogus;
      }

      // ----------------------------------------------------------------
      // Names, label by label
      // ----------------------------------------------------------------

      // The labels of a dotted name, leftmost first. The root has none.
      std::vector<AnsiString> LabelsOf(const AnsiString &name)
      {
         std::vector<AnsiString> labels;
         if (name.IsEmpty())
            return labels;

         for (const AnsiString &label : StringParser::SplitString(name, "."))
            if (!label.IsEmpty())
               labels.push_back(label);

         return labels;
      }

      // The name made of labels[first] and every label right of it.
      AnsiString NameFromLabel(const std::vector<AnsiString> &labels, size_t first)
      {
         AnsiString name;
         for (size_t i = first; i < labels.size(); i++)
         {
            if (!name.IsEmpty())
               name += ".";
            name += labels[i];
         }
         return name;
      }

      // The longest name both are under, "" when only the root.
      AnsiString CommonAncestor(const AnsiString &left, const AnsiString &right)
      {
         const std::vector<AnsiString> leftLabels = LabelsOf(left);
         const std::vector<AnsiString> rightLabels = LabelsOf(right);

         size_t shared = 0;
         while (shared < leftLabels.size() && shared < rightLabels.size() &&
                leftLabels[leftLabels.size() - 1 - shared] == rightLabels[rightLabels.size() - 1 - shared])
            shared++;

         return NameFromLabel(leftLabels, leftLabels.size() - shared);
      }

      AnsiString WildcardAt(const AnsiString &name)
      {
         return name.IsEmpty() ? AnsiString("*") : AnsiString("*.") + name;
      }

      // ----------------------------------------------------------------
      // A signed zone's proof that an answer is empty (RFC 4035 sections
      // 3.1.3 and 5.4, RFC 5155 sections 7.2 and 8, RFC 6840 section 4.1)
      // ----------------------------------------------------------------

      enum class Absence
      {
         NoData,     // the name exists and holds nothing of the type
         NoName,     // the name does not exist, nor a wildcard that would answer for it
         OptOut,     // an NSEC3 Opt-Out span covers the name, which may then sit below
                     // an unsigned delegation (RFC 5155 section 9.2), or the zone's NSEC3
                     // iterations are past what a resolver computes (RFC 9276): insecure
         Unproven
      };

      // One verified NSEC: its owner, the next name, and its type bitmap.
      struct NsecProof
      {
         AnsiString owner;
         AnsiString next;
         std::vector<unsigned char> rdata;
         size_t bitmap = 0;
      };

      // One verified NSEC3: its hash parameters, its owner's hash and the next
      // one (base32hex, uppercase, which sorts as the octets do), and its type
      // bitmap.
      struct Nsec3Proof
      {
         unsigned char algorithm = 0;
         unsigned char flags = 0;
         unsigned short iterations = 0;
         std::vector<unsigned char> salt;
         AnsiString owner_hash;
         AnsiString next_hash;
         std::vector<unsigned char> rdata;
         size_t bitmap = 0;
      };

      bool ReadNsecProof(const DnsResponse &response, const ParsedRr &record, NsecProof &proof)
      {
         size_t offset = record.rdata_offset;
         if (!ReadName(response.packet, offset, proof.next))
            return false;

         if (offset < record.rdata_offset || offset - record.rdata_offset > record.rdata.size())
            return false;

         proof.owner = record.owner;
         proof.rdata = record.rdata;
         proof.bitmap = offset - record.rdata_offset;
         return true;
      }

      bool ReadNsec3Proof(const ParsedRr &record, const AnsiString &zone, Nsec3Proof &proof)
      {
         const std::vector<unsigned char> &rdata = record.rdata;
         if (rdata.size() < 6)
            return false;

         proof.algorithm = rdata[0];
         proof.flags = rdata[1];
         proof.iterations = static_cast<unsigned short>((static_cast<unsigned short>(rdata[2]) << 8) | rdata[3]);

         // RFC 5155 section 8.2: a record with a flag other than Opt-Out is ignored.
         if ((proof.flags & 0xFE) != 0)
            return false;

         const size_t saltLength = rdata[4];
         if (rdata.size() < 5 + saltLength + 1)
            return false;
         proof.salt.assign(rdata.begin() + 5, rdata.begin() + 5 + static_cast<std::ptrdiff_t>(saltLength));

         const size_t hashLengthOffset = 5 + saltLength;
         const size_t hashLength = rdata[hashLengthOffset];
         if (hashLength == 0 || rdata.size() < hashLengthOffset + 1 + hashLength)
            return false;

         const std::vector<unsigned char> nextHash(rdata.begin() + static_cast<std::ptrdiff_t>(hashLengthOffset + 1),
                                                   rdata.begin() + static_cast<std::ptrdiff_t>(hashLengthOffset + 1 + hashLength));
         proof.next_hash = Base32Hex(nextHash);
         proof.bitmap = hashLengthOffset + 1 + hashLength;
         proof.rdata = rdata;

         // RFC 5155 section 8.2 too: the owner is the hash, one label, directly
         // below the zone that signed it.
         const int dot = record.owner.Find('.');
         if (zone.IsEmpty())
         {
            if (dot >= 0 || record.owner.IsEmpty())
               return false;
            proof.owner_hash = record.owner;
         }
         else
         {
            if (dot <= 0 || record.owner.Mid(dot + 1) != zone)
               return false;
            proof.owner_hash = record.owner.Left(dot);
         }

         proof.owner_hash.MakeUpper();
         return true;
      }

      // The NSEC and NSEC3 records of a negative answer's authority section
      // that zone signed, each RRset with a signature by one of zone's
      // validated keys that counts every label of its owner (no denial is
      // synthesised from a wildcard). A record another zone signed proves
      // nothing about a name in this one; the walk has already placed the name
      // below any delegation (RFC 6840 section 4.1).
      void CollectProofs(const DnsResponse &response, const AnsiString &zone,
                         std::vector<NsecProof> &nsecs, std::vector<Nsec3Proof> &nsec3s)
      {
         std::vector<ParsedRr> records;
         std::vector<RrsigInfo> signatures;

         for (const ParsedRr &record : response.authority)
         {
            if ((record.type == DnsTypeNsec || record.type == DnsTypeNsec3) && IsSubdomainOrEqual(record.owner, zone))
            {
               records.push_back(record);
            }
            else if (record.type == DnsTypeRrsig)
            {
               RrsigInfo info;
               if (ParseRrsig(response, record, info) && info.signer == zone &&
                   (info.type_covered == DnsTypeNsec || info.type_covered == DnsTypeNsec3))
               {
                  signatures.push_back(info);
               }
            }
         }

         if (records.empty() || signatures.empty())
            return;

         std::vector<std::vector<unsigned char>> keys;
         if (ValidateZoneKeys(zone, 1, keys) != InternalStatus::Secure)
            return;

         std::set<std::pair<AnsiString, unsigned short>> done;

         for (const ParsedRr &record : records)
         {
            if (!done.insert(std::make_pair(record.owner, record.type)).second)
               continue;

            std::vector<ParsedRr> rrset;
            for (const ParsedRr &other : records)
               if (other.owner == record.owner && other.type == record.type)
                  rrset.push_back(other);

            bool verified = false;
            for (const RrsigInfo &sig : signatures)
            {
               if (verified)
                  break;

               if (sig.type_covered != record.type || static_cast<int>(sig.labels) != CountLabels(record.owner))
                  continue;

               for (const std::vector<unsigned char> &key : keys)
               {
                  if (key.size() < 5 || key[3] != sig.algorithm || ComputeKeyTag(key) != sig.key_tag)
                     continue;

                  if (VerifyRrsigOverSet(response, rrset, sig, key))
                  {
                     verified = true;
                     break;
                  }
               }
            }

            if (!verified)
               continue;

            for (const ParsedRr &member : rrset)
            {
               if (member.type == DnsTypeNsec)
               {
                  NsecProof proof;
                  if (ReadNsecProof(response, member, proof))
                     nsecs.push_back(proof);
               }
               else
               {
                  Nsec3Proof proof;
                  if (ReadNsec3Proof(member, zone, proof))
                     nsec3s.push_back(proof);
               }
            }
         }
      }

      bool InBitmap(const std::vector<unsigned char> &rdata, size_t bitmap, unsigned short type)
      {
         return TypeBitmapHas(rdata, bitmap, type);
      }

      // The parent side of a delegation: NS without SOA.
      bool IsDelegationPoint(const std::vector<unsigned char> &rdata, size_t bitmap)
      {
         return InBitmap(rdata, bitmap, DnsTypeNs) && !InBitmap(rdata, bitmap, DnsTypeSoa);
      }

      // Says the name holds nothing of type, and is no alias.
      bool BitmapLacks(const std::vector<unsigned char> &rdata, size_t bitmap, unsigned short type)
      {
         return !InBitmap(rdata, bitmap, type) && !InBitmap(rdata, bitmap, DnsTypeCname);
      }

      // An NSEC covers a name between its owner and the next name - past the
      // owner, for the last record, whose next name is the apex.
      bool NsecCovers(const NsecProof &proof, const AnsiString &name)
      {
         if (CompareCanonicalNames(proof.owner, name) >= 0)
            return false;

         return CompareCanonicalNames(name, proof.next) < 0 || CompareCanonicalNames(proof.next, proof.owner) <= 0;
      }

      // RFC 6840 section 4.1: an NSEC at a delegation point is the parent
      // side's, and one at a DNAME redirects what lies below it; neither speaks
      // for a name below its owner.
      bool SpeaksFor(const NsecProof &proof, const AnsiString &name)
      {
         if (proof.owner == name || !IsSubdomainOrEqual(name, proof.owner))
            return true;

         return !IsDelegationPoint(proof.rdata, proof.bitmap) && !InBitmap(proof.rdata, proof.bitmap, DnsTypeDname);
      }

      // The closest encloser an NSEC covering name shows: the deeper of the
      // names name shares with its owner and with its next name.
      AnsiString NsecClosestEncloser(const NsecProof &cover, const AnsiString &name)
      {
         const AnsiString byOwner = CommonAncestor(name, cover.owner);
         const AnsiString byNext = CommonAncestor(name, cover.next);
         return CountLabels(byNext) > CountLabels(byOwner) ? byNext : byOwner;
      }

      Absence NsecNameError(const std::vector<NsecProof> &nsecs, const AnsiString &name)
      {
         for (const NsecProof &cover : nsecs)
         {
            if (!NsecCovers(cover, name) || !SpeaksFor(cover, name))
               continue;

            const AnsiString closest = NsecClosestEncloser(cover, name);
            if (closest == name)
               continue; // names below it exist, so it does too

            // No wildcard at the closest encloser to answer for the name.
            const AnsiString wildcard = WildcardAt(closest);
            for (const NsecProof &other : nsecs)
            {
               if (NsecCovers(other, wildcard) && SpeaksFor(other, wildcard))
                  return Absence::NoName;
            }
         }

         return Absence::Unproven;
      }

      Absence NsecNoData(const std::vector<NsecProof> &nsecs, const AnsiString &name, unsigned short type)
      {
         // The record at the name, which does not list the type. The parent side
         // of a delegation speaks for DS alone.
         for (const NsecProof &proof : nsecs)
         {
            if (proof.owner != name)
               continue;
            if (IsDelegationPoint(proof.rdata, proof.bitmap) && type != DnsTypeDs)
               continue;
            if (BitmapLacks(proof.rdata, proof.bitmap, type))
               return Absence::NoData;
         }

         // An empty non-terminal: no record at the name, and one covering it
         // whose next name is below it.
         for (const NsecProof &cover : nsecs)
         {
            if (NsecCovers(cover, name) && SpeaksFor(cover, name) &&
                cover.next != name && IsSubdomainOrEqual(cover.next, name))
               return Absence::NoData;
         }

         // A wildcard that answers for the name and holds nothing of the type
         // (RFC 4035 section 3.1.3.4).
         for (const NsecProof &cover : nsecs)
         {
            if (!NsecCovers(cover, name) || !SpeaksFor(cover, name))
               continue;

            const AnsiString closest = NsecClosestEncloser(cover, name);
            if (closest == name)
               continue;

            const AnsiString wildcard = WildcardAt(closest);
            for (const NsecProof &star : nsecs)
            {
               if (star.owner == wildcard && BitmapLacks(star.rdata, star.bitmap, type))
                  return Absence::NoData;
            }
         }

         return Absence::Unproven;
      }

      // H(name) under one record's parameters, in base32hex: computed once per
      // name and parameters, since a response may carry many records with the
      // same ones.
      class Nsec3Hasher
      {
      public:
         bool Hash(const Nsec3Proof &proof, const AnsiString &name, AnsiString &hashed)
         {
            std::string key;
            key += static_cast<char>(proof.algorithm);
            key += static_cast<char>(proof.iterations >> 8);
            key += static_cast<char>(proof.iterations & 0xFF);
            key.append(proof.salt.begin(), proof.salt.end());
            key += '\0';
            key += name.c_str();

            auto known = hashes_.find(key);
            if (known == hashes_.end())
            {
               std::vector<unsigned char> digest;
               AnsiString result;
               if (ComputeNsec3Hash(name, proof.algorithm, proof.iterations, proof.salt, digest))
                  result = Base32Hex(digest);
               known = hashes_.insert(std::make_pair(key, result)).first;
            }

            hashed = known->second;
            return !hashed.IsEmpty();
         }

      private:
         std::map<std::string, AnsiString> hashes_;
      };

      // An NSEC3 covers a hash strictly between its owner's and the next one,
      // the span of the last record wrapping round past the end to the first.
      bool Nsec3Covers(const Nsec3Proof &proof, const AnsiString &hashed)
      {
         const bool afterOwner = proof.owner_hash.Compare(hashed) < 0;
         const bool beforeNext = hashed.Compare(proof.next_hash) < 0;
         if (proof.next_hash.Compare(proof.owner_hash) <= 0)
            return afterOwner || beforeNext;
         return afterOwner && beforeNext;
      }

      bool Nsec3MatchFor(const std::vector<Nsec3Proof> &nsec3s, Nsec3Hasher &hasher, const AnsiString &name, const Nsec3Proof *&match)
      {
         for (const Nsec3Proof &proof : nsec3s)
         {
            AnsiString hashed;
            if (hasher.Hash(proof, name, hashed) && proof.owner_hash == hashed)
            {
               match = &proof;
               return true;
            }
         }
         return false;
      }

      bool Nsec3CoverFor(const std::vector<Nsec3Proof> &nsec3s, Nsec3Hasher &hasher, const AnsiString &name, const Nsec3Proof *&cover)
      {
         for (const Nsec3Proof &proof : nsec3s)
         {
            AnsiString hashed;
            if (hasher.Hash(proof, name, hashed) && Nsec3Covers(proof, hashed))
            {
               cover = &proof;
               return true;
            }
         }
         return false;
      }

      // RFC 5155 section 8.3: the closest encloser of name - the longest of its
      // ancestors, up to the zone's apex, with a matching record that is neither
      // a delegation's parent side nor a DNAME - and the next closer name (the
      // closest encloser's child on the way to name) covered by a record, whose
      // Opt-Out flag is reported.
      bool Nsec3ClosestEncloser(const std::vector<Nsec3Proof> &nsec3s, Nsec3Hasher &hasher, const AnsiString &name,
                                const AnsiString &zone, AnsiString &closest, bool &optOut)
      {
         const std::vector<AnsiString> labels = LabelsOf(name);
         const size_t zoneLabels = static_cast<size_t>(CountLabels(zone));

         for (size_t first = 1; first < labels.size() + 1 && labels.size() - first + 1 > zoneLabels; first++)
         {
            const AnsiString ancestor = NameFromLabel(labels, first);

            const Nsec3Proof *match = nullptr;
            if (!Nsec3MatchFor(nsec3s, hasher, ancestor, match))
               continue;

            if (IsDelegationPoint(match->rdata, match->bitmap) || InBitmap(match->rdata, match->bitmap, DnsTypeDname))
               return false;

            const Nsec3Proof *cover = nullptr;
            if (!Nsec3CoverFor(nsec3s, hasher, NameFromLabel(labels, first - 1), cover))
               return false;

            closest = ancestor;
            optOut = (cover->flags & 0x01) != 0;
            return true;
         }

         return false;
      }

      Absence Nsec3NameError(const std::vector<Nsec3Proof> &nsec3s, const AnsiString &name, const AnsiString &zone, bool excessive)
      {
         Nsec3Hasher hasher;

         // A record matching the name says it exists.
         const Nsec3Proof *match = nullptr;
         if (Nsec3MatchFor(nsec3s, hasher, name, match))
            return Absence::Unproven;

         AnsiString closest;
         bool optOut = false;
         if (!Nsec3ClosestEncloser(nsec3s, hasher, name, zone, closest, optOut))
            return excessive ? Absence::OptOut : Absence::Unproven;

         if (optOut)
            return Absence::OptOut;

         // No wildcard at the closest encloser to answer for the name.
         const Nsec3Proof *cover = nullptr;
         if (Nsec3CoverFor(nsec3s, hasher, WildcardAt(closest), cover))
            return Absence::NoName;

         return excessive ? Absence::OptOut : Absence::Unproven;
      }

      Absence Nsec3NoData(const std::vector<Nsec3Proof> &nsec3s, const AnsiString &name, unsigned short type,
                          const AnsiString &zone, bool excessive)
      {
         Nsec3Hasher hasher;

         // The record at the name - an empty non-terminal has one too - which
         // does not list the type (RFC 5155 section 8.5). The parent side of a
         // delegation speaks for DS alone.
         const Nsec3Proof *match = nullptr;
         if (Nsec3MatchFor(nsec3s, hasher, name, match))
         {
            if (IsDelegationPoint(match->rdata, match->bitmap) && type != DnsTypeDs)
               return Absence::Unproven;

            return BitmapLacks(match->rdata, match->bitmap, type) ? Absence::NoData : Absence::Unproven;
         }

         // A wildcard that answers for the name and holds nothing of the type
         // (RFC 5155 section 8.7).
         AnsiString closest;
         bool optOut = false;
         if (Nsec3ClosestEncloser(nsec3s, hasher, name, zone, closest, optOut))
         {
            const Nsec3Proof *star = nullptr;
            if (Nsec3MatchFor(nsec3s, hasher, WildcardAt(closest), star) && BitmapLacks(star->rdata, star->bitmap, type))
               return Absence::NoData;
         }

         return excessive ? Absence::OptOut : Absence::Unproven;
      }

      // Whether an empty answer for name, of type, under the signed zone zone,
      // came with zone's proof of what it says: NXDOMAIN that the name and its
      // wildcard are not there, NOERROR that the name holds no record of the
      // type. Without one, the answer is somebody's word.
      Absence VerifyAbsence(const AnsiString &name, unsigned short type, const DnsResponse &response, const AnsiString &zone)
      {
         std::vector<NsecProof> nsecs;
         std::vector<Nsec3Proof> nsec3s;
         CollectProofs(response, zone, nsecs, nsec3s);

         // RFC 9276: past the iterations this resolver computes (ComputeNsec3Hash),
         // a zone's NSEC3 records are read as insecure, as other validators read them.
         const bool excessive = std::any_of(nsec3s.begin(), nsec3s.end(),
            [](const Nsec3Proof &proof) { return proof.algorithm == 1 && proof.iterations > 150; });

         if (response.rcode == 3)
         {
            if (NsecNameError(nsecs, name) == Absence::NoName)
               return Absence::NoName;

            return Nsec3NameError(nsec3s, name, zone, excessive);
         }

         if (NsecNoData(nsecs, name, type) == Absence::NoData)
            return Absence::NoData;

         return Nsec3NoData(nsec3s, name, type, zone, excessive);
      }

      // Whether response holds zone's proof that name is not there at all, nor a
      // wildcard that would answer for it, whatever its RCODE says (an RCODE is
      // not signed).
      bool ProvenNoName(const AnsiString &name, const DnsResponse &response, const AnsiString &zone)
      {
         std::vector<NsecProof> nsecs;
         std::vector<Nsec3Proof> nsec3s;
         CollectProofs(response, zone, nsecs, nsec3s);

         return NsecNameError(nsecs, name) == Absence::NoName ||
                Nsec3NameError(nsec3s, name, zone, false) == Absence::NoName;
      }

      // ----------------------------------------------------------------
      // Where a name is (RFC 7672 section 2.1.1, RFC 4035 section 5)
      //
      // An answer to a TLSA or MX lookup that carries no signature, that
      // carries no records and no proof of their absence, or that never came,
      // is read as unsigned only when the name is: below a delegation that the
      // signed zone above it proves unsigned. The walk asks each label's DS
      // from the root down, as a validator finds a name's closest enclosing
      // zone, and stops at the first unsigned cut. A signed zone that does not
      // show what is below it - no proof, a proof signed by somebody else, a
      // lookup that does not complete - leaves the answer Indeterminate.
      // ----------------------------------------------------------------

      bool CachedCut(const AnsiString &name, CutKind &kind)
      {
         // Not on the tester's thread (TestEnvironment).
         if (test_environment != nullptr)
            return false;

         boost::lock_guard<boost::recursive_mutex> guard(cut_cache_mutex);

         auto cached = cut_cache.find(name);
         if (cached == cut_cache.end() || cached->second.expires <= time(nullptr))
            return false;

         kind = cached->second.kind;
         return true;
      }

      void RememberCut(const AnsiString &name, CutKind kind)
      {
         if (test_environment != nullptr)
            return;

         boost::lock_guard<boost::recursive_mutex> guard(cut_cache_mutex);

         // Kept bounded, as the zone cache is.
         if (cut_cache.size() >= 512)
            cut_cache.clear();

         CutEntry entry;
         entry.kind = kind;
         entry.expires = time(nullptr) + (kind == CutKind::Signed ? SignedCutCacheSeconds : CutCacheSeconds);
         cut_cache[name] = entry;
      }

      // What candidate is, asked of the signed zone above it (zone). A DS RRset
      // makes it a signed cut, which ValidateZoneKeys then proves or finds bogus.
      // Without one, only a denial signed by zone itself says what it is - an
      // unsigned delegation, or no zone cut at all - and an alias signed by zone
      // is no cut (a CNAME has no NS beside it). Anything else is Indeterminate:
      // zone is signed and did not show what lies below it. Remembered only
      // when proven.
      InternalStatus ClassifyCut(const AnsiString &candidate, const AnsiString &zone, CutKind &kind)
      {
         if (CachedCut(candidate, kind))
            return InternalStatus::Secure;

         DnsResponse response;
         if (!RunQuery(candidate, DnsTypeDs, response))
         {
            LOG_DEBUG("DNSSEC: The DS lookup for " + String(candidate) + " did not complete; whether the name is signed cannot be told.");
            return InternalStatus::Indeterminate;
         }

         bool hasDs = false;
         std::vector<ParsedRr> aliases;
         std::vector<RrsigInfo> aliasSignatures;

         for (const ParsedRr &record : response.answers)
         {
            if (record.owner != candidate)
               continue;

            if (record.type == DnsTypeDs)
            {
               hasDs = true;
            }
            else if (record.type == DnsTypeCname)
            {
               aliases.push_back(record);
            }
            else if (record.type == DnsTypeRrsig)
            {
               RrsigInfo info;
               if (ParseRrsig(response, record, info) && info.type_covered == DnsTypeCname && info.signer == zone)
                  aliasSignatures.push_back(info);
            }
         }

         if (hasDs)
         {
            kind = CutKind::Signed;
            return InternalStatus::Secure;
         }

         if (!aliases.empty())
         {
            const RrsetVerdict verdict = ValidateRrset(response, aliases, aliasSignatures);
            if (verdict == RrsetVerdict::Secure)
            {
               kind = CutKind::NotACut;
               RememberCut(candidate, kind);
               return InternalStatus::Secure;
            }

            return verdict == RrsetVerdict::Bogus ? InternalStatus::Bogus : InternalStatus::Indeterminate;
         }

         switch (EvaluateDsDenial(candidate, response, 1, &zone))
         {
         case DsDenial::UnsignedDelegation:
            kind = CutKind::Unsigned;
            RememberCut(candidate, kind);
            return InternalStatus::Secure;
         case DsDenial::Nonexistent:
            // A top-level name the signed root proves is not there - the name and,
            // with it, any wildcard that could answer for it (ProvenNoName) - is
            // outside the public tree: no chain of trust reaches anything under it,
            // which is what a private name (company.local) is, and it is read as
            // unsigned, as a validator's own local zones are. The proof is the
            // root's, so nobody can make a name of the public tree look like one.
            // Below the root a name proven not there is no cut, and the walk goes on.
            if (zone.IsEmpty() && ProvenNoName(candidate, response, zone))
            {
               LOG_DEBUG("DNSSEC: the root proves " + String(candidate) + " is not a top-level name; a name under it is outside the signed tree.");
               kind = CutKind::Unsigned;
               RememberCut(candidate, kind);
               return InternalStatus::Secure;
            }
            kind = CutKind::NotACut;
            RememberCut(candidate, kind);
            return InternalStatus::Secure;
         case DsDenial::NotADelegation:
            kind = CutKind::NotACut;
            RememberCut(candidate, kind);
            return InternalStatus::Secure;
         case DsDenial::NoProof:
         case DsDenial::Invalid:
         case DsDenial::Indeterminate:
            break;
         }

         LOG_DEBUG("DNSSEC: " + String(zone.IsEmpty() ? AnsiString(".") : zone) + " is signed and did not prove what " + String(candidate) + " is; whether the name is signed cannot be told.");
         return InternalStatus::Indeterminate;
      }

      // When the application log last said that the root's keys could not be had
      // (RootUnverifiable); 0 before it ever has.
      std::atomic<time_t> root_unverifiable_said_at(0);

      // Says, at most once in ten minutes, that the root's keys could not be
      // validated - which, since 6.3.6, holds up the mail to every domain that is
      // not signed, so the application log names the cause and what to do rather
      // than leaving the administrator a deferral per message to read.
      void RootUnverifiable(InternalStatus root)
      {
         // The tester's answers are not this server's DNS (TestEnvironment).
         if (test_environment != nullptr)
            return;

         const time_t now = time(nullptr);
         time_t said = root_unverifiable_said_at.load();
         if (said != 0 && now - said < 600)
            return;
         if (!root_unverifiable_said_at.compare_exchange_strong(said, now))
            return;

         const AnsiString why = root == InternalStatus::Indeterminate
            ? AnsiString("could not be fetched: no DNS server it asks answered the query for them")
            : AnsiString("did not validate against the trust anchors (DnssecTrustAnchors, or the built-in IANA root anchors): they came without a signature, or with none that verifies");

         LOG_APPLICATION("DNSSEC: the root zone's keys " + String(why) + ". Until they validate, no answer can be proven unsigned, so every MX or TLSA lookup that comes back without signatures is indeterminate and its delivery is deferred (RFC 7672 section 2.1.1). The DNS servers this server asks (DNSServer, or else the system's first two IPv4 DNS servers, or its first two IPv6 ones when it lists no IPv4 server) must answer DNSSEC queries - the DO bit, with the signatures - over UDP and TCP; DaneEnforcementEnabled=0 delivers without DANE instead.");
      }

      // Where name is: Secure, with zone the closest enclosing zone the chain
      // of trust reaches; Insecure when an unsigned delegation is proven above
      // it, or an Opt-Out span leaves one possible (RFC 5155 section 9.2), or a
      // zone above it names only algorithms this resolver does not follow
      // (RFC 4035 section 5.2); Bogus when the chain fails; Indeterminate when
      // a lookup on the way did not complete or a signed zone did not prove
      // what lies below it - and when the root's own keys could not be had.
      InternalStatus FindEnclosingZone(const AnsiString &name, AnsiString &zone, AnsiString *why = nullptr)
      {
         zone = "";

         // The root's keys, validated against the trust anchors, are where every
         // proof starts. Without them nothing below can be told, and the fault is
         // this server's or its resolver's - signatures stripped on the way, no
         // answer, anchors out of date - not the name's: Indeterminate, never
         // Insecure, which would be the downgrade, and never Bogus, which would
         // report every recipient domain to TLS reporting as failing DNSSEC.
         std::vector<std::vector<unsigned char>> keys;
         AnsiString keysWhy;
         const InternalStatus root = ValidateZoneKeys("", 1, keys, &keysWhy);
         if (root != InternalStatus::Secure)
         {
            RootUnverifiable(root);
            if (why != nullptr)
               *why = keysWhy;
            return InternalStatus::Indeterminate;
         }

         const std::vector<AnsiString> labels = LabelsOf(name);
         if (labels.size() > MaxWalkLabels)
         {
            if (why != nullptr)
               *why = AnsiString("the name has more than ") + Number(static_cast<unsigned long>(MaxWalkLabels)) + " labels";
            return InternalStatus::Indeterminate;
         }

         for (size_t first = labels.size(); first-- > 0;)
         {
            const AnsiString candidate = NameFromLabel(labels, first);

            CutKind kind = CutKind::NotACut;
            const InternalStatus classified = ClassifyCut(candidate, zone, kind);
            if (classified != InternalStatus::Secure)
            {
               if (why != nullptr)
               {
                  *why = classified == InternalStatus::Bogus
                     ? AnsiString("the alias at ") + candidate + " does not validate"
                     : AnsiString("the DS lookup for ") + candidate + " did not complete, or " + ZoneText(zone) + ", which is signed, did not prove what " + candidate + " is";
               }
               return classified;
            }

            if (kind == CutKind::Unsigned)
               return InternalStatus::Insecure;

            if (kind == CutKind::Signed)
            {
               const InternalStatus cut = ValidateZoneKeys(candidate, 1, keys, &keysWhy);
               if (cut != InternalStatus::Secure)
               {
                  if (why != nullptr)
                     *why = keysWhy;
                  return cut;
               }

               zone = candidate;
               RememberCut(candidate, CutKind::Signed);
            }
         }

         return InternalStatus::Secure;
      }

      // The verdict on name when its answer cannot speak for itself - it did
      // not come, or came without the signatures it needs: Insecure when the
      // name is proven below an unsigned delegation, Bogus when the chain to it
      // fails, and Indeterminate otherwise (RFC 7672 section 2.1.1). reason is
      // set to why, for a verdict other than Insecure, and the caller logs it
      // (QueryValidatedRrset_).
      DnssecResolver::ChainStatus WhereUnproven(const AnsiString &name, const AnsiString &what, AnsiString &reason)
      {
         AnsiString zone;
         AnsiString why;
         switch (FindEnclosingZone(name, zone, &why))
         {
         case InternalStatus::Insecure:
            return DnssecResolver::ChainStatus::Insecure;
         case InternalStatus::Bogus:
            reason = AnsiString("the answer for ") + name + " " + what + "; and the chain of trust above the name is bogus: " + why;
            return DnssecResolver::ChainStatus::Bogus;
         case InternalStatus::Secure:
            reason = name + " is in the signed zone " + NameText(zone) + ", and its answer " + what +
                     " (RFC 7672 section 2.1.1)";
            return DnssecResolver::ChainStatus::Indeterminate;
         case InternalStatus::Indeterminate:
            break;
         }

         reason = AnsiString("the answer for ") + name + " " + what + "; and whether the name is in a signed zone could not be told: " + why;
         return DnssecResolver::ChainStatus::Indeterminate;
      }
   }

   DnssecResolver::ChainStatus
   DnssecResolver::QueryValidatedRrset_(const AnsiString &query_name, unsigned short query_type,
                                        std::vector<std::vector<unsigned char>> &rdatas,
                                        bool strict, AnsiString &final_name, bool &nonexistent)
   {
      rdatas.clear();
      nonexistent = false;
      failure_reason_.Empty();

      AnsiString queryName = query_name;
      queryName.MakeLower();
      final_name = queryName;

      // A verdict of Bogus or Indeterminate, with why: kept for
      // GetFailureReason and said once in the debug log, the stage and the
      // check that failed in it - which RRset, which key, which signature.
      const AnsiString lookup = AnsiString("the ") + TypeText(query_type) + " lookup for " + NameText(queryName);
      auto failed = [&](ChainStatus status, const AnsiString &reason) -> ChainStatus
      {
         if (status != ChainStatus::Bogus && status != ChainStatus::Indeterminate)
            return status;

         failure_reason_ = reason;
         LOG_DEBUG("DNSSEC: " + String(lookup) + (status == ChainStatus::Bogus ? String(" is bogus: ") : String(" is indeterminate: ")) +
                   String(reason) + ".");
         return status;
      };

      // The final answer can only be Secure if every CNAME link in the
      // chain was validated as well (RFC 4035 section 5). Once a link is
      // proven insecure, so is everything after it, and nothing after it is
      // asked to prove more.
      bool aliasChainSecure = true;

      for (int hop = 0; hop <= MaxCnameHops; hop++)
      {
         final_name = queryName;

         DnsResponse response;
         if (!RunQuery(queryName, query_type, response))
         {
            LOG_DEBUG("DNSSEC: Lookup for " + String(queryName) + " failed at the transport level.");

            if (!strict || !aliasChainSecure)
               return ChainStatus::Insecure;

            AnsiString reason;
            const ChainStatus where = WhereUnproven(queryName, "did not come: no DNS server answered, or it answered SERVFAIL or a reply that does not parse", reason);
            return failed(where, reason);
         }

         std::vector<ParsedRr> typedRrset;
         std::vector<ParsedRr> cnameRrset;
         std::vector<RrsigInfo> signatures;

         for (const ParsedRr &record : response.answers)
         {
            if (record.owner != queryName)
               continue;

            if (record.type == query_type)
            {
               typedRrset.push_back(record);
            }
            else if (record.type == DnsTypeCname)
            {
               cnameRrset.push_back(record);
            }
            else if (record.type == DnsTypeRrsig)
            {
               RrsigInfo info;
               if (ParseRrsig(response, record, info))
                  signatures.push_back(info);
            }
         }

         // Said of an RRset that came without the signatures its zone publishes:
         // the DNS server did not return what the DO bit asked for, or something
         // on the way took it out.
         const AnsiString unsignedAnswer = "came without its signatures: the DNS server did not return the RRSIG records asked for (the DO bit), or something on the way removed them";

         if (!typedRrset.empty())
         {
            AnsiString why;
            const RrsetVerdict verdict = ValidateRrset(response, typedRrset, signatures, &why);
            const AnsiString rrsetText = AnsiString("the ") + TypeText(query_type) + " RRset of " + NameText(queryName);

            if (verdict == RrsetVerdict::Bogus)
               return failed(ChainStatus::Bogus, rrsetText + " does not validate: " + why);

            ChainStatus status = ChainStatus::Insecure;
            AnsiString reason;

            if (verdict == RrsetVerdict::Secure)
            {
               status = aliasChainSecure ? ChainStatus::Secure : ChainStatus::Insecure;
            }
            else if (verdict != RrsetVerdict::InsecureZone && strict && aliasChainSecure)
            {
               // No signature applies, or the signer's keys could not be had: the
               // records are not taken as unsigned unless the name is.
               if (verdict == RrsetVerdict::Indeterminate)
               {
                  status = ChainStatus::Indeterminate;
                  reason = rrsetText + " could not be validated: " + why;
               }
               else
               {
                  status = WhereUnproven(queryName, unsignedAnswer, reason);
               }
            }

            // The RDATA in canonical form (GetCanonicalRdata): a name in it -
            // an MX record's exchange - written out in full and in lowercase,
            // whatever compression the answer arrived with.
            if (status == ChainStatus::Secure || status == ChainStatus::Insecure)
            {
               for (const ParsedRr &record : typedRrset)
               {
                  std::vector<unsigned char> rdata;
                  if (GetCanonicalRdata(response, record, rdata))
                     rdatas.push_back(rdata);
               }
            }

            return failed(status, reason);
         }

         if (!cnameRrset.empty())
         {
            // Follow the alias, but only across validated links.
            AnsiString why;
            const RrsetVerdict verdict = ValidateRrset(response, cnameRrset, signatures, &why);

            if (verdict == RrsetVerdict::Bogus)
               return failed(ChainStatus::Bogus, AnsiString("the CNAME RRset of ") + queryName + " does not validate: " + why);

            if (verdict != RrsetVerdict::Secure)
            {
               if (verdict != RrsetVerdict::InsecureZone && strict && aliasChainSecure)
               {
                  AnsiString reason;
                  ChainStatus where = ChainStatus::Indeterminate;
                  if (verdict == RrsetVerdict::Indeterminate)
                     reason = AnsiString("the CNAME RRset of ") + queryName + " could not be validated: " + why;
                  else
                     where = WhereUnproven(queryName, AnsiString("is an alias that ") + unsignedAnswer, reason);

                  if (where != ChainStatus::Insecure)
                     return failed(where, reason);
               }

               aliasChainSecure = false;
            }

            AnsiString target;
            size_t offset = cnameRrset[0].rdata_offset;
            if (!ReadName(response.packet, offset, target) || target.IsEmpty())
            {
               if (strict && aliasChainSecure)
                  return failed(ChainStatus::Indeterminate, AnsiString("the CNAME record of ") + queryName + " could not be read");
               return ChainStatus::Insecure;
            }

            queryName = target;
            continue;
         }

         // NOERROR or NXDOMAIN without records.
         if (!strict || !aliasChainSecure)
            return ChainStatus::Insecure;

         AnsiString zone;
         AnsiString why;
         switch (FindEnclosingZone(queryName, zone, &why))
         {
         case InternalStatus::Insecure:
            return ChainStatus::Insecure;
         case InternalStatus::Bogus:
            return failed(ChainStatus::Bogus, AnsiString("the answer for ") + queryName + " holds no records, and the chain of trust above the name is bogus: " + why);
         case InternalStatus::Indeterminate:
            return failed(ChainStatus::Indeterminate, AnsiString("the answer for ") + queryName +
                          " holds no records, and whether the name is in a signed zone could not be told: " + why);
         case InternalStatus::Secure:
            break;
         }

         switch (VerifyAbsence(queryName, query_type, response, zone))
         {
         case Absence::NoData:
            return ChainStatus::Secure;
         case Absence::NoName:
            nonexistent = true;
            return ChainStatus::Secure;
         case Absence::OptOut:
            return ChainStatus::Insecure;
         case Absence::Unproven:
            break;
         }

         return failed(ChainStatus::Indeterminate, AnsiString("the answer for ") + queryName + " holds no records, and the signed zone " + NameText(zone) +
                       " gave no NSEC or NSEC3 proof, signed by it, that none exist: the DNS server did not return it, or something on the way removed it");
      }

      // Too many CNAME hops.
      if (strict && aliasChainSecure)
         return failed(ChainStatus::Indeterminate, AnsiString("the aliases from ") + query_name + " run past " + Number(MaxCnameHops) + " CNAME records");

      return ChainStatus::Insecure;
   }

   String
   DnssecResolver::GetFailureReason() const
   {
      return String(failure_reason_);
   }

   bool
   DnssecResolver::QueryRecords(const AnsiString &name, unsigned short query_type,
                                std::vector<DNSRecord> &records, int &status)
   {
      // The record types the server asks for. Anything else is a caller error
      // rather than a lookup failure, and is reported as such below.
      switch (query_type)
      {
      case 1:     // A
      case 5:     // CNAME
      case 12:    // PTR
      case 15:    // MX
      case 16:    // TXT
      case 28:    // AAAA
         break;
      default:
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5036, "DnssecResolver::QueryRecords",
            Formatter::Format(_T("Queried for unsupported record type {0}."), query_type));
         status = 9002;
         return false;
      }

      DnsResponse response;

      if (!RunQuery(name, query_type, response))
      {
         // Nothing answered: no server was configured, none was reachable, or
         // every one of them timed out. This is TEMPORARY - it must not become a
         // bounce - so it is not the "name does not exist" number.
         status = 9002;
         return false;
      }

      if (response.rcode == 3)
      {
         // NXDOMAIN. Permanent, and the one answer that lets a caller stop.
         status = 9003;
         return true;
      }

      // ToLower mutates and returns a reference, so the copy is made first.
      AnsiString wanted = name;
      wanted.ToLower();

      // A CNAME chain: the answer for "mail.example.com" may arrive as a CNAME to
      // "host.example.net" followed by that name's A records, and the owner of the
      // records that matter is the end of the chain rather than the name asked
      // for. The chain is followed by name so that the order records arrive in
      // does not decide the answer.
      AnsiString target = wanted;
      for (int hop = 0; hop < 8; hop++)
      {
         bool followed = false;
         for (const ParsedRr &record : response.answers)
         {
            if (record.type != 5 || record.owner != target)
               continue;
            // Reading the CNAME's target needs the whole packet, because the name
            // in it may be compressed against an earlier one.
            size_t offset = record.rdata_offset;
            AnsiString canonical;
            if (ReadName(response.packet, offset, canonical) && !canonical.IsEmpty())
            {
               canonical.ToLower();
               target = canonical;
               followed = true;
            }
            break;
         }
         if (!followed)
            break;
      }

      for (const ParsedRr &record : response.answers)
      {
         if (record.type != query_type)
            continue;
         if (record.owner != wanted && record.owner != target)
            continue;

         switch (query_type)
         {
         case 1:  // A - four octets, printed the way inet_ntop prints them, which
                  // is what WSAAddressToStringA gives on Windows.
            {
               if (record.rdata.size() != 4)
                  break;
               char text[INET_ADDRSTRLEN];
               struct in_addr address;
               memcpy(&address, record.rdata.data(), 4);
               if (inet_ntop(AF_INET, &address, text, sizeof(text)))
                  records.push_back(DNSRecord(AnsiString(text), query_type, 0));
               break;
            }
         case 28: // AAAA
            {
               if (record.rdata.size() != 16)
                  break;
               char text[INET6_ADDRSTRLEN];
               struct in6_addr address;
               memcpy(&address, record.rdata.data(), 16);
               if (inet_ntop(AF_INET6, &address, text, sizeof(text)))
                  records.push_back(DNSRecord(AnsiString(text), query_type, 0));
               break;
            }
         case 5:  // CNAME
         case 12: // PTR
            {
               size_t offset = record.rdata_offset;
               AnsiString host;
               if (ReadName(response.packet, offset, host) && !host.IsEmpty())
                  records.push_back(DNSRecord(host, query_type, 0));
               break;
            }
         case 15: // MX - a two-octet preference, then the exchange's name.
            {
               if (record.rdata.size() < 3)
                  break;
               const int preference = (static_cast<int>(record.rdata[0]) << 8) | record.rdata[1];
               size_t offset = record.rdata_offset + 2;
               AnsiString exchange;
               if (ReadName(response.packet, offset, exchange) && !exchange.IsEmpty())
                  records.push_back(DNSRecord(exchange, query_type, preference));
               break;
            }
         case 16: // TXT - one or more length-prefixed strings, joined without a
                  // separator, which is what the Windows client's string array
                  // amounts to for every consumer here (SPF, DKIM, DMARC).
            {
               AnsiString value;
               size_t position = 0;
               while (position < record.rdata.size())
               {
                  const size_t length = record.rdata[position];
                  if (position + 1 + length > record.rdata.size())
                     break;
                  value += AnsiString(reinterpret_cast<const char *>(record.rdata.data() + position + 1), length);
                  position += 1 + length;
               }
               records.push_back(DNSRecord(value, query_type, 0));
               break;
            }
         default:
            break;
         }
      }

      // The name resolved but carries no record of this type. Distinct from
      // NXDOMAIN, and the callers that ask for AAAA before A depend on it.
      status = records.empty() ? 9501 : 0;
      return true;
   }

   DnssecResolver::ChainStatus
   DnssecResolver::QueryTlsa(const String &host_name, int port, std::vector<TlsaRecord> &records)
   {
      records.clear();

      AnsiString narrowHost = host_name;
      narrowHost.MakeLower();

      AnsiString queryName;
      queryName.Format("_%d._tcp.%hs", port, narrowHost.c_str());

      std::vector<std::vector<unsigned char>> rdatas;
      AnsiString finalName;
      bool nonexistent = false;
      ChainStatus status = QueryValidatedRrset_(queryName, DnsTypeTlsa, rdatas, true, finalName, nonexistent);

      if (status != ChainStatus::Secure)
      {
         if (status == ChainStatus::Insecure && !rdatas.empty())
            LOG_DEBUG("DNSSEC: TLSA records for " + String(queryName) + " are under a delegation proven unsigned. Proceeding without DANE.");

         return status;
      }

      // The signed zone proved there is none.
      if (rdatas.empty())
         return ChainStatus::Secure;

      for (const std::vector<unsigned char> &rdata : rdatas)
      {
         if (rdata.size() < 4)
            continue;

         TlsaRecord tlsa;
         tlsa.usage = rdata[0];
         tlsa.selector = rdata[1];
         tlsa.matching_type = rdata[2];
         tlsa.data.assign(reinterpret_cast<const char*>(rdata.data() + 3), rdata.size() - 3);

         records.push_back(tlsa);
      }

      return records.empty() ? ChainStatus::Insecure : ChainStatus::Secure;
   }

   DnssecResolver::ChainStatus
   DnssecResolver::QueryMx(const String &domain, std::vector<AnsiString> &exchanges)
   //---------------------------------------------------------------------------
   // DESCRIPTION:
   // Looks up and validates the MX RRset for a domain.
   //
   // RFC 7672 section 2.2: DANE may only be applied to an MX host that was
   // itself learned from a DNSSEC-validated MX RRset. Without this, validating
   // the TLSA record proves only that the attacker-chosen host has a TLSA record
   // of its own - and an attacker able to forge the MX answer supplies both.
   //
   // exchanges is populated only for a Secure result, because a name from an
   // unvalidated answer is exactly what must not be trusted.
   //---------------------------------------------------------------------------
   {
      exchanges.clear();

      AnsiString queryName = domain;
      queryName.MakeLower();

      std::vector<std::vector<unsigned char>> rdatas;
      AnsiString finalName;
      bool nonexistent = false;
      ChainStatus status = QueryValidatedRrset_(queryName, DnsTypeMx, rdatas, true, finalName, nonexistent);

      if (status != ChainStatus::Secure)
         return status;

      if (rdatas.empty())
      {
         // The signed zone proved there is no MX RRset. The name is then its own
         // mail host, the implicit MX of RFC 5321 section 5.1 - the one host
         // DNSResolver::GetEmailServers may name, after the same CNAMEs - and with
         // a proof that the name does not exist there is no host at all. Until
         // 6.3.6 both were Insecure, and a forged plain answer chose the host.
         if (!nonexistent)
            exchanges.push_back(finalName);

         return ChainStatus::Secure;
      }

      for (const std::vector<unsigned char> &rdata : rdatas)
      {
         // 2 bytes of preference, then the exchange as an uncompressed wire-format
         // name: QueryValidatedRrset_ hands over the RDATA in canonical form, the
         // exchange written out and lowercased whatever compression the answer
         // came with (GetCanonicalRdata) - the form the signature was checked
         // over. Until work item 80 it handed over the RDATA as received, and
         // the RRSIG was checked over that too, so a compressed exchange - the
         // way nearly every resolver sends one - made the RRset Bogus. A pointer
         // here could only be a fault, so it is still refused rather than
         // followed into a packet this function does not have.
         if (rdata.size() < 3)
            continue;

         AnsiString exchange;
         size_t offset = 2;
         bool malformed = false;

         while (offset < rdata.size())
         {
            const unsigned char labelLength = rdata[offset];

            if (labelLength == 0)
               break;

            if ((labelLength & 0xC0) != 0)
            {
               malformed = true;
               break;
            }

            if (offset + 1 + labelLength > rdata.size())
            {
               malformed = true;
               break;
            }

            if (!exchange.IsEmpty())
               exchange += ".";

            exchange.append(reinterpret_cast<const char*>(rdata.data() + offset + 1), labelLength);
            offset += 1 + labelLength;
         }

         if (malformed || exchange.IsEmpty())
            continue;

         exchange.MakeLower();
         exchanges.push_back(exchange);
      }

      // A validated but unusable RRset is not Secure for our purposes: with no
      // name to compare against, the caller cannot establish that the host it is
      // about to contact is the one the domain published.
      return exchanges.empty() ? ChainStatus::Insecure : ChainStatus::Secure;
   }

   DnssecResolver::ChainStatus
   DnssecResolver::QueryTxt(const String &name, std::vector<AnsiString> &texts)
   {
      texts.clear();

      AnsiString queryName = name;
      queryName.MakeLower();

      std::vector<std::vector<unsigned char>> rdatas;
      AnsiString finalName;
      bool nonexistent = false;
      ChainStatus status = QueryValidatedRrset_(queryName, DnsTypeTxt, rdatas, false, finalName, nonexistent);

      if (status == ChainStatus::Bogus)
         return status;

      // TXT rdata is a sequence of length-prefixed character strings
      // which are concatenated to form the record value.
      for (const std::vector<unsigned char> &rdata : rdatas)
      {
         AnsiString text;
         size_t offset = 0;

         while (offset < rdata.size())
         {
            size_t segmentLength = rdata[offset];
            offset++;

            if (offset + segmentLength > rdata.size())
               break;

            text.append(reinterpret_cast<const char*>(rdata.data() + offset), segmentLength);
            offset += segmentLength;
         }

         texts.push_back(text);
      }

      return status;
   }

   void
   DnssecResolverTester::Test()
   {
      auto expect = [](bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(std::string("DnssecResolverTester: ") + what);
      };

      // A chosen server as text, "address port", the port in host order.
      auto text = [](const NameServer &server) -> std::string
      {
         char address[INET6_ADDRSTRLEN] = {};
         unsigned short port = 0;
         if (server.address.ss_family == AF_INET)
         {
            const sockaddr_in *v4 = reinterpret_cast<const sockaddr_in *>(&server.address);
            if (inet_ntop(AF_INET, &v4->sin_addr, address, sizeof(address)) == nullptr)
               return "?";
            port = ntohs(v4->sin_port);
         }
         else if (server.address.ss_family == AF_INET6)
         {
            const sockaddr_in6 *v6 = reinterpret_cast<const sockaddr_in6 *>(&server.address);
            if (inet_ntop(AF_INET6, &v6->sin6_addr, address, sizeof(address)) == nullptr)
               return "?";
            port = ntohs(v6->sin6_port);
         }
         else
         {
            return "?";
         }

         return std::string(address) + " " + std::to_string(port);
      };

      auto chosen = [&text](const std::string &resolvConf) -> std::string
      {
         std::string joined;
         for (const NameServer &server : ChooseNameServers(ReadResolvConf(resolvConf)))
         {
            if (!joined.empty())
               joined += ", ";
            joined += text(server);
         }
         return joined;
      };

      expect(chosen("nameserver 192.0.2.1\nnameserver 192.0.2.2\nnameserver 192.0.2.3\n") == "192.0.2.1 53, 192.0.2.2 53",
             "of three IPv4 servers, the first two are asked, on port 53.");

      // The case 6.3.6's re-check found: a host whose DNS servers are IPv6 alone.
      // Until it their servers were passed over, and the lookups reached none.
      expect(chosen("# written by the network\nsearch example.test\nnameserver 2001:db8::53\nnameserver 2001:db8::54 # second\n") ==
             "2001:db8::53 53, 2001:db8::54 53",
             "a system that lists IPv6 servers alone has them asked.");

      expect(chosen("nameserver 2001:db8::53\r\nnameserver 192.0.2.1\r\n") == "192.0.2.1 53",
             "a system that lists an IPv4 server has it asked, and not its IPv6 servers, as before.");

      expect(chosen("nameserver fec0:0:0:ffff::1\nnameserver fec0:0:0:ffff::2\nnameserver fec0:0:0:ffff::3\n").empty(),
             "the site-local servers Windows lists for an adapter given no IPv6 DNS server are not asked.");
      expect(chosen("nameserver fec0:0:0:ffff::1\nnameserver 2001:db8::53\n") == "2001:db8::53 53",
             "past the site-local servers, the next IPv6 server is asked.");

      expect(chosen("nameserver ::ffff:192.0.2.7\n") == "192.0.2.7 53",
             "an IPv4 address written as IPv6 is asked as the IPv4 address it is.");

      expect(chosen("nameservers 192.0.2.9\n  nameserver\t192.0.2.8;comment\nnameserver mail.example.test\nnameserver\n") == "192.0.2.8 53",
             "only a whole nameserver keyword with an address counts, and the address ends at a comment.");

      expect(chosen("").empty() && chosen("options edns0\n").empty(), "a file that names no server names none.");

      TestCanonicalForm_();
      TestCapturedAnswers_();
   }

   void
   DnssecResolverTester::TestCanonicalForm_()
   {
      auto expect = [](bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(std::string("DnssecResolverTester: ") + what);
      };

      // An answer for "Example.TEST" MX, by hand: the question at offset 12, and
      // one MX record whose owner points to it and whose RDATA is the given
      // octets, its RDLENGTH as given (the RDATA's own length by default).
      auto answer = [](const std::vector<unsigned char> &rdata, int rdlength = -1) -> std::vector<unsigned char>
      {
         std::vector<unsigned char> packet = { 0x12, 0x34, 0x81, 0x80, 0, 1, 0, 1, 0, 0, 0, 0 };
         const unsigned char question[] = { 7, 'E', 'x', 'a', 'm', 'p', 'l', 'e', 4, 'T', 'E', 'S', 'T', 0, 0, 15, 0, 1 };
         packet.insert(packet.end(), question, question + sizeof(question));
         const unsigned char header[] = { 0xC0, 0x0C, 0, 15, 0, 1, 0, 0, 0x0E, 0x10 };
         packet.insert(packet.end(), header, header + sizeof(header));
         const size_t length = rdlength < 0 ? rdata.size() : static_cast<size_t>(rdlength);
         Push16(packet, static_cast<unsigned short>(length));
         packet.insert(packet.end(), rdata.begin(), rdata.end());
         return packet;
      };

      auto canonical = [](const std::vector<unsigned char> &packet, std::vector<unsigned char> &out) -> bool
      {
         DnsResponse response;
         if (!ParseResponse(packet, response) || response.answers.size() != 1)
            return false;
         return GetCanonicalRdata(response, response.answers[0], out);
      };

      // "10 MaIl" and a pointer to the question's name, as a server compresses
      // an exchange under the domain it serves.
      const std::vector<unsigned char> compressed = { 0, 10, 4, 'M', 'a', 'I', 'l', 0xC0, 0x0C };
      const std::vector<unsigned char> expanded = { 0, 10, 4, 'm', 'a', 'i', 'l', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 4, 't', 'e', 's', 't', 0 };

      std::vector<unsigned char> out;
      expect(canonical(answer(compressed), out) && out == expanded,
             "an MX record's exchange, compressed against the question and in mixed case, is written out in full and in lowercase (RFC 4034 section 6.2), as its RRSIG signs it.");

      expect(canonical(answer(expanded), out) && out == expanded,
             "an exchange already in canonical form is taken as it is.");

      // The pointer in the RDATA aimed at itself: it does not point backwards,
      // and followed it would never end.
      std::vector<unsigned char> selfPointing = compressed;
      std::vector<unsigned char> probe = answer(compressed);
      const size_t rdataStart = probe.size() - compressed.size();
      selfPointing[7] = static_cast<unsigned char>(0xC0 | ((rdataStart + 7) >> 8));
      selfPointing[8] = static_cast<unsigned char>((rdataStart + 7) & 0xFF);
      expect(!canonical(answer(selfPointing), out), "a pointer in the RDATA that points at itself is refused.");

      // The pointer aimed forwards, at a whole name ("test") the packet holds
      // after the record: a name that would read, and only the rule that a
      // pointer points backwards (RFC 1035 section 4.1.4, "a prior occurrence")
      // refuses it.
      std::vector<unsigned char> forwardPointing = compressed;
      const size_t after = rdataStart + compressed.size();
      forwardPointing[7] = static_cast<unsigned char>(0xC0 | (after >> 8));
      forwardPointing[8] = static_cast<unsigned char>(after & 0xFF);
      std::vector<unsigned char> forwardPacket = answer(forwardPointing);
      const unsigned char laterName[] = { 4, 't', 'e', 's', 't', 0 };
      forwardPacket.insert(forwardPacket.end(), laterName, laterName + sizeof(laterName));
      expect(!canonical(forwardPacket, out), "a pointer in the RDATA that points forwards, to a name after the record, is refused.");

      // The name runs past the RDATA: its RDLENGTH ends inside the first label,
      // and the octets after it are still in the packet.
      DnsResponse parsed;
      expect(ParseResponse(answer(expanded, 6), parsed) && parsed.answers.size() == 1 && parsed.answers[0].rdata.size() == 6,
             "an answer whose RDLENGTH ends inside the exchange parses.");
      expect(!GetCanonicalRdata(parsed, parsed.answers[0], out), "an exchange that runs past the end of its RDATA is refused.");

      // A type with no name in its RDATA is taken as received, pointer-like
      // octets and all.
      ParsedRr txt;
      txt.type = DnsTypeTxt;
      txt.rdata = { 3, 'A', 0xC0, 0x0C };
      DnsResponse none;
      expect(GetCanonicalRdata(none, txt, out) && out == txt.rdata, "the RDATA of a type that holds no name is taken as received.");
   }

   void
   DnssecResolverTester::TestCapturedAnswers_()
   {
      auto expect = [](bool condition, const std::string &what)
      {
         if (!condition)
            throw std::logic_error(std::string("DnssecResolverTester: ") + what);
      };

      auto bytes = [](const char *hex) -> std::vector<unsigned char>
      {
         std::vector<unsigned char> out;
         HexToBytes(AnsiString(hex), out);
         return out;
      };

      // Installs a TestEnvironment on this thread for as long as it lives.
      struct Installed
      {
         explicit Installed(const TestEnvironment &environment) { test_environment = &environment; }
         ~Installed() { test_environment = nullptr; }
      };

      TestEnvironment captured;
      captured.now = DnssecCapturedAnswers::CapturedAt;
      captured.answers[std::make_pair(AnsiString(""), DnsTypeDnskey)] = bytes(DnssecCapturedAnswers::RootDnskey);
      captured.answers[std::make_pair(AnsiString("com"), DnsTypeDs)] = bytes(DnssecCapturedAnswers::ComDs);
      captured.answers[std::make_pair(AnsiString("com"), DnsTypeDnskey)] = bytes(DnssecCapturedAnswers::ComDnskey);
      captured.answers[std::make_pair(AnsiString("live.com"), DnsTypeDs)] = bytes(DnssecCapturedAnswers::LiveComDs);
      captured.answers[std::make_pair(AnsiString("live.com"), DnsTypeDnskey)] = bytes(DnssecCapturedAnswers::LiveComDnskey);
      captured.answers[std::make_pair(AnsiString("live.com"), DnsTypeMx)] = bytes(DnssecCapturedAnswers::LiveComMx);
      captured.answers[std::make_pair(AnsiString("cloudflare.com"), DnsTypeDs)] = bytes(DnssecCapturedAnswers::CloudflareComDs);
      captured.answers[std::make_pair(AnsiString("cloudflare.com"), DnsTypeDnskey)] = bytes(DnssecCapturedAnswers::CloudflareComDnskey);
      captured.answers[std::make_pair(AnsiString("cloudflare.com"), DnsTypeMx)] = bytes(DnssecCapturedAnswers::CloudflareComMx);

      // Work item 80: live.com's MX RRset, its exchange compressed against the
      // question, validated from the root's keys down. Until the fix it was Bogus,
      // and with DANE enforcement on no host of live.com was tried.
      {
         Installed installed(captured);
         DnssecResolver resolver;
         std::vector<AnsiString> exchanges;
         const DnssecResolver::ChainStatus status = resolver.QueryMx(String(_T("live.com")), exchanges);
         expect(status == DnssecResolver::ChainStatus::Secure,
                "live.com's MX RRset as the public DNS answers it - its exchange compressed against the question - validates from the root's keys down (work item 80): " +
                std::string(AnsiString(resolver.GetFailureReason()).c_str()));
         expect(exchanges.size() == 1 && exchanges[0] == "live-com.olc.protection.outlook.com",
                "live.com's one exchange is live-com.olc.protection.outlook.com, written out in full.");
      }

      // cloudflare.com: three exchanges point into the first one's RDATA.
      {
         Installed installed(captured);
         DnssecResolver resolver;
         std::vector<AnsiString> exchanges;
         const DnssecResolver::ChainStatus status = resolver.QueryMx(String(_T("cloudflare.com")), exchanges);
         expect(status == DnssecResolver::ChainStatus::Secure,
                "cloudflare.com's MX RRset, three of its exchanges compressed into the first one's RDATA, validates: " +
                std::string(AnsiString(resolver.GetFailureReason()).c_str()));

         std::set<AnsiString> names(exchanges.begin(), exchanges.end());
         expect(exchanges.size() == 4 &&
                names.count("mxa.global.inbound.cf-emailsecurity.net") == 1 && names.count("mxb.global.inbound.cf-emailsecurity.net") == 1 &&
                names.count("mxa-canary.global.inbound.cf-emailsecurity.net") == 1 && names.count("mxb-canary.global.inbound.cf-emailsecurity.net") == 1,
                "cloudflare.com's four exchanges are written out in full.");
      }

      // debian.org: its exchanges compressed against the question, under an
      // RSA/SHA-256 signature and an ECDSA P-256 one, each checked with the key
      // it names from the zone's own DNSKEY answer.
      {
         Installed installed(captured);

         DnsResponse mx;
         DnsResponse dnskey;
         expect(ParseResponse(bytes(DnssecCapturedAnswers::DebianOrgMx), mx) && ParseResponse(bytes(DnssecCapturedAnswers::DebianOrgDnskey), dnskey),
                "the captured debian.org answers parse.");

         std::vector<ParsedRr> rrset;
         std::vector<RrsigInfo> signatures;
         for (const ParsedRr &record : mx.answers)
         {
            RrsigInfo info;
            if (record.type == DnsTypeMx)
               rrset.push_back(record);
            else if (record.type == DnsTypeRrsig && ParseRrsig(mx, record, info) && info.type_covered == DnsTypeMx)
               signatures.push_back(info);
         }

         std::set<int> verifiedAlgorithms;
         for (const RrsigInfo &sig : signatures)
         {
            for (const ParsedRr &key : dnskey.answers)
            {
               if (key.type != DnsTypeDnskey || key.rdata.size() < 5 || key.rdata[3] != sig.algorithm || ComputeKeyTag(key.rdata) != sig.key_tag)
                  continue;

               if (CheckRrsigOverSet(mx, rrset, sig, key.rdata) == SignatureCheck::Verified)
                  verifiedAlgorithms.insert(sig.algorithm);
            }
         }

         expect(rrset.size() == 3 && verifiedAlgorithms.count(8) == 1 && verifiedAlgorithms.count(13) == 1,
                "debian.org's compressed MX RRset verifies under its RSA/SHA-256 signature and its ECDSA P-256 one.");
      }

      // The controls: what still fails, and says why.
      DnsResponse liveMx;
      expect(ParseResponse(captured.answers[std::make_pair(AnsiString("live.com"), DnsTypeMx)], liveMx) &&
             liveMx.answers.size() >= 2 && liveMx.answers[0].type == DnsTypeMx,
             "the captured live.com MX answer holds its MX record first.");

      RrsigInfo liveSig;
      for (const ParsedRr &record : liveMx.answers)
      {
         if (record.type == DnsTypeRrsig)
            ParseRrsig(liveMx, record, liveSig);
      }

      // The same answers a second after the MX RRSIG expired.
      {
         TestEnvironment later = captured;
         later.now = liveSig.expiration + 1;
         Installed installed(later);

         DnssecResolver resolver;
         std::vector<AnsiString> exchanges;
         const DnssecResolver::ChainStatus status = resolver.QueryMx(String(_T("live.com")), exchanges);
         const AnsiString why = resolver.GetFailureReason();
         expect(status == DnssecResolver::ChainStatus::Bogus && exchanges.empty() &&
                why.Find("key tag 44331") >= 0 && why.Find("expired at") >= 0,
                "an MX RRSIG past its expiration is Bogus, and the reason names the key tag and the expiry: " + std::string(why.c_str()));
      }

      // One letter of the exchange changed on the way: "olc" became "plc".
      {
         TestEnvironment tampered = captured;
         std::vector<unsigned char> &packet = tampered.answers[std::make_pair(AnsiString("live.com"), DnsTypeMx)];
         const unsigned char olc[] = { 3, 'o', 'l', 'c' };
         auto found = std::search(packet.begin(), packet.end(), olc, olc + sizeof(olc));
         expect(found != packet.end(), "the captured live.com MX answer spells olc.");
         *(found + 1) = 'p';
         Installed installed(tampered);

         DnssecResolver resolver;
         std::vector<AnsiString> exchanges;
         const DnssecResolver::ChainStatus status = resolver.QueryMx(String(_T("live.com")), exchanges);
         const AnsiString why = resolver.GetFailureReason();
         expect(status == DnssecResolver::ChainStatus::Bogus && exchanges.empty() &&
                why.Find("MX RRset of live.com") >= 0 && why.Find("does not match") >= 0,
                "an exchange changed on the way is Bogus, and the reason says the signature does not match: " + std::string(why.c_str()));
      }

      // The MX record without its RRSIG, as a resolver that does not return
      // DNSSEC data answers: live.com is in a signed zone, so Indeterminate.
      {
         TestEnvironment stripped = captured;
         const ParsedRr &first = liveMx.answers[0];
         std::vector<unsigned char> packet(liveMx.packet.begin(), liveMx.packet.begin() + static_cast<std::ptrdiff_t>(first.rdata_offset + first.rdata.size()));
         packet[6] = 0; packet[7] = 1;    // ANCOUNT 1
         packet[8] = 0; packet[9] = 0;    // NSCOUNT 0
         packet[10] = 0; packet[11] = 0;  // ARCOUNT 0
         stripped.answers[std::make_pair(AnsiString("live.com"), DnsTypeMx)] = packet;
         Installed installed(stripped);

         DnssecResolver resolver;
         std::vector<AnsiString> exchanges;
         const DnssecResolver::ChainStatus status = resolver.QueryMx(String(_T("live.com")), exchanges);
         const AnsiString why = resolver.GetFailureReason();
         expect(status == DnssecResolver::ChainStatus::Indeterminate && exchanges.empty() &&
                why.Find("signed zone live.com") >= 0 && why.Find("came without its signatures") >= 0,
                "an MX answer without its RRSIG from a signed zone is Indeterminate, and the reason says the DNS server did not return the signatures: " + std::string(why.c_str()));
      }

      // live.com's MX lookup against the answers given, on this thread.
      auto liveComMx = [](const TestEnvironment &environment, std::vector<AnsiString> &exchanges, AnsiString &why) -> DnssecResolver::ChainStatus
      {
         Installed installed(environment);
         DnssecResolver resolver;
         const DnssecResolver::ChainStatus status = resolver.QueryMx(String(_T("live.com")), exchanges);
         why = resolver.GetFailureReason();
         return status;
      };

      const ParsedRr *liveSigRecord = nullptr;
      for (const ParsedRr &record : liveMx.answers)
      {
         if (record.type == DnsTypeRrsig)
            liveSigRecord = &record;
      }
      expect(liveSigRecord != nullptr && liveSigRecord->rdata.size() > 20, "the captured live.com MX answer holds its RRSIG.");

      // One octet of the signature itself changed on the way.
      {
         TestEnvironment tampered = captured;
         std::vector<unsigned char> &packet = tampered.answers[std::make_pair(AnsiString("live.com"), DnsTypeMx)];
         packet[liveSigRecord->rdata_offset + liveSigRecord->rdata.size() - 1] ^= 0x01;

         std::vector<AnsiString> exchanges;
         AnsiString why;
         const DnssecResolver::ChainStatus status = liveComMx(tampered, exchanges, why);
         expect(status == DnssecResolver::ChainStatus::Bogus && exchanges.empty() && why.Find("key tag 44331") >= 0 && why.Find("does not match") >= 0,
                "an MX RRSIG whose signature was changed on the way is Bogus, and the reason says it does not match: " + std::string(why.c_str()));
      }

      // The RRSIG names key tag 44330, a key live.com does not publish.
      {
         TestEnvironment tampered = captured;
         std::vector<unsigned char> &packet = tampered.answers[std::make_pair(AnsiString("live.com"), DnsTypeMx)];
         packet[liveSigRecord->rdata_offset + 17] ^= 0x01;

         std::vector<AnsiString> exchanges;
         AnsiString why;
         const DnssecResolver::ChainStatus status = liveComMx(tampered, exchanges, why);
         expect(status == DnssecResolver::ChainStatus::Bogus && exchanges.empty() && why.Find("key tag 44330") >= 0 && why.Find("names no key live.com publishes") >= 0,
                "an MX RRSIG naming a key the zone does not publish is Bogus, and the reason says so: " + std::string(why.c_str()));
      }

      // A wrong DS: live.com's key-signing key, the one its DS record 28654
      // names, is not the key that came, so no DNSKEY of the zone matches.
      {
         TestEnvironment tampered = captured;
         std::vector<unsigned char> &packet = tampered.answers[std::make_pair(AnsiString("live.com"), DnsTypeDnskey)];
         DnsResponse keys;
         expect(ParseResponse(packet, keys), "the captured live.com DNSKEY answer parses.");

         bool altered = false;
         for (const ParsedRr &record : keys.answers)
         {
            if (record.type == DnsTypeDnskey && record.rdata.size() > 4 && record.rdata[0] == 1 && record.rdata[1] == 1)
            {
               packet[record.rdata_offset + record.rdata.size() - 1] ^= 0x01;
               altered = true;
            }
         }
         expect(altered, "the captured live.com DNSKEY answer holds its key-signing key.");

         std::vector<AnsiString> exchanges;
         AnsiString why;
         const DnssecResolver::ChainStatus status = liveComMx(tampered, exchanges, why);
         expect(status == DnssecResolver::ChainStatus::Bogus && exchanges.empty() && why.Find("no DNSKEY of live.com matches its DS records (key tag 28654") >= 0,
                "a zone none of whose keys matches its DS record is Bogus, and the reason names the DS record: " + std::string(why.c_str()));
      }

      // An MX record of 10 evil.example, signed by nobody, added after the
      // signed one: the RRset is no longer the one signed.
      // And the signed record again, written out and in capitals: in canonical
      // form the same record, which the signer counted once (RFC 4034 section
      // 6.3), so the RRset is the one signed.
      {
         const std::vector<unsigned char> &original = captured.answers[std::make_pair(AnsiString("live.com"), DnsTypeMx)];
         const size_t afterMx = liveMx.answers[0].rdata_offset + liveMx.answers[0].rdata.size();

         auto withRecord = [&](const char *exchange, unsigned short preference) -> std::vector<unsigned char>
         {
            std::vector<unsigned char> rdata;
            Push16(rdata, preference);
            AnsiString spelled(exchange);
            for (const AnsiString &label : StringParser::SplitString(spelled, "."))
            {
               rdata.push_back(static_cast<unsigned char>(label.GetLength()));
               rdata.insert(rdata.end(), label.c_str(), label.c_str() + label.GetLength());
            }
            rdata.push_back(0);

            std::vector<unsigned char> record = { 0xC0, 0x0C, 0, 15, 0, 1, 0, 0, 0x0E, 0x10 };
            Push16(record, static_cast<unsigned short>(rdata.size()));
            record.insert(record.end(), rdata.begin(), rdata.end());

            std::vector<unsigned char> packet = original;
            packet.insert(packet.begin() + static_cast<std::ptrdiff_t>(afterMx), record.begin(), record.end());
            packet[7] = static_cast<unsigned char>(packet[7] + 1);   // ANCOUNT, 2 in the capture
            return packet;
         };

         TestEnvironment added = captured;
         added.answers[std::make_pair(AnsiString("live.com"), DnsTypeMx)] = withRecord("evil.example", 1);

         std::vector<AnsiString> exchanges;
         AnsiString why;
         DnssecResolver::ChainStatus status = liveComMx(added, exchanges, why);
         expect(status == DnssecResolver::ChainStatus::Bogus && exchanges.empty() && why.Find("does not match") >= 0,
                "an MX record nobody signed, added to a signed RRset, makes it Bogus: " + std::string(why.c_str()));

         TestEnvironment repeated = captured;
         repeated.answers[std::make_pair(AnsiString("live.com"), DnsTypeMx)] = withRecord("LIVE-COM.OLC.Protection.OUTLOOK.com", 2);

         status = liveComMx(repeated, exchanges, why);
         expect(status == DnssecResolver::ChainStatus::Secure && !exchanges.empty() && why.IsEmpty(),
                "the signed MX record repeated, written out and in capitals - the same record in canonical form - is counted once, and the RRset validates: " + std::string(why.c_str()));
      }

      expect(test_environment == nullptr, "the tester's answers are taken off its thread when it is done.");
   }

}
