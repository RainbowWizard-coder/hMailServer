// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "CountryBlocking.h"

#include "GeoIpDatabase.h"
#include "ServerStatus.h"
#include "Time.h"
#include "../Application/Configuration.h"
#include "../BO/Domain.h"
#include "../Cache/CacheContainer.h"
#include "../TCPIP/IPAddress.h"
#include "../TCPIP/ProxyProtocol.h"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // How long the loaded file is trusted before its size and modification
      // time are looked at again. An administrator who replaces the database
      // does not want to restart the service, and the accept path does not want
      // a stat() per connection; five minutes is the compromise.
      const int RECHECK_SECONDS = 300;

      AnsiString JsonEscape(const AnsiString &value)
      {
         AnsiString out;

         for (int i = 0; i < value.GetLength(); i++)
         {
            const unsigned char c = (unsigned char) value.GetAt(i);

            if (c == '"' || c == '\\')
            {
               out += '\\';
               out += (char) c;
            }
            else if (c < 0x20)
            {
               AnsiString escaped;
               escaped.Format("\\u%04x", (int) c);
               out += escaped;
            }
            else
            {
               out += (char) c;
            }
         }

         return out;
      }
   }

   CountryBlocking::CountryBlocking() :
      file_size_(0),
      file_written_(0),
      next_recheck_(0),
      reported_missing_(false)
   {
   }

   CountryBlocking::~CountryBlocking()
   {
   }

   // ------------------------------------------------------------------ grammar

   bool
   CountryBlocking::ParsePolicy(const String &text, Policy &policy, String &error)
   {
      policy = Policy();
      error.Empty();

      String value = text;
      value.Trim();

      if (value.GetLength() > MaxPolicyLength)
      {
         error = Formatter::Format(_T("A country policy may be at most {0} characters. Use an allow list rather than a very long block list."),
            StringParser::IntToString(MaxPolicyLength));
         return false;
      }

      if (value.IsEmpty())
         return true;

      String keyword = value;
      String list;

      const int colon = value.Find(_T(":"));

      if (colon >= 0)
      {
         keyword = value.Left(colon);
         list = value.Mid(colon + 1);
      }

      keyword.Trim();
      keyword.MakeLower();

      if (keyword == _T("off"))
      {
         if (!list.Trim().IsEmpty())
         {
            error = _T("A policy of off takes no countries. Write off on its own, or allow: or block: followed by the countries.");
            return false;
         }

         return true;
      }

      if (keyword == _T("allow"))
         policy.mode = ModeAllow;
      else if (keyword == _T("block"))
         policy.mode = ModeBlock;
      else
      {
         error = Formatter::Format(_T("{0} is not a country policy. Write off, allow:GB,IE or block:CN,RU."), value);
         return false;
      }

      if (colon < 0)
      {
         error = Formatter::Format(_T("{0} names no countries. Write {0}: followed by two-letter country codes, or off."), keyword);
         return false;
      }

      std::vector<String> parts = StringParser::SplitString(list, _T(","));

      for (String part : parts)
      {
         part.Trim();

         if (part.IsEmpty())
            continue;

         const wchar_t first = part.GetLength() == 2 ? part.GetAt(0) : 0;
         const wchar_t second = part.GetLength() == 2 ? part.GetAt(1) : 0;

         const bool isLetterPair =
            ((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z')) &&
            ((second >= 'a' && second <= 'z') || (second >= 'A' && second <= 'Z'));

         if (!isLetterPair)
         {
            error = Formatter::Format(_T("{0} is not a two-letter country code. A country is written as its ISO 3166-1 code, such as GB, DE or US."), part);
            return false;
         }

         part.MakeUpper();
         policy.countries.insert(AnsiString(part));
      }

      if (policy.countries.empty())
      {
         error = Formatter::Format(_T("{0} names no countries, so it would {1}. Write off if that is what you mean."),
            value, policy.mode == ModeAllow ? _T("refuse everything") : _T("refuse nothing"));
         return false;
      }

      return true;
   }

   String
   CountryBlocking::FormatPolicy(const Policy &policy)
   {
      if (policy.mode == ModeOff || policy.countries.empty())
         return _T("");

      String out = policy.mode == ModeAllow ? _T("allow:") : _T("block:");
      bool first = true;

      for (std::set<AnsiString>::const_iterator it = policy.countries.begin(); it != policy.countries.end(); ++it)
      {
         if (!first)
            out += _T(",");

         out += String(*it);
         first = false;
      }

      return out;
   }

   bool
   CountryBlocking::Canonicalise(const String &text, String &canonical, String &error)
   {
      Policy policy;

      if (!ParsePolicy(text, policy, error))
         return false;

      canonical = FormatPolicy(policy);
      return true;
   }

   String
   CountryBlocking::SurfaceName(Surface surface)
   {
      switch (surface)
      {
      case SurfaceSmtp:
         return _T("smtp");
      case SurfaceImap:
         return _T("imap");
      case SurfacePop3:
         return _T("pop3");
      default:
         return _T("http");
      }
   }

   bool
   CountryBlocking::SurfaceFromName(const String &name, Surface &surface)
   {
      String lower = name;
      lower.Trim();
      lower.MakeLower();

      if (lower == _T("smtp"))
         surface = SurfaceSmtp;
      else if (lower == _T("imap"))
         surface = SurfaceImap;
      else if (lower == _T("pop3"))
         surface = SurfacePop3;
      else if (lower == _T("http") || lower == _T("web") || lower == _T("rest"))
         surface = SurfaceHttp;
      else
         return false;

      return true;
   }

   // ------------------------------------------------------------------ policy

   CountryBlocking::Policy
   CountryBlocking::PolicyFor(Surface surface)
   {
      Policy policy;
      String error;

      String stored;

      switch (surface)
      {
      case SurfaceSmtp:
         stored = Configuration::Instance()->GetCountryPolicySMTP();
         break;
      case SurfaceImap:
         stored = Configuration::Instance()->GetCountryPolicyIMAP();
         break;
      case SurfacePop3:
         stored = Configuration::Instance()->GetCountryPolicyPOP3();
         break;
      default:
         stored = Configuration::Instance()->GetCountryPolicyHTTP();
         break;
      }

      // A stored value that does not parse is treated as off rather than as a
      // refusal of everything: a setting written by hand into the database must
      // not be able to close the server.
      if (!ParsePolicy(stored, policy, error))
      {
         LOG_DEBUG("CountryBlocking: the stored " + SurfaceName(surface) + " policy could not be read (" + error + "); nothing is judged by country on that listener.");
         return Policy();
      }

      return policy;
   }

   bool
   CountryBlocking::Allows(const Policy &policy, const AnsiString &country)
   {
      if (policy.mode == ModeOff || policy.countries.empty())
         return true;

      if (policy.mode == ModeAllow)
      {
         // An allow list admits the countries it names and nothing else, and an
         // address whose country is not known is not one of them. That is what
         // an allow list means; the setting's description says so.
         if (country.IsEmpty())
            return false;

         return policy.countries.find(country) != policy.countries.end();
      }

      // A block list refuses the countries it names. An address with no known
      // country is not named, so it is allowed.
      if (country.IsEmpty())
         return true;

      return policy.countries.find(country) == policy.countries.end();
   }

   // ----------------------------------------------------------------- database

   void
   CountryBlocking::Invalidate()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      database_.reset();
      configured_path_.Empty();
      load_error_.Empty();
      file_size_ = 0;
      file_written_ = 0;
      next_recheck_ = 0;
      reported_missing_ = false;
   }

   std::shared_ptr<const GeoIpDatabase>
   CountryBlocking::Database_()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      const String path = Configuration::Instance()->GetGeoIpDatabaseFile();
      const time_t now = ::time(nullptr);

      if (path.IsEmpty())
      {
         database_.reset();
         configured_path_.Empty();
         load_error_.Empty();
         return std::shared_ptr<const GeoIpDatabase>();
      }

      bool reload = false;

      if (path != configured_path_)
      {
         reload = true;
      }
      else if (now >= next_recheck_)
      {
         next_recheck_ = now + RECHECK_SECONDS;

         // A file that has been replaced since it was read. Size and modification
         // time together, because an update that kept the size is a rebuild of
         // the same database and an update that kept the time is not a thing
         // any tool does.
         boost::system::error_code sizeError;
         boost::system::error_code timeError;

         const boost::uintmax_t size = boost::filesystem::file_size(path.c_str(), sizeError);
         const std::time_t written = boost::filesystem::last_write_time(path.c_str(), timeError);

         if (sizeError || timeError)
            reload = true;
         else if ((unsigned __int64) size != file_size_ || (__int64) written != file_written_)
            reload = true;
      }

      if (!reload)
         return database_;

      configured_path_ = path;
      next_recheck_ = now + RECHECK_SECONDS;

      std::shared_ptr<GeoIpDatabase> fresh = std::shared_ptr<GeoIpDatabase>(new GeoIpDatabase());
      String error;

      if (!fresh->LoadFile(path, error))
      {
         load_error_ = error;

         // Said once, not once per connection. The counter that matters is in
         // the status, which the Control Panel, the Deck and hmctl all show.
         if (!reported_missing_)
         {
            reported_missing_ = true;
            LOG_APPLICATION("Country blocking: the geographic database could not be read, so no connection is judged by country. " + error);
         }

         // The database that was working is kept: a file replaced badly must not
         // silently stop a policy that was in force.
         return database_;
      }

      reported_missing_ = false;
      load_error_.Empty();
      database_ = fresh;

      boost::system::error_code sizeError;
      boost::system::error_code timeError;

      const boost::uintmax_t size = boost::filesystem::file_size(path.c_str(), sizeError);
      const std::time_t written = boost::filesystem::last_write_time(path.c_str(), timeError);

      file_size_ = sizeError ? 0 : (unsigned __int64) size;
      file_written_ = timeError ? 0 : (__int64) written;

      const int ageDays = database_->BuildAgeDays(now);
      const int maxAge = Configuration::Instance()->GetGeoIpDatabaseMaxAgeDays();

      String message;
      message.Format(_T("Country blocking: read %s (%s), built %d day(s) ago, %I64u byte(s)."),
         path.c_str(), String(database_->GetMetadata().database_type).c_str(), ageDays, database_->GetByteCount());
      LOG_APPLICATION(message);

      if (maxAge > 0 && ageDays > maxAge)
      {
         String stale;
         stale.Format(_T("Country blocking: %s was built %d day(s) ago, which is more than the %d day(s) GeoIpDatabaseMaxAgeDays allows. It is still used - a stale answer beats no answer - but the addresses in it have moved since. Download a fresh one."),
            path.c_str(), ageDays, maxAge);
         LOG_APPLICATION(stale);
      }

      return database_;
   }

   AnsiString
   CountryBlocking::CountryOf(const IPAddress &address)
   {
      std::shared_ptr<const GeoIpDatabase> database = Database_();

      if (!database)
         return "";

      AnsiString iso;
      AnsiString name;

      database->Lookup(Unmapped(address), iso, name);

      return iso;
   }

   AnsiString
   CountryBlocking::CountryNameOf(const IPAddress &address)
   {
      std::shared_ptr<const GeoIpDatabase> database = Database_();

      if (!database)
         return "";

      AnsiString iso;
      AnsiString name;

      database->Lookup(Unmapped(address), iso, name);

      return name;
   }

   // ---------------------------------------------------------------- decisions

   const wchar_t *
   CountryBlocking::DefaultExemptRanges()
   {
      // The loopback addresses and every private network, in the order they are
      // most often met. The schema step seeds the settings row with exactly this
      // text; docs/CountryBlocking.md prints it.
      return _T("127.0.0.0/8,::1/128,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,169.254.0.0/16,100.64.0.0/10,fe80::/10,fc00::/7");
   }

   IPAddress
   CountryBlocking::Unmapped(const IPAddress &address)
   {
      try
      {
         const boost::asio::ip::address asio = address.GetAddress();

         if (asio.is_v6())
         {
            boost::asio::ip::address_v6 v6 = asio.to_v6();

            if (v6.is_v4_mapped())
            {
               boost::asio::ip::address_v6::bytes_type raw = v6.to_bytes();
               boost::asio::ip::address_v4::bytes_type four;

               four[0] = raw[12];
               four[1] = raw[13];
               four[2] = raw[14];
               four[3] = raw[15];

               return IPAddress(boost::asio::ip::address(boost::asio::ip::address_v4(four)));
            }
         }
      }
      catch (...)
      {
         // An address that cannot be examined is handed back as it came.
      }

      return address;
   }

   bool
   CountryBlocking::IsExemptBy(const String &exemptRanges, const IPAddress &address)
   {
      if (exemptRanges.IsEmpty())
         return false;

      return TrustedProxyList::Matches(exemptRanges, Unmapped(address));
   }

   bool
   CountryBlocking::IsExempt(const IPAddress &address)
   {
      return IsExemptBy(Configuration::Instance()->GetCountryBlockingExemptRanges(), address);
   }

   bool
   CountryBlocking::IsConnectionAllowed(Surface surface, const IPAddress &address)
   {
      const Policy policy = PolicyFor(surface);

      if (policy.mode == ModeOff)
         return true;

      if (IsExempt(address))
         return true;

      std::shared_ptr<const GeoIpDatabase> database = Database_();

      // No database means no policy. Said once when the file was read, not here:
      // a line per connection would be the denial of service the policy exists
      // to prevent.
      if (!database)
         return true;

      AnsiString iso;
      AnsiString name;
      database->Lookup(Unmapped(address), iso, name);

      if (Allows(policy, iso))
         return true;

      String surfaceName = SurfaceName(surface);
      surfaceName.MakeUpper();

      RecordRefusal_(surfaceName, address, iso, _T("connection"));
      return false;
   }

   bool
   CountryBlocking::IsLogonAllowed(const String &username, const IPAddress &address)
   {
      String domainName = username;
      const int at = domainName.Find(_T("@"));

      if (at >= 0)
         domainName = domainName.Mid(at + 1);
      else
         domainName = Configuration::Instance()->GetDefaultDomain();

      domainName.Trim();

      if (domainName.IsEmpty())
         return true;

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(domainName);

      if (!domain)
         return true;

      Policy policy;
      String error;

      if (!ParsePolicy(domain->GetCountryPolicy(), policy, error))
         return true;

      if (policy.mode == ModeOff)
         return true;

      if (IsExempt(address))
         return true;

      std::shared_ptr<const GeoIpDatabase> database = Database_();

      if (!database)
         return true;

      AnsiString iso;
      AnsiString name;
      database->Lookup(Unmapped(address), iso, name);

      if (Allows(policy, iso))
         return true;

      RecordRefusal_(_T("sign-in"), address, iso, domainName);
      return false;
   }

   void
   CountryBlocking::RecordRefusal_(const String &surfaceName, const IPAddress &address, const AnsiString &country, const String &detail)
   {
      const AnsiString code = country.IsEmpty() ? AnsiString("??") : country;

      ServerStatus::Instance()->OnCountryRefusal(String(code));

      // One line per refusal, in a fixed shape, in the application log: it is
      // the administrator's evidence, it is what a fail2ban filter would match,
      // and - deliberately - it says the country here and never to the client.
      String message;
      message.Format(_T("Country blocking: refused %s from %s (%s) for %s."),
         surfaceName.c_str(), String(address.ToString()).c_str(), String(code).c_str(), detail.c_str());

      LOG_APPLICATION(message);
   }

   // ------------------------------------------------------------------- status

   AnsiString
   CountryBlocking::StatusJson()
   {
      std::shared_ptr<const GeoIpDatabase> database = Database_();

      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      const String path = Configuration::Instance()->GetGeoIpDatabaseFile();
      const int maxAge = Configuration::Instance()->GetGeoIpDatabaseMaxAgeDays();
      const time_t now = ::time(nullptr);

      AnsiString json = "{";
      json += "\"configured\":";
      json += path.IsEmpty() ? "false" : "true";
      json += ",\"path\":\"" + JsonEscape(AnsiString(path)) + "\"";
      json += ",\"loaded\":";
      json += database ? "true" : "false";
      json += ",\"error\":\"" + JsonEscape(AnsiString(load_error_)) + "\"";

      if (database)
      {
         const int ageDays = database->BuildAgeDays(now);
         const bool stale = maxAge > 0 && ageDays > maxAge;

         AnsiString built;
         built.Format(",\"type\":\"%hs\",\"build_epoch\":%I64u,\"age_days\":%d,\"stale\":%hs,\"bytes\":%I64u,\"ip_version\":%u,\"record_size\":%u",
            JsonEscape(database->GetMetadata().database_type).c_str(),
            database->GetMetadata().build_epoch,
            ageDays,
            stale ? "true" : "false",
            database->GetByteCount(),
            database->GetMetadata().ip_version,
            database->GetMetadata().record_size);

         json += built;
      }

      AnsiString tail;
      tail.Format(",\"max_age_days\":%d,\"refusals_total\":%d,\"refusals_by_country\":{", maxAge,
         ServerStatus::Instance()->GetNumberOfCountryRefusals());
      json += tail;

      std::map<String, __int64> byCountry = ServerStatus::Instance()->GetCountryRefusals();
      bool first = true;

      for (std::map<String, __int64>::const_iterator it = byCountry.begin(); it != byCountry.end(); ++it)
      {
         if (!first)
            json += ",";

         AnsiString entry;
         entry.Format("\"%hs\":%I64d", JsonEscape(AnsiString(it->first)).c_str(), it->second);
         json += entry;
         first = false;
      }

      json += "}";

      // The policies themselves, so that one answer says both what is configured
      // and what the server can actually do about it.
      json += ",\"policies\":{";
      json += "\"smtp\":\"" + JsonEscape(AnsiString(FormatPolicy(PolicyFor(SurfaceSmtp)))) + "\"";
      json += ",\"imap\":\"" + JsonEscape(AnsiString(FormatPolicy(PolicyFor(SurfaceImap)))) + "\"";
      json += ",\"pop3\":\"" + JsonEscape(AnsiString(FormatPolicy(PolicyFor(SurfacePop3)))) + "\"";
      json += ",\"http\":\"" + JsonEscape(AnsiString(FormatPolicy(PolicyFor(SurfaceHttp)))) + "\"";
      json += "}}";

      return json;
   }

   String
   CountryBlocking::StatusText()
   {
      const String path = Configuration::Instance()->GetGeoIpDatabaseFile();

      if (path.IsEmpty())
         return _T("No geographic database is configured, so nothing is judged by country.");

      std::shared_ptr<const GeoIpDatabase> database = Database_();

      if (!database)
      {
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);

         return Formatter::Format(_T("{0} could not be read, so nothing is judged by country. {1}"), path, load_error_);
      }

      const int ageDays = database->BuildAgeDays(::time(nullptr));
      const int maxAge = Configuration::Instance()->GetGeoIpDatabaseMaxAgeDays();

      String text;
      text.Format(_T("%s, built %d day(s) ago%s."),
         String(database->GetMetadata().database_type).c_str(),
         ageDays,
         (maxAge > 0 && ageDays > maxAge) ? _T(" - which is stale") : _T(""));

      return text;
   }

   // ---------------------------------------------------------------- the tests

   namespace
   {
      void Expect(bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(what);
      }

      CountryBlocking::Policy Parsed(const wchar_t *text)
      {
         CountryBlocking::Policy policy;
         String error;

         if (!CountryBlocking::ParsePolicy(text, policy, error))
            throw std::logic_error(AnsiString(String(_T("A policy that should have parsed did not: ")) + String(text)).c_str());

         return policy;
      }

      void ExpectRefused(const wchar_t *text)
      {
         CountryBlocking::Policy policy;
         String error;

         if (CountryBlocking::ParsePolicy(text, policy, error))
            throw std::logic_error(AnsiString(String(_T("A policy that should have been refused parsed: ")) + String(text)).c_str());

         if (error.IsEmpty())
            throw std::logic_error(AnsiString(String(_T("A policy was refused without a reason: ")) + String(text)).c_str());
      }
   }

   void
   CountryBlockingTester::Test()
   {
      // The three shapes, and the spellings that mean the same thing.
      Expect(Parsed(_T("")).mode == CountryBlocking::ModeOff, "An empty policy did not read as off.");
      Expect(Parsed(_T("off")).mode == CountryBlocking::ModeOff, "off did not read as off.");
      Expect(Parsed(_T("  OFF  ")).mode == CountryBlocking::ModeOff, "OFF with spaces around it did not read as off.");

      CountryBlocking::Policy allow = Parsed(_T("allow:gb, ie ,FR"));
      Expect(allow.mode == CountryBlocking::ModeAllow, "allow: did not read as an allow list.");
      Expect(allow.countries.size() == 3, "An allow list of three countries did not read three.");
      Expect(allow.countries.find("GB") != allow.countries.end(), "A lower-case country code was not upper-cased.");

      CountryBlocking::Policy block = Parsed(_T("block:CN,RU"));
      Expect(block.mode == CountryBlocking::ModeBlock, "block: did not read as a block list.");

      // The canonical form: one policy has one stored spelling.
      String canonical;
      String error;

      Expect(CountryBlocking::Canonicalise(_T("ALLOW: fr,gb , ie"), canonical, error), "A policy that should have parsed did not canonicalise.");
      Expect(canonical == _T("allow:FR,GB,IE"), "A policy was not stored in its canonical form.");

      Expect(CountryBlocking::Canonicalise(_T("off"), canonical, error), "off did not canonicalise.");
      Expect(canonical.IsEmpty(), "off was not stored as an empty policy.");

      // Every refusal, each with a sentence.
      ExpectRefused(_T("deny:CN"));
      ExpectRefused(_T("allow"));
      ExpectRefused(_T("block"));
      ExpectRefused(_T("allow:"));
      ExpectRefused(_T("block:GBR"));
      ExpectRefused(_T("block:G1"));
      ExpectRefused(_T("off:GB"));

      String tooLong = _T("block:");
      for (int i = 0; i < 100; i++)
         tooLong += _T("CN,");

      ExpectRefused(tooLong.c_str());

      // What a policy MEANS, which is the half that matters. The empty country
      // is what the database answers for an address it does not know.
      CountryBlocking::Policy off;

      Expect(CountryBlocking::Allows(off, "CN"), "A policy that is off refused a country.");
      Expect(CountryBlocking::Allows(off, ""), "A policy that is off refused an unknown country.");

      Expect(CountryBlocking::Allows(allow, "GB"), "An allow list refused a country it names.");
      Expect(!CountryBlocking::Allows(allow, "CN"), "An allow list admitted a country it does not name.");
      Expect(!CountryBlocking::Allows(allow, ""), "An allow list admitted an address whose country is not known.");

      Expect(!CountryBlocking::Allows(block, "CN"), "A block list admitted a country it names.");
      Expect(CountryBlocking::Allows(block, "GB"), "A block list refused a country it does not name.");
      Expect(CountryBlocking::Allows(block, ""), "A block list refused an address whose country is not known.");

      // The surface names, which are what COM, REST and the documents all use.
      CountryBlocking::Surface surface = CountryBlocking::SurfaceHttp;

      Expect(CountryBlocking::SurfaceFromName(_T("SMTP"), surface) && surface == CountryBlocking::SurfaceSmtp, "SMTP was not recognised as a surface.");
      Expect(CountryBlocking::SurfaceFromName(_T("imap"), surface) && surface == CountryBlocking::SurfaceImap, "imap was not recognised as a surface.");
      Expect(CountryBlocking::SurfaceFromName(_T("pop3"), surface) && surface == CountryBlocking::SurfacePop3, "pop3 was not recognised as a surface.");
      Expect(CountryBlocking::SurfaceFromName(_T("http"), surface) && surface == CountryBlocking::SurfaceHttp, "http was not recognised as a surface.");
      Expect(!CountryBlocking::SurfaceFromName(_T("managesieve"), surface), "A listener with no policy of its own was accepted as a surface.");

      Expect(CountryBlocking::SurfaceName(CountryBlocking::SurfaceSmtp) == _T("smtp"), "The SMTP surface is not named smtp.");

      // The exemptions, driven against the shipped list rather than against
      // whatever this bench's setting happens to hold - a test that reads the
      // configuration is a test of the configuration.
      const String shipped = CountryBlocking::DefaultExemptRanges();

      IPAddress loopback;
      Expect(loopback.TryParse("127.0.0.1", false), "127.0.0.1 could not be parsed.");
      Expect(CountryBlocking::IsExemptBy(shipped, loopback), "The loopback address is not in the list this server ships.");

      IPAddress office;
      Expect(office.TryParse("192.168.1.20", false), "192.168.1.20 could not be parsed.");
      Expect(CountryBlocking::IsExemptBy(shipped, office), "A private address is not in the list this server ships.");

      IPAddress carrier;
      Expect(carrier.TryParse("100.70.1.1", false), "100.70.1.1 could not be parsed.");
      Expect(CountryBlocking::IsExemptBy(shipped, carrier), "A carrier-grade NAT address is not in the list this server ships.");

      IPAddress unique;
      Expect(unique.TryParse("fd00::1", false), "fd00::1 could not be parsed.");
      Expect(CountryBlocking::IsExemptBy(shipped, unique), "A unique local IPv6 address is not in the list this server ships.");

      IPAddress six;
      Expect(six.TryParse("::1", false), "::1 could not be parsed.");
      Expect(CountryBlocking::IsExemptBy(shipped, six), "The IPv6 loopback address is not in the list this server ships.");

      IPAddress outside;
      Expect(outside.TryParse("198.51.100.7", false), "198.51.100.7 could not be parsed.");
      Expect(!CountryBlocking::IsExemptBy(shipped, outside), "A public address was treated as exempt.");

      // An empty list exempts nothing, which is what lets an administrator - and
      // a test - have their own network judged like anybody else's.
      Expect(!CountryBlocking::IsExemptBy(_T(""), loopback), "An empty exempt list still exempted the loopback address.");

      // An IPv4 client of a dual-stack listener, as the socket hands it over.
      IPAddress mapped;
      Expect(mapped.TryParse("::ffff:127.0.0.1", false), "::ffff:127.0.0.1 could not be parsed.");
      Expect(CountryBlocking::IsExemptBy(shipped, mapped), "A v4-mapped loopback address was not recognised as the IPv4 address it is.");
   }
}
