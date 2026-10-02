// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "BrandLogo.h"
#include "HttpsClient.h"
#include "../TCPIP/DNSResolver.h"
#include "../AntiSpam/DMARC/DMARC.h"
#include "../Application/Configuration.h"

#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>
#include <chrono>
#include <ctime>
#include <map>
#include <regex>
#include <stdexcept>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const size_t MaximumLogoBytes = 32 * 1024;
      const time_t KeptSeconds = 24 * 60 * 60;
      const size_t KeptDomains = 2000;

      struct Answer
      {
         time_t at = 0;
         BrandLogo::Outcome outcome = BrandLogo::NoRecord;
         AnsiString svg;
         AnsiString reason;
      };

      boost::mutex cacheLock;
      std::map<String, Answer> cache;

      // Organizational domains a lookup under which took SlowSeconds or more,
      // and when (BrandLogo::NoteUnreachable). Under cacheLock, bounded as the
      // cache is.
      // Keyed by the Public Suffix List alone (HoldKey), never by the DNS.
      const time_t UnreachableSeconds = 60 * 60;
      const time_t SlowSeconds = 3;
      std::map<String, time_t> unreachable;

      AnsiString Lower(const AnsiString &text)
      {
         AnsiString lower = text;
         lower.ToLower();
         return lower;
      }

      // The first v=BIMI1 record among a name's TXT records, if it has one.
      bool FindBimiRecord(const String &name, AnsiString &record)
      {
         DNSResolver resolver;
         std::vector<String> found;
         if (!resolver.GetTXTRecords(name, found))
            return false;

         for (size_t i = 0; i < found.size(); i++)
         {
            AnsiString text = AnsiString(found[i]);
            text.Trim();
            if (Lower(text).StartsWith("v=bimi1"))
            {
               record = text;
               return true;
            }
         }

         return false;
      }

      void Expect(bool condition, const char *message)
      {
         if (!condition)
            throw std::logic_error(message);
      }

      // The name a hold is kept under: the registrable domain by the Public
      // Suffix List, with no DNS query. Not DMARC::GetOrganizationalDomain,
      // which under DmarcTreeWalkEnabled (on by default) walks the DNS and
      // answers a name that publishes no DMARC record as its own organization:
      // a tarpit domain need only publish none for every subdomain of it to be
      // held apart, and the hold would never cover the next one.
      String HoldKey(const String &domain)
      {
         const String normalised = BrandLogo::NormaliseDomain(domain);
         if (normalised.IsEmpty())
            return String();
         return DMARC::GetOrganizationalDomainByPublicSuffixList(normalised);
      }
   }

   const char *
   BrandLogo::Word(Outcome outcome)
   {
      switch (outcome)
      {
      case Found: return "found";
      case BadDomain: return "bad_domain";
      case SwitchedOff: return "switched_off";
      case NoRecord: return "no_record";
      case Declined: return "declined";
      case WeakPolicy: return "weak_policy";
      case NotHttps: return "not_https";
      case FetchFailed: return "fetch_failed";
      case NotTinyPs: return "not_tiny_ps";
      }
      return "no_record";
   }

   String
   BrandLogo::NormaliseDomain(const String &text)
   {
      String domain = text;
      domain.Trim();
      domain.ToLower();
      if (domain.GetLength() < 3 || domain.GetLength() > 253)
         return String();

      int dots = 0;
      int labelLength = 0;
      for (int i = 0; i < domain.GetLength(); i++)
      {
         wchar_t c = domain[i];
         if (c == '.')
         {
            if (labelLength == 0)
               return String();
            dots++;
            labelLength = 0;
            continue;
         }
         const bool letter = c >= 'a' && c <= 'z';
         const bool digit = c >= '0' && c <= '9';
         if (!letter && !digit && c != '-')
            return String();
         if (c == '-' && labelLength == 0)
            return String();
         labelLength++;
         if (labelLength > 63)
            return String();
      }

      if (dots == 0 || labelLength == 0)
         return String();

      return domain;
   }

   bool
   BrandLogo::ParseRecord(const AnsiString &record, AnsiString &location, AnsiString &authority)
   {
      location = "";
      authority = "";

      std::vector<AnsiString> tags = StringParser::SplitString(record, ";");
      bool version = false;
      for (size_t i = 0; i < tags.size(); i++)
      {
         AnsiString tag = tags[i];
         tag.Trim();
         if (tag.IsEmpty())
            continue;

         const int equals = tag.Find("=");
         if (equals <= 0)
            return false;

         AnsiString name = Lower(tag.Mid(0, equals));
         name.Trim();
         AnsiString value = tag.Mid(equals + 1);
         value.Trim();

         // The version comes first, and exactly so (the draft's rule, as DMARC's).
         if (i == 0)
         {
            if (name != "v" || value != "BIMI1")
               return false;
            version = true;
            continue;
         }

         if (name == "l")
            location = value;
         else if (name == "a")
            authority = value;
      }

      return version;
   }

   bool
   BrandLogo::PolicyEnforces(const String &p, const String &sp, int pct, bool atOrganizationalDomain)
   {
      if (pct != 100)
         return false;

      String policy = p;
      policy.ToLower();
      if (policy != _T("quarantine") && policy != _T("reject"))
         return false;

      // The organization's record governs its subdomains through sp=, and an
      // sp=none lets every subdomain's mail through unenforced.
      if (atOrganizationalDomain && !sp.IsEmpty())
      {
         String sub = sp;
         sub.ToLower();
         if (sub != _T("quarantine") && sub != _T("reject"))
            return false;
      }

      return true;
   }

   bool
   BrandLogo::IsTinyPs(const AnsiString &svg, AnsiString &reason)
   {
      if (svg.GetLength() == 0)
      {
         reason = "The logo is empty.";
         return false;
      }
      if ((size_t) svg.GetLength() > MaximumLogoBytes)
      {
         reason = "The logo is larger than 32 KB.";
         return false;
      }

      const std::string lower = std::string(Lower(svg).c_str());

      const int root = (int) lower.find("<svg");
      if (root < 0)
      {
         reason = "The logo is not an SVG document.";
         return false;
      }

      const size_t rootEnd = lower.find('>', root);
      if (rootEnd == std::string::npos)
      {
         reason = "The logo's svg element is not closed.";
         return false;
      }

      const std::string rootTag = lower.substr(root, rootEnd - root);
      if (rootTag.find("baseprofile=\"tiny-ps\"") == std::string::npos && rootTag.find("baseprofile='tiny-ps'") == std::string::npos)
      {
         reason = "The logo does not declare baseProfile=\"tiny-ps\".";
         return false;
      }

      // SVG Tiny PS places the logo in the square it is given: no x or y on the root.
      if (std::regex_search(rootTag, std::regex("\\s(x|y)\\s*=")))
      {
         reason = "The logo's svg element carries x or y, which SVG Tiny PS does not allow.";
         return false;
      }

      if (lower.find("<title") == std::string::npos)
      {
         reason = "The logo has no title, which SVG Tiny PS requires.";
         return false;
      }

      static const char *forbidden[] =
      {
         "<!doctype", "<!entity", "<script", "<foreignobject", "<image", "<a ", "<a>", "<a\t", "<a\n", "<a\r",
         "<animate", "<set", "<iframe", "<video", "<audio", "<style", "javascript:", "data:",
         "href=\"http", "href='http", "href=\"//", "href='//", "url(http", "url(//", "@import"
      };
      for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++)
      {
         if (lower.find(forbidden[i]) != std::string::npos)
         {
            reason = AnsiString("The logo contains ") + forbidden[i] + ", which SVG Tiny PS does not allow.";
            return false;
         }
      }

      if (std::regex_search(lower, std::regex("\\son[a-z]+\\s*=")))
      {
         reason = "The logo carries an event attribute, which SVG Tiny PS does not allow.";
         return false;
      }

      reason = "";
      return true;
   }

   void
   BrandLogo::ClearCache()
   {
      boost::lock_guard<boost::mutex> guard(cacheLock);
      cache.clear();
      unreachable.clear();
   }

   void
   BrandLogo::NoteUnreachable(const String &domain)
   {
      const String organizational = HoldKey(domain);
      if (organizational.IsEmpty())
         return;

      boost::lock_guard<boost::mutex> guard(cacheLock);
      if (unreachable.size() >= KeptDomains)
         unreachable.clear();
      unreachable[organizational] = std::time(nullptr);
   }

   bool
   BrandLogo::OrganizationUnreachable(const String &domain)
   {
      const String organizational = HoldKey(domain);
      if (organizational.IsEmpty())
         return false;

      boost::lock_guard<boost::mutex> guard(cacheLock);
      std::map<String, time_t>::const_iterator held = unreachable.find(organizational);
      return held != unreachable.end() && std::time(nullptr) - held->second < UnreachableSeconds;
   }

   bool
   BrandLogo::Known(const String &requested, Outcome &outcome, AnsiString &svg, AnsiString &reason)
   {
      svg = "";

      const String domain = NormaliseDomain(requested);
      if (domain.IsEmpty())
      {
         reason = "That is not a domain name.";
         outcome = BadDomain;
         return true;
      }

      if (!Configuration::Instance()->GetBrandLogosEnabled())
      {
         reason = "Brand logos are switched off on this server (BrandLogosEnabled).";
         outcome = SwitchedOff;
         return true;
      }

      const time_t now = std::time(nullptr);
      {
         boost::lock_guard<boost::mutex> guard(cacheLock);
         std::map<String, Answer>::const_iterator known = cache.find(domain);
         if (known != cache.end() && now - known->second.at < KeptSeconds)
         {
            svg = known->second.svg;
            reason = known->second.reason;
            outcome = known->second.outcome;
            return true;
         }
      }

      if (OrganizationUnreachable(domain))
      {
         reason = "Looking up a logo under this domain's organization took three seconds or more within the last hour, so it is not asked again yet.";
         outcome = FetchFailed;
         return true;
      }

      return false;
   }

   BrandLogo::Outcome
   BrandLogo::Lookup(const String &requested, AnsiString &svg, AnsiString &reason)
   {
      Outcome kept = NoRecord;
      if (Known(requested, kept, svg, reason))
         return kept;

      const String domain = NormaliseDomain(requested);
      const time_t now = std::time(nullptr);

      // Everything from here is on the clock the hold is kept by (the end of
      // this function), not only the fetch: the DNS comes first, and it is the
      // owner of the name who answers it.
      const std::chrono::steady_clock::time_point asking = std::chrono::steady_clock::now();

      Answer answer;
      answer.at = now;

      // The record at the domain, and at the organizational domain when the
      // domain has none - a brand publishes once for all of its subdomains.
      AnsiString record;
      bool found = FindBimiRecord(_T("default._bimi.") + domain, record);
      if (!found)
      {
         const String organizational = DMARC::GetOrganizationalDomain(domain);
         if (!organizational.IsEmpty() && organizational.CompareNoCase(domain) != 0)
            found = FindBimiRecord(_T("default._bimi.") + organizational, record);
      }

      AnsiString location, authority;
      if (!found)
      {
         answer.outcome = NoRecord;
         answer.reason = "The domain publishes no BIMI record.";
      }
      else if (!ParseRecord(record, location, authority))
      {
         answer.outcome = NoRecord;
         answer.reason = "The domain's BIMI record cannot be read.";
      }
      else if (location.IsEmpty())
      {
         answer.outcome = Declined;
         answer.reason = "The domain's BIMI record declines to publish a logo.";
      }
      else
      {
         DMARC dmarc;
         DMARC::Evaluation evaluation;
         std::vector<AnsiString> noDkim;
         dmarc.Verify(domain, domain, true, noDkim, &evaluation);
         const bool atOrganization = evaluation.policy_found && evaluation.policy_domain.CompareNoCase(domain) != 0;

         if (!evaluation.policy_found || !PolicyEnforces(evaluation.p, evaluation.sp, evaluation.pct, atOrganization))
         {
            answer.outcome = WeakPolicy;
            answer.reason = "The domain's DMARC policy is not quarantine or reject for all of its mail, so its logo is not shown.";
         }
         else if (!Lower(location).StartsWith("https://"))
         {
            answer.outcome = NotHttps;
            answer.reason = "The logo's location is not an https address.";
         }
         else
         {
            HttpsClient::Response response;
            String error;
            std::vector<AnsiString> headers;
            headers.push_back("Accept: image/svg+xml");
            // Public addresses only, and through HttpProxy when one is set
            // (RequestPublicThroughProxy): until 6.3.6 a server that reaches
            // the web only through its proxy could fetch no logo.
            const bool ok = HttpsClient::RequestPublicThroughProxy("GET", location, headers, "", "", response, error, 10, MaximumLogoBytes);
            if (!ok || response.status_code != 200)
            {
               answer.outcome = FetchFailed;
               answer.reason = ok ? AnsiString("The logo's address answered ") + AnsiString(StringParser::IntToString(response.status_code)) + "."
                                  : AnsiString("The logo could not be fetched: ") + AnsiString(error);
            }
            else
            {
               AnsiString why;
               if (!IsTinyPs(response.body, why))
               {
                  answer.outcome = NotTinyPs;
                  answer.reason = why;
               }
               else
               {
                  answer.outcome = Found;
                  answer.svg = response.body;
                  answer.reason = "";
               }
            }
         }
      }

      // A lookup that held this worker SlowSeconds or more is held against the
      // organization (NoteUnreachable), whatever it came to. Holding only a
      // logo host that never answered left the wait to whoever chose it: the
      // owner of a name runs its DNS too, and a server that answers each query
      // for a fresh subdomain after a wait - up to DNSQueryTimeout apiece, for
      // the record, the tree walk and the policy - or a logo host that answers
      // 404, or a logo, after nine seconds, costs a worker as much as silence
      // does. The name asked keeps its own answer, kept below; the hold is on
      // every other name under the organization, for an hour. A quick answer of
      // any kind costs nothing and is not held.
      if (std::chrono::steady_clock::now() - asking >= std::chrono::seconds(SlowSeconds))
         NoteUnreachable(domain);

      {
         boost::lock_guard<boost::mutex> guard(cacheLock);
         if (cache.size() >= KeptDomains)
            cache.clear();
         cache[domain] = answer;
      }

      svg = answer.svg;
      reason = answer.reason;
      return answer.outcome;
   }

   void
   BrandLogoTester::Test()
   {
      typedef BrandLogo B;

      Expect(B::NormaliseDomain(_T(" Example.COM ")) == _T("example.com"), "A domain is lower-cased and trimmed.");
      Expect(B::NormaliseDomain(_T("localhost")).IsEmpty(), "A single label is not a domain here.");
      Expect(B::NormaliseDomain(_T("a..b")).IsEmpty(), "An empty label is refused.");
      Expect(B::NormaliseDomain(_T("-a.example")).IsEmpty(), "A label beginning with a hyphen is refused.");
      Expect(B::NormaliseDomain(_T("ex ample.com")).IsEmpty(), "A space is refused.");
      Expect(B::NormaliseDomain(_T("a/b.example")).IsEmpty(), "A slash is refused.");

      AnsiString location, authority;
      Expect(B::ParseRecord("v=BIMI1; l=https://example.com/logo.svg; a=https://example.com/vmc.pem", location, authority) &&
             location == "https://example.com/logo.svg" && authority == "https://example.com/vmc.pem", "A full record is read.");
      Expect(B::ParseRecord("v=BIMI1; l=; a=;", location, authority) && location.IsEmpty(), "An empty l= declines.");
      Expect(!B::ParseRecord("l=https://example.com/logo.svg; v=BIMI1", location, authority), "The version comes first.");
      Expect(!B::ParseRecord("v=bimi2; l=https://example.com/logo.svg", location, authority), "Another version is not read.");

      Expect(B::PolicyEnforces(_T("reject"), _T(""), 100, false), "p=reject enforces.");
      Expect(B::PolicyEnforces(_T("Quarantine"), _T(""), 100, false), "p=quarantine enforces, whatever its case.");
      Expect(!B::PolicyEnforces(_T("none"), _T(""), 100, false), "p=none does not.");
      Expect(!B::PolicyEnforces(_T("reject"), _T(""), 50, false), "pct under 100 does not.");
      Expect(!B::PolicyEnforces(_T("reject"), _T("none"), 100, true), "sp=none at the organization does not, for a subdomain.");
      Expect(B::PolicyEnforces(_T("reject"), _T("none"), 100, false), "sp does not govern the organization's own mail.");

      const AnsiString good =
         "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<svg version=\"1.2\" baseProfile=\"tiny-ps\" xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 100\">"
         "<title>Example</title><circle cx=\"50\" cy=\"50\" r=\"40\" fill=\"#1a73e8\"/></svg>";
      AnsiString why;
      Expect(B::IsTinyPs(good, why), "A Tiny PS logo is accepted.");
      Expect(!B::IsTinyPs("<svg xmlns=\"http://www.w3.org/2000/svg\"><title>x</title></svg>", why), "No tiny-ps profile is refused.");
      Expect(!B::IsTinyPs("<svg baseProfile=\"tiny-ps\"><circle r=\"1\"/></svg>", why), "No title is refused.");
      Expect(!B::IsTinyPs("<svg baseProfile=\"tiny-ps\"><title>x</title><script>alert(1)</script></svg>", why), "A script is refused.");
      Expect(!B::IsTinyPs("<svg baseProfile=\"tiny-ps\" onload=\"alert(1)\"><title>x</title></svg>", why), "An event attribute is refused.");
      Expect(!B::IsTinyPs("<svg baseProfile=\"tiny-ps\"><title>x</title><image href=\"https://tracker.example/p.gif\"/></svg>", why), "An image is refused.");
      Expect(!B::IsTinyPs("<svg baseProfile=\"tiny-ps\"><title>x</title><use href=\"http://x.example/s.svg#a\"/></svg>", why), "An external reference is refused.");
      Expect(!B::IsTinyPs("<!DOCTYPE svg [<!ENTITY a \"b\">]><svg baseProfile=\"tiny-ps\"><title>x</title></svg>", why), "An entity is refused.");
      Expect(!B::IsTinyPs("<svg baseProfile=\"tiny-ps\" x=\"10\"><title>x</title></svg>", why), "x on the root is refused.");
      Expect(!B::IsTinyPs("<svg baseProfile=\"tiny-ps\"><title>x</title><a href=\"#b\">b</a></svg>", why), "A link is refused.");
      const std::string padding(33000, ' ');
      Expect(!B::IsTinyPs(AnsiString(padding.c_str()) + good, why), "More than 32 KB is refused.");
      Expect(!B::IsTinyPs("<html><body>not a logo</body></html>", why), "Not an SVG is refused.");

      // OUT-2: a logo host that never answered is held against the organization,
      // so a fresh subdomain of it is not another ten seconds on a REST worker.
      // The old cache was keyed by the full name alone and had no such hold.
      //
      // The organization is the Public Suffix List's, never the DNS tree walk's:
      // tarpit.example publishes no DMARC record, and the walk (on by default)
      // answers such a name as its own organization, so keyed by it b2 was not
      // held for a1 - which is how this tester failed in the 6.3.4 gate, under
      // the regression suite's zone, where every lookup succeeds with nothing.
      B::ClearCache();
      Expect(!B::OrganizationUnreachable(_T("a1.tarpit.example")), "Nothing is held before anything failed.");
      B::NoteUnreachable(_T("a1.tarpit.example"));
      Expect(B::OrganizationUnreachable(_T("b2.tarpit.example")), "Another subdomain of the same organization is held.");
      Expect(B::OrganizationUnreachable(_T("tarpit.example")), "The organization's own name is held.");
      Expect(!B::OrganizationUnreachable(_T("tarpit2.example")), "Another organization is not held.");
      Expect(!B::OrganizationUnreachable(_T("tarpit.example.net")), "A name that merely contains it is not held.");
      B::ClearCache();
      Expect(!B::OrganizationUnreachable(_T("b2.tarpit.example")), "Clearing the cache clears the hold.");

      // Under a public suffix of two labels the organization is three: the hold
      // covers tarpit.co.uk and every name under it, and neither its neighbour
      // under co.uk nor the suffix itself.
      B::NoteUnreachable(_T("a1.tarpit.co.uk"));
      Expect(B::OrganizationUnreachable(_T("b2.tarpit.co.uk")), "Another subdomain under a two-label suffix is held.");
      Expect(B::OrganizationUnreachable(_T("x.y.Tarpit.Co.Uk")), "A deeper name under a two-label suffix is held, whatever its case.");
      Expect(B::OrganizationUnreachable(_T("tarpit.co.uk")), "The organization under a two-label suffix is held.");
      Expect(!B::OrganizationUnreachable(_T("tarpit2.co.uk")), "Another organization under the same suffix is not held.");
      Expect(!B::OrganizationUnreachable(_T("co.uk")), "The public suffix itself is not held.");
      B::ClearCache();
   }
}
