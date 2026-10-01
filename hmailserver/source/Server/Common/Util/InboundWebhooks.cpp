// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See InboundWebhooks.h.

#include "StdAfx.h"

#include "InboundWebhooks.h"

#include "AlertManager.h"
#include "Crypt.h"
#include "HttpsClient.h"
#include "ListManager.h"
#include "MailerDaemonAddressDeterminer.h"
#include "MessageUtilities.h"
#include "Unicode.h"
#include "Encoding/Base64.h"
#include "Hashing/HashCreator.h"

#include "../AntiSpam/AuthenticationResults.h"
#include "../BO/Domain.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"
#include "../BO/MessageRecipient.h"
#include "../BO/MessageRecipients.h"
#include "../BO/RouteAddresses.h"
#include "../BO/Routes.h"
#include "../Cache/CacheContainer.h"
#include "../Mime/Mime.h"
#include "../Persistence/MessageStore.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/PersistentMessageRecipient.h"
#include "../Threading/WorkQueue.h"

#include "../../SMTP/PlusAddressing.h"
#include "../../SMTP/RecipientParser.h"
#include "../../SMTP/RuleApplier.h"
#include "../../SMTP/SMTPConfiguration.h"
#include "../../SMTP/SMTPDeliverer.h"

#include <algorithm>
#include <ctime>
#include <set>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const wchar_t *InboundWebhooks::EventName = L"message.inbound";

   namespace
   {
      // The routes are re-read at most this often, so that a change made in
      // another process is picked up; a change made HERE invalidates at once.
      const __int64 CacheSeconds = 30;

      const int MaxErrorLength = 250;

      // Longer than an outbound webhook's ten seconds: this body is the message,
      // attachments and all, and a receiver that has to take a few megabytes has
      // not stopped answering because it took more than ten seconds to.
      const int RequestTimeoutSeconds = 30;

      // How much of an answer is read. Only its status is judged, so an answer
      // longer than this - a thank-you page, an endpoint that echoes what it was
      // sent - is read this far and judged by its status line like any other
      // (HttpsClient's keep_head_of_long_answer), never taken for no answer.
      const size_t MaxResponseBytes = 16 * 1024;

      // One pass reads at most this many due rows and runs for at most this
      // long, for the reason WebhookManager's pass is bounded. Fewer rows than
      // the outbound pass, since each is a message rather than a line of JSON.
      const int MaxPerPass = 50;
      const __int64 MaxPassSeconds = 60;

      // How many times a route that delivers as well may lead, through what it
      // expands to, to another route in one message. Every route takes a
      // message once, so this is only ever reached by as many routes chained
      // one behind another, and stops there.
      const int MaxRounds = 8;

      // What a sender asked for with DSN NOTIFY (RFC 3461): a failure report, or
      // none.
      bool WantsFailureReport(const std::shared_ptr<MessageRecipient> &recipient)
      {
         const int notify = recipient->GetDSNNotify();
         return notify == MessageRecipient::DSNNotifyDefault || (notify & MessageRecipient::DSNNotifyFailure) != 0;
      }

      String OneLine(const String &value)
      {
         String result = value;

         for (int i = 0; i < result.GetLength(); i++)
         {
            if (result[i] == '\r' || result[i] == '\n' || result[i] == '\t')
               result[i] = ' ';
         }

         return result;
      }

      String Shorten(const String &value, int maximum)
      {
         if (value.GetLength() <= maximum)
            return value;

         if (maximum <= 3)
            return value.Mid(0, maximum);

         return value.Mid(0, maximum - 3) + _T("...");
      }

      AnsiString Utf8(const String &value)
      {
         AnsiString result;
         Unicode::WideToMultiByte(value, result);
         return result;
      }

      String TimeText(__int64 seconds)
      {
         time_t value = (time_t) seconds;

         tm utc = {};
         gmtime_s(&utc, &value);

         String text;
         text.Format(_T("%04d-%02d-%02d %02d:%02d:%02dZ"), utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
            utc.tm_hour, utc.tm_min, utc.tm_sec);

         return text;
      }

      // Bytes that are UTF-8 left as they are; a byte that begins no valid
      // sequence becomes '?'. What a JSON string must be, and what a header
      // block written in some other character set is not.
      AnsiString ValidUtf8(const AnsiString &value)
      {
         const std::string bytes(value.c_str(), (size_t) value.GetLength());
         std::string result;
         result.reserve(bytes.size());

         size_t i = 0;
         while (i < bytes.size())
         {
            const unsigned char c = (unsigned char) bytes[i];

            size_t length = 0;
            if (c < 0x80)
               length = 1;
            else if (c >= 0xC2 && c <= 0xDF)
               length = 2;
            else if (c >= 0xE0 && c <= 0xEF)
               length = 3;
            else if (c >= 0xF0 && c <= 0xF4)
               length = 4;

            bool valid = length > 0 && i + length <= bytes.size();
            for (size_t k = 1; valid && k < length; k++)
               valid = ((unsigned char) bytes[i + k] & 0xC0) == 0x80;

            if (!valid)
            {
               result += '?';
               i++;
               continue;
            }

            result.append(bytes, i, length);
            i += length;
         }

         AnsiString answer;
         answer.assign(result.data(), result.size());
         return answer;
      }

      // A JSON string's contents from UTF-8 bytes: the quote, the backslash and
      // every control character escaped, everything else as it is.
      AnsiString JsonEscape(const AnsiString &value)
      {
         static const char *hex = "0123456789abcdef";

         const std::string bytes(value.c_str(), (size_t) value.GetLength());
         std::string result;
         result.reserve(bytes.size() + 16);

         for (size_t i = 0; i < bytes.size(); i++)
         {
            const unsigned char c = (unsigned char) bytes[i];

            switch (c)
            {
            case '\"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\r': result += "\\r"; break;
            case '\n': result += "\\n"; break;
            case '\t': result += "\\t"; break;
            default:
               if (c < 0x20)
               {
                  result += "\\u00";
                  result += hex[c >> 4];
                  result += hex[c & 0x0F];
               }
               else
               {
                  result += (char) c;
               }
               break;
            }
         }

         AnsiString answer;
         answer.assign(result.data(), result.size());
         return answer;
      }

      AnsiString JsonString(const AnsiString &utf8)
      {
         AnsiString result = "\"";
         result += JsonEscape(ValidUtf8(utf8));
         result += "\"";
         return result;
      }

      // A value for a Content-Disposition parameter: no quote, no line break,
      // and no backslash - a receiver that reads backslash escapes in a
      // quoted-string (RFC 7578 4.2 leaves it open; busboy and Python's do)
      // would read a name ending in one as a string never closed.
      AnsiString DispositionValue(const AnsiString &value)
      {
         AnsiString result = value;
         result.Replace("\\", "%5C");
         result.Replace("\"", "%22");
         result.Replace("\r", " ");
         result.Replace("\n", " ");
         return result;
      }

      // The header block of the message: everything before the first empty line.
      AnsiString HeaderBlock(const AnsiString &raw)
      {
         const int end = raw.Find("\r\n\r\n");
         if (end < 0)
            return raw;

         return raw.Mid(0, end + 2);
      }

      // The Authentication-Results field this server wrote, and only that one,
      // read the way the webmail reads it (RestApiServer::OwnAuthenticationResults_):
      // the authserv-id is the first token before ';', unquoted, a version
      // dropped, and only while the server writes one at all - with
      // AuthenticationResultsEnabled off a sender's own field is not stripped
      // on receipt, and its "spf=pass" would be carried as the server's verdict.
      AnsiString OwnAuthenticationResults(std::shared_ptr<MimeBody> root)
      {
         if (!root || !IniFileSettings::Instance()->GetAuthenticationResultsEnabled())
            return "";

         AnsiString ours = AuthenticationResults::GetAuthservId();
         ours.MakeLower();
         if (ours.IsEmpty())
            return "";

         const std::vector<MimeField> &fields = root->Fields();
         for (size_t i = 0; i < fields.size(); i++)
         {
            const char *name = fields[i].GetName();
            if (name == nullptr || AnsiString(name).CompareNoCase("Authentication-Results") != 0)
               continue;

            const char *rawValue = fields[i].GetValue();
            AnsiString value = rawValue != nullptr ? AnsiString(rawValue) : AnsiString();
            value.Replace("\r", "");
            value.Replace("\n", " ");

            AnsiString id = value;
            const int semicolon = id.Find(";");
            if (semicolon >= 0)
               id = id.Mid(0, semicolon);
            id.Trim();
            if (id.GetLength() >= 2 && id[0] == '"')
            {
               const int closing = id.Find("\"", 1);
               if (closing > 0)
                  id = id.Mid(1, closing - 1);
            }
            const int space = id.Find(" ");
            if (space > 0)
               id = id.Mid(0, space);
            id.MakeLower();

            if (id == ours)
               return value;
         }

         return "";
      }

      // One method's result out of that field: "pass", "fail", "none" ... or
      // empty when the method is not there. A message with several DKIM
      // signatures is "pass" when any of them passed, else the first result.
      AnsiString MethodResult(const AnsiString &results, const char *method)
      {
         if (results.IsEmpty())
            return "";

         AnsiString lower = results;
         lower.MakeLower();

         const AnsiString prefix = AnsiString(method) + "=";

         std::vector<AnsiString> clauses = StringParser::SplitString(lower, AnsiString(";"));

         AnsiString first;
         bool anyPass = false;

         // The first clause is the authserv-id.
         for (size_t i = 1; i < clauses.size(); i++)
         {
            AnsiString clause = clauses[i];
            clause.Trim();

            if (clause.Find(prefix) != 0)
               continue;

            AnsiString value = clause.Mid(prefix.GetLength());
            int end = 0;
            while (end < value.GetLength() && value[end] >= 'a' && value[end] <= 'z')
               end++;
            value = value.Mid(0, end);

            if (value.IsEmpty())
               continue;

            if (first.IsEmpty())
               first = value;

            if (value == "pass")
               anyPass = true;
         }

         return anyPass ? AnsiString("pass") : first;
      }

      // One field of a multipart/form-data body, before the boundary is known.
      struct FormPart
      {
         AnsiString name;
         AnsiString filename;     // empty for a plain field
         AnsiString content_type; // empty for a plain field
         AnsiString value;
      };

      void AddField(std::vector<FormPart> &parts, const char *name, const AnsiString &value)
      {
         FormPart part;
         part.name = name;
         part.value = value;
         parts.push_back(part);
      }

      struct CarriedAttachment
      {
         CarriedAttachment() : size(0) {}

         AnsiString filename;
         AnsiString type;
         AnsiString content_id;
         AnsiString bytes;
         __int64 size;
      };
   }

   InboundWebhooks::InboundWebhooks() :
      cached_at_(0),
      cache_valid_(false),
      draining_(false),
      passing_(false),
      drain_wanted_(false)
   {

   }

   InboundWebhooks::~InboundWebhooks()
   {

   }

   // ---- Addresses ---------------------------------------------------------------

   String
   InboundWebhooks::NormaliseAddress(const String &address)
   {
      String value = address;
      value.Trim();
      value.MakeLower();

      if (value.IsEmpty() || value.GetLength() > 255)
         return _T("");

      if (value.Find(_T("@")) >= 0)
         return StringParser::IsValidEmailAddress(value) ? value : String();

      return StringParser::IsValidDomainName(value) ? value : String();
   }

   String
   InboundWebhooks::DomainOf(const String &routeAddress)
   {
      if (routeAddress.Find(_T("@")) >= 0)
      {
         String domain = StringParser::ExtractDomain(routeAddress);
         domain.MakeLower();
         return domain;
      }

      String domain = routeAddress;
      domain.MakeLower();
      return domain;
   }

   String
   InboundWebhooks::FileOf_(const String &partialName)
   {
      // A copy is a working file in the queue directory, named as a queued
      // message's file is, so the store reads it as the bytes it holds and never
      // as a container (MessageStore::MayHoldContainer).
      std::shared_ptr<Message> named = std::shared_ptr<Message>(new Message(false));
      named->SetPartialFileName(partialName);
      return MessageStore::WorkingFile(named);
   }

   // ---- The routes ----------------------------------------------------------------

   InboundWebhooks::Route
   InboundWebhooks::ReadRoute_(const std::shared_ptr<DALRecordset> &recordset)
   {
      Route route;

      route.id = recordset->GetInt64Value("ihid");
      route.address = recordset->GetStringValue("ihaddress");
      route.url = recordset->GetStringValue("ihurl");

      // Protected at rest as an outbound webhook's secret is, so that a copy of
      // the database does not hand out what every body is signed with.
      route.secret = Crypt::Instance()->UnprotectSecret(recordset->GetStringValue("ihsecret"));
      route.deliver = recordset->GetLongValue("ihdeliver") != 0;
      route.on_failure = recordset->GetLongValue("ihonfailure") == FailureBounce ? FailureBounce : FailureKeep;
      route.format = recordset->GetLongValue("ihformat") == FormatJson ? FormatJson : FormatForm;

      const long attachments = recordset->GetLongValue("ihattachments");
      route.attachments = attachments == AttachmentsNames ? AttachmentsNames :
                          attachments == AttachmentsNone ? AttachmentsNone : AttachmentsInclude;

      route.include_raw = recordset->GetLongValue("ihincluderaw") != 0;
      route.max_size_kb = recordset->GetLongValue("ihmaxsizekb");
      route.private_network = recordset->GetLongValue("ihprivatenetwork") != 0;
      route.enabled = recordset->GetLongValue("ihenabled") != 0;
      route.created = recordset->GetInt64Value("ihcreated");

      return route;
   }

   InboundWebhooks::Delivery
   InboundWebhooks::ReadDelivery_(const std::shared_ptr<DALRecordset> &recordset)
   {
      Delivery delivery;

      delivery.id = recordset->GetInt64Value("iqid");
      delivery.route_id = recordset->GetInt64Value("iqhookid");
      delivery.created = recordset->GetInt64Value("iqcreated");
      delivery.state = recordset->GetLongValue("iqstate");
      delivery.tries = recordset->GetLongValue("iqtries");
      delivery.next = recordset->GetInt64Value("iqnext");
      delivery.error = recordset->GetStringValue("iqerror");
      delivery.file = recordset->GetStringValue("iqfile");
      delivery.queue_id = recordset->GetInt64Value("iqqueueid");
      delivery.sender = recordset->GetStringValue("iqsender");
      delivery.recipients = recordset->GetStringValue("iqrecipients");
      delivery.quiet = recordset->GetStringValue("iqquiet");
      delivery.sender_ip = recordset->GetStringValue("iqsenderip");
      delivery.spam = recordset->GetLongValue("iqspam") != 0;
      delivery.size = recordset->GetInt64Value("iqsize");

      return delivery;
   }

   bool
   InboundWebhooks::ReadRoutes_(std::vector<Route> &routes)
   {
      routes.clear();

      SQLCommand command(_T("select * from hm_inboundhooks order by ihaddress asc"));

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         routes.push_back(ReadRoute_(recordset));
         recordset->MoveNext();
      }

      return true;
   }

   /*
      The routes as the mail path sees them. The common answer is an empty map,
      and the database is asked at most twice a minute however much mail is
      flowing. Called with mutex_ held.
   */
   bool
   InboundWebhooks::EnsureCache_()
   {
      const __int64 now = (__int64) time(nullptr);

      if (cache_valid_ && now >= cached_at_ && now - cached_at_ < CacheSeconds)
         return true;

      std::vector<Route> fresh;
      if (!ReadRoutes_(fresh))
      {
         // The database is not answering: whatever was last read stands, and a
         // first read that fails leaves the cache invalid for the next ask.
         return cache_valid_;
      }

      cached_.clear();
      for (const Route &route : fresh)
         cached_[route.address] = route;

      cached_at_ = now;
      cache_valid_ = true;
      return true;
   }

   void
   InboundWebhooks::Invalidate()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      cache_valid_ = false;
      cached_.clear();
      cached_at_ = 0;
   }

   void
   InboundWebhooks::Forget()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      cache_valid_ = false;
      cached_.clear();
      cached_at_ = 0;

      // A drain posted to the queue the restart took away never starts, and a
      // flag left set would keep every later message from posting one. The
      // pass flags stay: a pass that is running clears its own.
      draining_ = false;
   }

   bool
   InboundWebhooks::FindRoute(const String &address, Route &route)
   {
      return FindRoute_(address, route, true);
   }

   bool
   InboundWebhooks::FindRoute_(const String &address, Route &route, bool enabledOnly)
   {
      String wanted = address;
      wanted.Trim();
      wanted.MakeLower();

      const String domainName = StringParser::ExtractDomain(wanted);
      if (domainName.IsEmpty())
         return false;

      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      if (!EnsureCache_() || cached_.empty())
         return false;

      // The address as written; then the address before its plus tag, as the
      // domain's own plus addressing reads it; then the domain.
      std::vector<String> candidates;
      candidates.push_back(wanted);

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(domainName);
      if (domain)
      {
         String withoutTag = PlusAddressing::ExtractAccountAddress(wanted, domain);
         withoutTag.MakeLower();

         if (withoutTag != wanted)
            candidates.push_back(withoutTag);
      }

      candidates.push_back(domainName);

      // The first route that is on, most specific first - so that an address
      // route switched off leaves its address to the domain's route, as RCPT TO
      // does. Asked for the routes that are off as well, one of them answers
      // only when no route that is on takes the address at all.
      const Route *switchedOff = nullptr;

      for (const String &candidate : candidates)
      {
         std::map<String, Route>::const_iterator found = cached_.find(candidate);
         if (found == cached_.end())
            continue;

         if (found->second.enabled)
         {
            route = found->second;
            return true;
         }

         if (switchedOff == nullptr)
            switchedOff = &found->second;
      }

      if (!enabledOnly && switchedOff != nullptr)
      {
         route = *switchedOff;
         return true;
      }

      return false;
   }

   bool
   InboundWebhooks::GetRoutes(std::vector<Route> &routes)
   {
      return ReadRoutes_(routes);
   }

   bool
   InboundWebhooks::GetRoute(const String &address, Route &route)
   {
      // Stored lower case, and compared so here rather than by the database,
      // which on PostgreSQL compares = case-sensitively.
      const String wanted = NormaliseAddress(address);
      if (wanted.IsEmpty())
         return false;

      SQLCommand command(_T("select * from hm_inboundhooks where ihaddress = @ADDRESS"));
      command.AddParameter("@ADDRESS", wanted);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      route = ReadRoute_(recordset);
      return true;
   }

   bool
   InboundWebhooks::SaveRoute(const Route &route, bool creating, bool mayReachPrivateNetwork, String &refusal)
   {
      refusal = _T("");

      const String address = NormaliseAddress(route.address);
      if (address.IsEmpty())
      {
         refusal = _T("an inbound webhook routes an address (user@example.com) or a whole domain (example.com)");
         return false;
      }

      // Only mail this server takes can reach a route, and it takes mail for
      // the domains it hosts: a route for any other would never be used, and is
      // better refused now than wondered about later.
      const String domainName = DomainOf(address);
      if (!CacheContainer::Instance()->GetDomain(domainName))
      {
         refusal = _T("an inbound webhook routes an address of a domain this server hosts, and it does not host ") + domainName;
         return false;
      }

      String url = route.url;
      url.Trim();

      if (url.IsEmpty())
      {
         refusal = _T("an inbound webhook needs the URL its mail is posted to");
         return false;
      }

      if (url.GetLength() > 1024)
      {
         refusal = _T("a URL is at most 1024 characters");
         return false;
      }

      // The path goes into the request line as it is written: a space, a line
      // break or another control character there would end that line and start
      // a header - or a request - of the URL's own making, and a character
      // outside ASCII has no one spelling there: the sender percent-encodes it,
      // and writes a domain name in its xn-- form. A String's character is a
      // wchar_t on both platforms, so no character is negative here; until
      // 26 September 2026 this refused control characters only, and let
      // everything from 0x80 up through, on Windows and Linux alike.
      for (int i = 0; i < url.GetLength(); i++)
      {
         const unsigned int c = (unsigned int) url[i];
         if (c < 0x21 || c >= 0x7F)
         {
            refusal = _T("a URL is printable ASCII, with no space, line break or other control character in it; percent-encode the rest");
            return false;
         }
      }

      {
         bool https = false;
         AnsiString host, port, path;

         if (!HttpsClient::ParseUrl(Utf8(url), https, host, port, path))
         {
            refusal = _T("the URL is an http or https URL with a host");
            return false;
         }

         // Plain http only to this machine, as an outbound webhook's: the body
         // is somebody's mail.
         if (!https && !HttpsClient::IsLoopbackHost(host))
         {
            refusal = _T("a URL reached over plain http must be on this machine; anywhere else needs https, since the body is the message itself");
            return false;
         }

         if (mayReachPrivateNetwork)
         {
            // The server's administrator may name this server's own network,
            // as for an outbound webhook - but never a link-local or cloud
            // metadata address. Judged now, for a refusal the administrator
            // sees, and again at every connection, which is the check that
            // holds: a name that does not resolve now is not refused here.
            bool resolved = false;
            String reason;
            if (!HttpsClient::HostIsNotLinkLocal(host, resolved, reason))
            {
               refusal = reason;
               return false;
            }
         }
         else
         {
            // Any other credential - an API key, a role that is not the
            // server's administrator - names a public address and nothing else,
            // so that it cannot point this server at the inside of its network.
            // The refusal names the host and not what it resolved to: which
            // private address a name leads to is this network's business, and
            // a key could otherwise map the inside of it one name at a time.
            String reason;
            if (!HttpsClient::HostIsPublic(host, reason))
            {
               refusal = _T("this credential may route mail only to a public address, and ") + String(host) +
                         _T(" does not resolve, or does not lead to public addresses only. Only the server's administrator may route mail to this server's own network.");
               return false;
            }
         }
      }

      // A signature nobody can check is not a signature.
      if (route.secret.IsEmpty())
      {
         refusal = _T("an inbound webhook needs a shared secret, so the requests it makes can be verified");
         return false;
      }

      if (route.secret.GetLength() > 255)
      {
         refusal = _T("a shared secret is at most 255 characters");
         return false;
      }

      const String protectedSecret = Crypt::Instance()->ProtectSecret(route.secret);
      if (protectedSecret.IsEmpty())
      {
         refusal = _T("the shared secret could not be protected for storage; see the error log");
         return false;
      }

      // No more than the 80 MB MessageData parses: a larger message could not
      // be made into a body, and would be kept or returned only after every
      // attempt had failed to read it.
      if (route.max_size_kb < 0 || route.max_size_kb > MaxSizeKb)
      {
         refusal = _T("a size ceiling is between 0 (none of the route's own: 81920 KB, the most any route posts) and 81920 KB");
         return false;
      }

      Route existing;
      const bool exists = GetRoute(address, existing);

      if (creating && exists)
      {
         refusal = _T("an inbound webhook for ") + address + _T(" exists already");
         return false;
      }

      if (!creating && !exists)
      {
         refusal = _T("there is no inbound webhook for ") + address;
         return false;
      }

      SQLStatement statement(exists ? SQLStatement::STUpdate : SQLStatement::STInsert, _T("hm_inboundhooks"));

      if (!exists)
      {
         statement.AddColumn(_T("ihaddress"), address, 255);
         statement.AddColumnInt64(_T("ihcreated"), (__int64) time(nullptr));
      }

      statement.AddColumn(_T("ihurl"), url, 1024);
      statement.AddColumn(_T("ihsecret"), protectedSecret, 1024);
      statement.AddColumn(_T("ihdeliver"), (long) (route.deliver ? 1 : 0));
      statement.AddColumn(_T("ihonfailure"), (long) (route.on_failure == FailureBounce ? FailureBounce : FailureKeep));
      statement.AddColumn(_T("ihformat"), (long) (route.format == FormatJson ? FormatJson : FormatForm));
      statement.AddColumn(_T("ihattachments"), (long) route.attachments);
      statement.AddColumn(_T("ihincluderaw"), (long) (route.include_raw ? 1 : 0));
      statement.AddColumn(_T("ihmaxsizekb"), (long) route.max_size_kb);

      // Recorded by who saved it last: a route a key changes is a route a key
      // made, and reaches public addresses only from then on.
      statement.AddColumn(_T("ihprivatenetwork"), (long) (mayReachPrivateNetwork ? 1 : 0));
      statement.AddColumn(_T("ihenabled"), (long) (route.enabled ? 1 : 0));

      if (exists)
         statement.AddWhereClauseColumn(_T("ihaddress"), address);
      else
         statement.SetIdentityColumn(_T("ihid"));

      String errorMessage;
      if (!Application::Instance()->GetDBManager()->Execute(statement, nullptr, 0, errorMessage))
      {
         refusal = _T("the inbound webhook could not be saved: ") + errorMessage;
         return false;
      }

      Invalidate();
      return true;
   }

   bool
   InboundWebhooks::DeleteRoute(const String &address)
   {
      Route existing;
      if (!GetRoute(address, existing))
         return false;

      return DeleteRouteById_(existing);
   }

   bool
   InboundWebhooks::DeleteRouteById_(const Route &existing)
   {
      // The copies its queue kept are named first: the rows go with the route
      // through the foreign key, and a copy with no row would be a message
      // nobody can reach.
      std::vector<String> files;
      {
         SQLCommand command(_T("select iqfile from hm_inboundhookqueue where iqhookid = @HOOKID"));
         command.AddParameter("@HOOKID", existing.id);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return false;

         while (!recordset->IsEOF())
         {
            const String file = recordset->GetStringValue("iqfile");
            if (!file.IsEmpty())
               files.push_back(file);

            recordset->MoveNext();
         }
      }

      // By its address, which is what the audit trail names a deletion by (the
      // column its WHERE is written with): the row was made under its address,
      // and a removal recorded under a number that appears nowhere else could
      // not be told apart from any other.
      SQLCommand command(_T("delete from hm_inboundhooks where ihaddress = @ADDRESS"));
      command.AddParameter("@ADDRESS", existing.address);

      if (!Application::Instance()->GetDBManager()->Execute(command))
         return false;

      for (const String &file : files)
         MessageStore::DiscardWorkingFile(FileOf_(file));

      Invalidate();
      return true;
   }

   bool
   InboundWebhooks::DeleteRoutesOfDomain(const String &domainName)
   {
      String domain = domainName;
      domain.MakeLower();

      if (domain.IsEmpty())
         return true;

      std::vector<Route> routes;
      if (!ReadRoutes_(routes))
         return false;

      bool allRemoved = true;

      for (const Route &route : routes)
      {
         if (DomainOf(route.address) != domain)
            continue;

         if (!DeleteRouteById_(route))
            allRemoved = false;
      }

      return allRemoved;
   }

   // ---- Mail ----------------------------------------------------------------------

   void
   InboundWebhooks::TakeOff_(std::shared_ptr<Message> message, std::shared_ptr<MessageRecipient> recipient)
   {
      // From the database and from the message, so that neither local nor
      // external delivery ever sees the route's recipient.
      if (!PersistentMessageRecipient::DeleteObject(recipient))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7406, "InboundWebhooks::TakeRoutedRecipients",
            Formatter::Format("Message {0} was queued for the inbound webhook of {1}, but that recipient could not be removed from the delivery queue. If the message is tried again, it is queued for the webhook again.",
               message->GetID(), recipient->GetAddress()));
      }

      // owned by this call: the recipients of the message this delivery holds, erased through the reference
      std::vector<std::shared_ptr<MessageRecipient> > &live = message->GetRecipients()->GetVector();
      for (auto it = live.begin(); it != live.end(); it++)
      {
         if (*it == recipient)
         {
            live.erase(it);
            break;
         }
      }
   }

   bool
   InboundWebhooks::IsCandidate_(const std::shared_ptr<MessageRecipient> &recipient)
   {
      if (!recipient || recipient->GetLocalAccountID() != 0)
         return false;

      const String domainName = StringParser::ExtractDomain(recipient->GetAddress());
      return !domainName.IsEmpty() && CacheContainer::Instance()->GetDomain(domainName) != nullptr;
   }

   bool
   InboundWebhooks::IsSomebodyElses_(const String &address)
   {
      // The list manager's: a managed list's own address, or one of the
      // addresses it answers (ListManager.h).
      ListManager::Command command = ListManager::CommandNone;
      std::shared_ptr<const DistributionList> list;
      String verpMember;
      if (ListManager::ParseAddress(address, command, list, verpMember))
         return true;

      // An SMTP route's: a domain this server hosts may be routed on as well
      // (a split domain), and the recipient such a route takes has no account
      // behind it either. Asked as RecipientParser asks it.
      std::shared_ptr<SMTPConfiguration> smtp = Configuration::Instance()->GetSMTPConfiguration();
      if (!smtp || !smtp->GetRoutes())
         return false;

      std::shared_ptr<HM::Route> smtpRoute = smtp->GetRoutes()->GetItemByNameWithWildcardMatch(StringParser::ExtractDomain(address));
      if (!smtpRoute)
         return false;

      return smtpRoute->ToAllAddresses() || smtpRoute->GetAddresses()->GetItemByName(address) != nullptr;
   }

   int
   InboundWebhooks::CeilingKb_(const Route &route)
   {
      if (route.max_size_kb <= 0 || route.max_size_kb > MaxSizeKb)
         return MaxSizeKb;

      return route.max_size_kb;
   }

   bool
   InboundWebhooks::Spool_(std::shared_ptr<Message> message, const Route &route, const String &recipients, const String &quiet,
                           __int64 size, const String &senderIp, bool tooLarge, const String &tooLargeReason)
   {
      // The copy, through the store: the queue file's bytes as the message
      // arrived, into a working file of its own beside it.
      std::shared_ptr<Message> copy = std::shared_ptr<Message>(new Message());
      const String copyFile = MessageStore::WorkingFile(copy);

      if (!MessageStore::CopyOriginal(MessageStore::WorkingFile(message), copyFile))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7400, "InboundWebhooks::TakeRoutedRecipients",
            Formatter::Format("Message {0} could not be copied for the inbound webhook of {1}. It is kept in the delivery queue and tried again.",
               message->GetID(), route.address));
         return false;
      }

      const __int64 now = (__int64) time(nullptr);

      SQLStatement statement(SQLStatement::STInsert, _T("hm_inboundhookqueue"));
      statement.AddColumnInt64(_T("iqhookid"), route.id);
      statement.AddColumnInt64(_T("iqcreated"), now);
      statement.AddColumn(_T("iqstate"), (long) (tooLarge ? QueueKept : QueuePending));
      statement.AddColumn(_T("iqtries"), (long) 0);

      // Due at once. The first attempt is made by the drain posted below, or by
      // the next pass - never from here, the delivery thread.
      statement.AddColumnInt64(_T("iqnext"), tooLarge ? (__int64) 0 : now);
      statement.AddColumn(_T("iqerror"), tooLarge ? tooLargeReason : String(), 255);
      statement.AddColumn(_T("iqfile"), copy->GetPartialFileName(), 255);
      statement.AddColumnInt64(_T("iqqueueid"), message->GetID());
      statement.AddColumn(_T("iqsender"), message->GetFromAddress(), 255);
      statement.AddColumn(_T("iqrecipients"), recipients);

      // Those of them whose sender asked, with DSN NOTIFY, not to be told of a
      // failure: a message returned when the endpoint never accepted it is
      // reported for the others only (RFC 3461 4.1).
      statement.AddColumn(_T("iqquiet"), quiet);
      statement.AddColumn(_T("iqsenderip"), senderIp, 64);
      statement.AddColumn(_T("iqspam"), (long) (message->GetFlagSpam() ? 1 : 0));
      statement.AddColumnInt64(_T("iqsize"), size);
      statement.SetIdentityColumn(_T("iqid"));

      String errorMessage;
      if (!Application::Instance()->GetDBManager()->Execute(statement, nullptr, 0, errorMessage))
      {
         MessageStore::DiscardWorkingFile(copyFile);

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7400, "InboundWebhooks::TakeRoutedRecipients",
            Formatter::Format("Message {0} could not be queued for the inbound webhook of {1}: {2}. It is kept in the delivery queue and tried again.",
               message->GetID(), route.address, errorMessage));
         return false;
      }

      return true;
   }

   bool
   InboundWebhooks::ExpandForMailbox_(std::shared_ptr<Message> message, std::shared_ptr<MessageRecipient> routed,
                                      const std::set<__int64> &routesTaken)
   {
      // The address as it would be with no route: its account, the alias's
      // target, the list's members - or the managed list itself, which the list
      // manager distributes next - a catch-all, whatever this server would have
      // delivered it to, and nothing when it is none of them. The routes this
      // message has been taken by are passed over; a route it has not, reached
      // through an alias or a list, makes a route's recipient of its own, which
      // the caller takes in its next round.
      std::shared_ptr<MessageRecipients> expanded = std::shared_ptr<MessageRecipients>(new MessageRecipients());

      RecipientParser parser;
      parser.SetRoutesTaken(routesTaken);

      // Who may reach what the address leads to is decided first, by the parser
      // that then expands it, exactly as at RCPT TO: a list's posting rules
      // refuse a sender here as they would have there, and a posting the list
      // sends to its moderator is marked for the moderator, so the expansion
      // below goes to the moderator and not to the members. The sender is taken
      // as it was then - signed in or not (Message::FlagSubmittedBySignedInUser)
      // - since that is what a list requiring authentication asks.
      String refusal;
      bool treatAsLocal = false;
      const RecipientParser::DeliveryPossibility possible =
         parser.CheckDeliveryPossibility(message->GetSubmittedBySignedInUser(), message->GetFromAddress(), routed->GetAddress(),
                                         refusal, treatAsLocal, 0);

      if (possible != RecipientParser::DP_Possible)
      {
         if (!refusal.IsEmpty())
         {
            LOG_APPLICATION(Formatter::Format(_T("InboundWebhooks: message {0} is not delivered to what {1} reaches without its route: {2}"),
               message->GetID(), routed->GetAddress(), refusal));
         }

         return false;
      }

      bool recipientOK = false;
      parser.CreateMessageRecipientList(routed->GetAddress(), expanded, recipientOK);

      std::set<String> present;
      {
         // owned by this call: the recipients of the message this delivery holds
         const std::vector<std::shared_ptr<MessageRecipient> > &live = message->GetRecipients()->GetVector();
         for (const std::shared_ptr<MessageRecipient> &recipient : live)
         {
            String key = recipient->GetAddress();
            key.MakeLower();
            present.insert(key);
         }
      }

      bool reached = false;

      // owned by this call: the collection the parser above filled, made in this call
      const std::vector<std::shared_ptr<MessageRecipient> > &mailboxes = expanded->GetVector();
      for (const std::shared_ptr<MessageRecipient> &mailbox : mailboxes)
      {
         String key = mailbox->GetAddress();
         key.MakeLower();

         // One the message already has is reached already.
         reached = true;

         if (!present.insert(key).second)
            continue;

         mailbox->SetMessageID(message->GetID());
         mailbox->SetOriginalAddress(routed->GetOriginalAddress());
         mailbox->SetDSNNotify(routed->GetDSNNotify());

         // Written to the queue as well as put in the message, so that a
         // recipient outside this server the address led to is retried with the
         // message like any other.
         SQLStatement statement(SQLStatement::STInsert, _T("hm_messagerecipients"));
         statement.AddColumnInt64(_T("recipientmessageid"), mailbox->GetMessageID());
         statement.AddColumn(_T("recipientaddress"), mailbox->GetAddress());
         statement.AddColumnInt64(_T("recipientlocalaccountid"), mailbox->GetLocalAccountID());
         statement.AddColumn(_T("recipientoriginaladdress"), mailbox->GetOriginalAddress());
         statement.AddColumnInt64(_T("recipientdsnnotify"), (__int64) mailbox->GetDSNNotify());
         statement.SetIdentityColumn(_T("recipientid"));

         if (!Application::Instance()->GetDBManager()->Execute(statement))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7407, "InboundWebhooks::TakeRoutedRecipients",
               Formatter::Format("Message {0} is delivered to {1} as well as posted by its inbound webhook, but that recipient could not be written to the delivery queue. It is delivered in this pass; if the message is tried again, that recipient is not.",
                  message->GetID(), mailbox->GetAddress()));
         }

         message->GetRecipients()->Add(mailbox);
      }

      return reached;
   }

   void
   InboundWebhooks::TakeRoutedRecipients(std::shared_ptr<Message> message,
                                         std::vector<std::shared_ptr<MessageRecipient> > &held)
   {
      if (!message)
         return;

      std::shared_ptr<MessageRecipients> recipients = message->GetRecipients();
      if (!recipients)
         return;

      // The one that must cost nothing: a message none of whose recipients has
      // the shape a route's recipient has - no account behind it, at a domain
      // this server hosts - which is nearly every message, route or no route.
      {
         bool anyCandidate = false;

         // owned by this call: the recipients of the message this delivery holds
         const std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients = recipients->GetVector();
         for (const std::shared_ptr<MessageRecipient> &recipient : vecRecipients)
         {
            if (IsCandidate_(recipient))
            {
               anyCandidate = true;
               break;
            }
         }

         if (!anyCandidate)
            return;
      }

      bool routesRead = false;
      {
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);
         routesRead = EnsureCache_();
      }

      if (!routesRead)
      {
         // The routes have never been read and cannot be now: which of these
         // recipients is a route's is not known. Each that is nobody else's
         // stays in the delivery queue and is tried again with the message, as
         // one whose copy could not be kept is - never sent on as though it
         // were an address outside this server.
         // owned by this call: the recipients of the message this delivery holds
         const std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients = recipients->GetVector();
         for (const std::shared_ptr<MessageRecipient> &recipient : vecRecipients)
         {
            if (IsCandidate_(recipient) && !IsSomebodyElses_(recipient->GetAddress()))
               held.push_back(recipient);
         }

         if (!held.empty())
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7405, "InboundWebhooks::TakeRoutedRecipients",
               Formatter::Format("The inbound webhook routes could not be read, so message {0} is kept in the delivery queue for {1} recipient(s) that may be a route's, and tried again.",
                  message->GetID(), (int) held.size()));
         }

         return;
      }

      struct Share
      {
         Route route;
         std::vector<std::shared_ptr<MessageRecipient> > recipients;
      };

      // Every recipient looked at, kept alive here so that one taken off the
      // message cannot be mistaken for a new one made at its address.
      std::set<std::shared_ptr<MessageRecipient> > seen;

      // The routes that have the message. A route that delivers as well puts
      // back what its address reaches, and what that reaches through another
      // route is that route's in the next round; a route never takes the same
      // message twice.
      std::set<__int64> routesTaken;

      // 5.1.1 and 5.3.4, reported together at the end, while the queue file is
      // still the message's: local delivery may take it for a mailbox next.
      std::vector<DeliveryFailure> returned;

      unsigned __int64 size = 0;
      bool sized = false;
      String senderIp;

      bool queuedAny = false;

      for (int round = 0; round < MaxRounds; round++)
      {
         std::vector<Share> shares;
         std::vector<std::shared_ptr<MessageRecipient> > unrouted;

         {
            // owned by this call: the recipients of the message this delivery holds
            const std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients = recipients->GetVector();

            for (const std::shared_ptr<MessageRecipient> &recipient : vecRecipients)
            {
               if (!IsCandidate_(recipient) || !seen.insert(recipient).second)
                  continue;

               Route route;
               const bool routed = FindRoute_(recipient->GetAddress(), route, false);

               if (round > 0)
               {
                  // What a route that delivers as well put back: a route's own
                  // recipient only for a route that is on and has not had the
                  // message - the parser makes none for any other - and anything
                  // else, a managed list or an SMTP route's recipient, somebody
                  // else's to deliver.
                  if (!routed || !route.enabled || routesTaken.find(route.id) != routesTaken.end())
                     continue;
               }
               else if (!routed)
               {
                  // No route takes it now. Either it is somebody else's - the list
                  // manager's, an SMTP route's - or a route's that has been removed
                  // since the message was accepted, which is what the address is
                  // without one. Never sent on as an address outside this server.
                  if (!IsSomebodyElses_(recipient->GetAddress()))
                     unrouted.push_back(recipient);

                  continue;
               }
               else if (!route.enabled)
               {
                  // Switched off since the message was accepted, and no route
                  // that is on takes the address instead (FindRoute_ prefers one):
                  // the address is what it is with no route.
                  unrouted.push_back(recipient);
                  continue;
               }

               bool added = false;
               for (Share &share : shares)
               {
                  if (share.route.id == route.id)
                  {
                     share.recipients.push_back(recipient);
                     added = true;
                     break;
                  }
               }

               if (!added)
               {
                  Share share;
                  share.route = route;
                  share.recipients.push_back(recipient);
                  shares.push_back(share);
               }
            }
         }

         if (shares.empty() && unrouted.empty())
            break;

         // Taken before anything is expanded, so that what one route's address
         // reaches through another route of this round is delivered there, and
         // not posted to that route a second time.
         for (const Share &share : shares)
            routesTaken.insert(share.route.id);

         for (const std::shared_ptr<MessageRecipient> &recipient : unrouted)
         {
            TakeOff_(message, recipient);

            if (ExpandForMailbox_(message, recipient, routesTaken))
               continue;

            // An address that is then nobody's is refused as an unknown one is.
            if (WantsFailureReport(recipient))
            {
               returned.push_back(DeliveryFailure(recipient->GetAddress(), _T("5.1.1"),
                  Formatter::Format(_T("{0}\r\n   Error Type: SMTP\r\n   Error Description: Delivery failed\r\n   Additional information: Unknown user.\r\n\r\n"),
                     recipient->GetAddress())));
            }
         }

         if (shares.empty())
            continue;

         if (!sized)
         {
            MessageStore::FileSize64(MessageStore::WorkingFile(message), size);
            senderIp = MessageUtilities::GetSendersIP(message);
            sized = true;
         }

         for (const Share &share : shares)
         {
            String addresses;
            String quiet;

            for (const std::shared_ptr<MessageRecipient> &recipient : share.recipients)
            {
               if (!addresses.IsEmpty())
                  addresses += _T(",");

               addresses += recipient->GetAddress();

               if (!WantsFailureReport(recipient))
               {
                  if (!quiet.IsEmpty())
                     quiet += _T(",");

                  quiet += recipient->GetAddress();
               }
            }

            // The route's own ceiling, and never more than MessageData parses:
            // a larger message could not be made into a body at all.
            const int ceilingKb = CeilingKb_(share.route);
            const bool tooLarge = size > (unsigned __int64) ceilingKb * 1024;

            String tooLargeReason;
            if (tooLarge)
            {
               tooLargeReason = Formatter::Format(_T("the message is {0} KB, and the route takes at most {1} KB"),
                  (__int64) ((size + 1023) / 1024), ceilingKb);
            }

            if (tooLarge && share.route.on_failure == FailureBounce)
            {
               // Not posted, and returned to its sender now - RFC 3463 5.3.4, too
               // big for the system - for each address that reaches nothing
               // else. A route that delivers as well still delivers: only the
               // post is refused, and a sender whose message reached the mailbox
               // is not told it failed.
               for (const std::shared_ptr<MessageRecipient> &recipient : share.recipients)
               {
                  TakeOff_(message, recipient);

                  if (share.route.deliver && ExpandForMailbox_(message, recipient, routesTaken))
                     continue;

                  if (WantsFailureReport(recipient))
                  {
                     returned.push_back(DeliveryFailure(recipient->GetAddress(), _T("5.3.4"),
                        Formatter::Format(_T("{0}\r\n   Error Type: SMTP\r\n   Error Description: Delivery failed\r\n   Additional information: The message is larger than this address accepts ({1} KB at most).\r\n\r\n"),
                           recipient->GetAddress(), ceilingKb)));
                  }
               }

               LOG_APPLICATION(Formatter::Format(_T("InboundWebhooks: message {0} to {1} was not posted: {2}. It is returned to its sender, as the route says{3}."),
                  message->GetID(), share.route.address, tooLargeReason,
                  String(share.route.deliver ? _T(", except where it reached a mailbox") : _T(""))));
               continue;
            }

            if (!Spool_(message, share.route, addresses, quiet, (__int64) size, senderIp, tooLarge, tooLargeReason))
            {
               // Kept in the message through local delivery and set aside after
               // it, as the list manager's held postings are, and tried again with
               // the message. The list manager is told to leave them alone.
               for (const std::shared_ptr<MessageRecipient> &recipient : share.recipients)
                  held.push_back(recipient);

               continue;
            }

            if (tooLarge)
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7403, "InboundWebhooks::TakeRoutedRecipients",
                  Formatter::Format("Message {0} to {1} was not posted: {2}. It is kept, as the route says, and can be read, retried or removed at /api/v1/inbound-webhooks/deliveries.",
                     message->GetID(), share.route.address, tooLargeReason));
            }
            else
            {
               queuedAny = true;
            }

            LOG_APPLICATION(Formatter::Format(_T("InboundWebhooks: message {0} to {1} is queued for the inbound webhook of {2}{3}."),
               message->GetID(), addresses, share.route.address,
               String(share.route.deliver ? _T(", and delivered to the mailbox as well") : _T(""))));

            for (const std::shared_ptr<MessageRecipient> &recipient : share.recipients)
            {
               TakeOff_(message, recipient);

               if (share.route.deliver)
                  ExpandForMailbox_(message, recipient, routesTaken);
            }
         }
      }

      if (!returned.empty())
         SMTPDeliverer::ReturnToSender(message, returned);

      if (queuedAny)
         ScheduleDrain_();
   }

   void
   InboundWebhooks::SetAside(std::shared_ptr<Message> message, const std::vector<std::shared_ptr<MessageRecipient> > &held)
   {
      if (!message || held.empty())
         return;

      std::shared_ptr<MessageRecipients> recipients = message->GetRecipients();
      if (!recipients)
         return;

      // owned by this call: the recipients of the message this delivery holds, erased through the reference
      std::vector<std::shared_ptr<MessageRecipient> > &live = recipients->GetVector();
      for (const std::shared_ptr<MessageRecipient> &recipient : held)
      {
         auto found = std::find(live.begin(), live.end(), recipient);
         if (found != live.end())
            live.erase(found);
      }
   }

   bool
   InboundWebhooks::HoldForRetry(std::shared_ptr<Message> message, const std::vector<std::shared_ptr<MessageRecipient> > &held,
                                 bool alreadyRescheduled, std::vector<DeliveryFailure> &failures)
   {
      if (!message || held.empty())
         return alreadyRescheduled;

      // Rescheduled already, for another recipient: the held rows stay with the
      // message and are tried on its next pass with the rest.
      if (alreadyRescheduled)
         return true;

      std::shared_ptr<SMTPConfiguration> smtp = Configuration::Instance()->GetSMTPConfiguration();
      const long retries = smtp ? smtp->GetNoOfRetries() : 0;
      long minutes = smtp ? smtp->GetMinutesBetweenTry() : 60;
      if (minutes < 1)
         minutes = 1;

      if ((long) message->GetNoOfRetries() < retries)
      {
         PersistentMessage::SetNextTryTime(message->GetID(), true, minutes);
         PersistentMessage::UnlockObject(message);

         LOG_APPLICATION(Formatter::Format(_T("InboundWebhooks: message {0} could not be queued for its inbound webhook and is tried again in {1} minute(s), try {2} of {3}."),
            message->GetID(), (int) minutes, (int) message->GetNoOfRetries() + 1, (int) retries));
         return true;
      }

      // The retries are spent, and the sender is told: 4.3.0, since what failed
      // is this server and not the address.
      for (const std::shared_ptr<MessageRecipient> &recipient : held)
      {
         const int notify = recipient->GetDSNNotify();
         const bool suppressFailureDsn = notify != MessageRecipient::DSNNotifyDefault &&
                                         (notify & MessageRecipient::DSNNotifyFailure) == 0;

         if (!suppressFailureDsn)
         {
            failures.push_back(DeliveryFailure(recipient->GetAddress(), _T("4.3.0"),
               Formatter::Format(_T("{0}\r\n   Error Type: SMTP\r\n   Error Description: Delivery failed\r\n   Additional information: The message could not be kept to be passed on for this address, and was tried again until the retries ran out. The server administrator should check the hMailServer error log.\r\n\r\n"),
                  recipient->GetAddress())));
         }

         if (!PersistentMessageRecipient::DeleteObject(recipient))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7406, "InboundWebhooks::HoldForRetry",
               Formatter::Format("Message {0} to {1} has run out of retries, and that recipient could not be removed from the delivery queue.",
                  message->GetID(), recipient->GetAddress()));
         }
      }

      LOG_APPLICATION(Formatter::Format(_T("InboundWebhooks: message {0} could not be queued for its inbound webhook after {1} tries; its sender is told."),
         message->GetID(), (int) message->GetNoOfRetries() + 1));

      return false;
   }

   // ---- The queue -------------------------------------------------------------------

   bool
   InboundWebhooks::GetDeliveries(const String &routeAddress, int state, int limit, std::vector<Delivery> &deliveries,
                                  const std::set<__int64> *onlyRoutes)
   {
      deliveries.clear();

      // A caller that reaches no route has no deliveries, whatever the queue
      // holds for the others.
      if (onlyRoutes != nullptr && onlyRoutes->empty())
         return true;

      if (limit < 1)
         limit = 1;
      if (limit > 1000)
         limit = 1000;

      std::vector<Route> routes;
      if (!ReadRoutes_(routes))
         return false;

      __int64 routeId = 0;

      if (!routeAddress.IsEmpty())
      {
         Route one;
         if (!GetRoute(routeAddress, one))
            return true;   // an address with no route has no deliveries, which is not an error

         routeId = one.id;
      }

      String sql = _T("select * from hm_inboundhookqueue");
      String where;

      if (routeId > 0)
         where = _T("iqhookid = @HOOKID");

      if (state > 0)
         where = where.IsEmpty() ? String(_T("iqstate = @STATE")) : where + _T(" and iqstate = @STATE");

      if (!where.IsEmpty())
         sql += _T(" where ") + where;

      sql += _T(" order by iqid desc");

      SQLCommand command(sql);

      if (routeId > 0)
         command.AddParameter("@HOOKID", routeId);
      if (state > 0)
         command.AddParameter("@STATE", state);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF() && (int) deliveries.size() < limit)
      {
         Delivery delivery = ReadDelivery_(recordset);

         // Another domain's row is passed over here rather than after the read,
         // so that the newest rows of a busier domain cannot fill the page and
         // leave the caller's own off it.
         if (onlyRoutes != nullptr && onlyRoutes->find(delivery.route_id) == onlyRoutes->end())
         {
            recordset->MoveNext();
            continue;
         }

         for (const Route &route : routes)
         {
            if (route.id == delivery.route_id)
            {
               delivery.route = route.address;
               break;
            }
         }

         deliveries.push_back(delivery);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   InboundWebhooks::GetDelivery(__int64 id, Delivery &delivery)
   {
      SQLCommand command(_T("select * from hm_inboundhookqueue where iqid = @ID"));
      command.AddParameter("@ID", id);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      delivery = ReadDelivery_(recordset);

      SQLCommand routeCommand(_T("select ihaddress from hm_inboundhooks where ihid = @HOOKID"));
      routeCommand.AddParameter("@HOOKID", delivery.route_id);

      std::shared_ptr<DALRecordset> routeRecordset = Application::Instance()->GetDBManager()->OpenRecordset(routeCommand);
      if (routeRecordset && !routeRecordset->IsEOF())
         delivery.route = routeRecordset->GetStringValue("ihaddress");

      return true;
   }

   bool
   InboundWebhooks::CountDeliveries(__int64 routeId, int &pending, int &delivered, int &kept, int &bounced)
   {
      pending = 0;
      delivered = 0;
      kept = 0;
      bounced = 0;

      SQLCommand command(_T("select iqstate, count(*) as statecount from hm_inboundhookqueue where iqhookid = @HOOKID group by iqstate"));
      command.AddParameter("@HOOKID", routeId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         const int state = recordset->GetLongValue("iqstate");
         const int count = recordset->GetLongValue("statecount");

         switch (state)
         {
         case QueuePending:   pending = count; break;
         case QueueDelivered: delivered = count; break;
         case QueueKept:      kept = count; break;
         case QueueBounced:   bounced = count; break;
         default: break;
         }

         recordset->MoveNext();
      }

      return true;
   }

   bool
   InboundWebhooks::UpdateDelivery_(__int64 id, int state, int tries, __int64 next, const String &error, bool fileGone)
   {
      if (fileGone)
      {
         SQLCommand command(_T("update hm_inboundhookqueue set iqstate = @STATE, iqtries = @TRIES, iqnext = @NEXT, iqerror = @ERROR, iqfile = @FILE where iqid = @ID"));
         command.AddParameter("@STATE", state);
         command.AddParameter("@TRIES", tries);
         command.AddParameter("@NEXT", next);
         command.AddParameter("@ERROR", Shorten(OneLine(error), MaxErrorLength));
         command.AddParameter("@FILE", String());
         command.AddParameter("@ID", id);

         return Application::Instance()->GetDBManager()->Execute(command);
      }

      SQLCommand command(_T("update hm_inboundhookqueue set iqstate = @STATE, iqtries = @TRIES, iqnext = @NEXT, iqerror = @ERROR where iqid = @ID"));
      command.AddParameter("@STATE", state);
      command.AddParameter("@TRIES", tries);
      command.AddParameter("@NEXT", next);
      command.AddParameter("@ERROR", Shorten(OneLine(error), MaxErrorLength));
      command.AddParameter("@ID", id);

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   InboundWebhooks::RetryDelivery(__int64 id, String &refusal)
   {
      refusal = _T("");

      Delivery delivery;
      if (!GetDelivery(id, delivery))
      {
         refusal = _T("there is no such delivery");
         return false;
      }

      if (delivery.state != QueueKept && delivery.state != QueuePending)
      {
         refusal = _T("only a kept or a pending delivery can be tried again; this one was delivered or returned to its sender");
         return false;
      }

      if (delivery.file.IsEmpty() || !MessageStore::Exists(FileOf_(delivery.file)))
      {
         refusal = _T("the copy of the message this delivery kept is gone, so there is nothing to post");
         return false;
      }

      // Due now, and a kept one with its attempts at none: a retry by hand is
      // the administrator saying the endpoint has been put right.
      const int tries = delivery.state == QueueKept ? 0 : delivery.tries;

      if (!UpdateDelivery_(id, QueuePending, tries, (__int64) time(nullptr), delivery.error, false))
      {
         refusal = _T("the delivery could not be updated");
         return false;
      }

      ScheduleDrain_();
      return true;
   }

   bool
   InboundWebhooks::DeleteDelivery(__int64 id)
   {
      Delivery delivery;
      if (!GetDelivery(id, delivery))
         return false;

      SQLCommand command(_T("delete from hm_inboundhookqueue where iqid = @ID"));
      command.AddParameter("@ID", id);

      if (!Application::Instance()->GetDBManager()->Execute(command))
         return false;

      if (!delivery.file.IsEmpty())
         MessageStore::DiscardWorkingFile(FileOf_(delivery.file));

      return true;
   }

   bool
   InboundWebhooks::ReadDeliveryMessage(__int64 id, AnsiString &bytes)
   {
      Delivery delivery;
      if (!GetDelivery(id, delivery) || delivery.file.IsEmpty())
         return false;

      return MessageStore::ReadAll(FileOf_(delivery.file), bytes);
   }

   // ---- The body ----------------------------------------------------------------------

   bool
   InboundWebhooks::BuildBody(const Route &route, const Delivery &delivery, const AnsiString &raw, const String &file,
                              AnsiString &body, AnsiString &contentType, String &error)
   {
      body = "";
      contentType = "";

      // The parsed parts, through MessageData, which reads the copy through the
      // store (MessageStorePlainCopy): a working file in the queue directory is
      // read as the bytes it holds.
      std::shared_ptr<Message> parsed = std::shared_ptr<Message>(new Message(false));
      std::shared_ptr<MessageData> data = std::shared_ptr<MessageData>(new MessageData());

      if (!MessageStore::Exists(file) || !data->LoadFromMessage(file, parsed))
      {
         error = _T("the copy of the message kept for this delivery could not be read as a message");
         return false;
      }

      const AnsiString from = Utf8(data->GetFrom());
      const AnsiString to = Utf8(data->GetTo());
      const AnsiString cc = Utf8(data->GetCC());
      const AnsiString subject = Utf8(data->GetSubject());
      const AnsiString text = Utf8(data->GetBody());
      const AnsiString html = Utf8(data->GetHTMLBody());
      const AnsiString headers = HeaderBlock(raw);

      std::shared_ptr<MimeBody> root = data->GetMimeMessage();

      // The verdicts: the spam classification the message was given when it
      // arrived, and the score and the three authentication results only where
      // this server wrote them.
      const AnsiString ownResults = OwnAuthenticationResults(root);
      const AnsiString spf = MethodResult(ownResults, "spf");
      const AnsiString dkim = MethodResult(ownResults, "dkim");
      const AnsiString dmarc = MethodResult(ownResults, "dmarc");

      int spamScore = -1;
      if (delivery.spam && Configuration::Instance()->GetAntiSpamConfiguration().GetAddHeaderReason())
      {
         const int score = _ttoi(data->GetFieldValue(_T("X-hMailServer-Reason-Score")));
         if (score > 0)
            spamScore = score;
      }

      std::vector<AnsiString> envelopeTo;
      {
         std::vector<String> recipients = StringParser::SplitString(delivery.recipients, _T(","));
         for (String recipient : recipients)
         {
            recipient.Trim();
            if (!recipient.IsEmpty())
               envelopeTo.push_back(Utf8(recipient));
         }
      }

      AnsiString envelope = "{\"to\":[";
      for (size_t i = 0; i < envelopeTo.size(); i++)
      {
         if (i > 0)
            envelope += ",";
         envelope += JsonString(envelopeTo[i]);
      }
      envelope += "],\"from\":";
      envelope += JsonString(Utf8(delivery.sender));
      envelope += "}";

      // The attachments: what MimeBody counts as one - a disposition of
      // attachment, a named inline part, a part that is neither text nor
      // multipart.
      std::vector<CarriedAttachment> attachments;
      if (root)
      {
         MimeBody::BodyList parts;
         root->GetAttachmentList(root, parts);

         for (std::shared_ptr<MimeBody> part : parts)
         {
            if (!part)
               continue;

            CarriedAttachment attachment;
            attachment.filename = Utf8(part->GetUnicodeFilename());

            String type = part->GetCleanContentType();
            type.MakeLower();
            if (type.GetLength() < 3 || type.Find(_T("/")) <= 0)
               type = _T("application/octet-stream");
            attachment.type = Utf8(type);

            const char *contentId = part->GetRawFieldValue(_T("Content-ID"));
            if (contentId != nullptr)
            {
               attachment.content_id = contentId;
               attachment.content_id.Trim();
               attachment.content_id.Replace("<", "");
               attachment.content_id.Replace(">", "");
            }

            part->GetDecodedContent(attachment.bytes);
            attachment.size = attachment.bytes.GetLength();

            if (route.attachments != AttachmentsInclude)
               attachment.bytes = "";

            attachments.push_back(attachment);
         }
      }

      // A route that leaves attachments out lists none of them either, and says
      // only how many there were.
      int leftOut = 0;
      if (route.attachments == AttachmentsNone)
      {
         leftOut = (int) attachments.size();
         attachments.clear();
      }

      AnsiString numbers;

      if (route.format == FormatJson)
      {
         contentType = "application/json";

         body += "{\"event\":\"message.inbound\",";
         body += "\"route\":" + JsonString(Utf8(route.address)) + ",";

         // When the message was taken, not when this attempt was made: the body
         // is the same bytes on every attempt.
         numbers.Format("\"delivery\":%I64d,\"time\":%I64d,", delivery.id, delivery.created);
         body += numbers;
         body += "\"time_utc\":" + JsonString(Utf8(TimeText(delivery.created))) + ",";
         body += "\"server\":" + JsonString(Utf8(Configuration::Instance()->GetHostName())) + ",";

         if (delivery.queue_id > 0)
         {
            numbers.Format("\"queue_id\":%I64d,", delivery.queue_id);
            body += numbers;
         }

         body += "\"envelope\":" + envelope + ",";
         body += "\"sender_ip\":" + JsonString(Utf8(delivery.sender_ip)) + ",";
         body += "\"from\":" + JsonString(from) + ",";
         body += "\"to\":" + JsonString(to) + ",";
         if (!cc.IsEmpty())
            body += "\"cc\":" + JsonString(cc) + ",";
         body += "\"subject\":" + JsonString(subject) + ",";
         body += "\"headers\":";
         body += JsonString(headers);
         body += ",\"text\":";
         body += JsonString(text);
         body += ",\"html\":";
         body += JsonString(html);

         body += ",\"verdicts\":{\"spam\":";
         body += delivery.spam ? "true" : "false";
         if (spamScore > 0)
         {
            numbers.Format(",\"spam_score\":%d", spamScore);
            body += numbers;
         }
         if (!spf.IsEmpty())
            body += ",\"spf\":" + JsonString(spf);
         if (!dkim.IsEmpty())
            body += ",\"dkim\":" + JsonString(dkim);
         if (!dmarc.IsEmpty())
            body += ",\"dmarc\":" + JsonString(dmarc);
         body += "}";

         body += ",\"attachments\":[";
         for (size_t i = 0; i < attachments.size(); i++)
         {
            const CarriedAttachment &attachment = attachments[i];

            if (i > 0)
               body += ",";

            body += "{\"filename\":" + JsonString(attachment.filename);
            body += ",\"type\":" + JsonString(attachment.type);
            if (!attachment.content_id.IsEmpty())
               body += ",\"content_id\":" + JsonString(attachment.content_id);
            numbers.Format(",\"size\":%I64d", attachment.size);
            body += numbers;

            if (route.attachments == AttachmentsInclude)
            {
               body += ",\"content\":\"";
               body += Base64::Encode(attachment.bytes.c_str(), attachment.bytes.GetLength());
               body += "\"";
            }

            body += "}";
         }
         body += "]";

         if (leftOut > 0)
         {
            numbers.Format(",\"attachments_left_out\":%d", leftOut);
            body += numbers;
         }

         // The message as it arrived, as base64: its bytes are whatever the
         // sender wrote, and a JSON string can only hold UTF-8.
         if (route.include_raw)
         {
            body += ",\"raw\":\"";
            body += Base64::Encode(raw.c_str(), raw.GetLength());
            body += "\"";
         }

         body += "}";
         return true;
      }

      // multipart/form-data, in the field names SendGrid's Inbound Parse posts,
      // so that a receiver written for it reads this one.
      std::vector<FormPart> parts;

      AddField(parts, "headers", headers);
      AddField(parts, "from", from);
      AddField(parts, "to", to);
      if (!cc.IsEmpty())
         AddField(parts, "cc", cc);
      AddField(parts, "subject", subject);
      if (!text.IsEmpty())
         AddField(parts, "text", text);
      if (!html.IsEmpty())
         AddField(parts, "html", html);
      AddField(parts, "envelope", envelope);
      AddField(parts, "sender_ip", Utf8(delivery.sender_ip));
      AddField(parts, "spam", delivery.spam ? "true" : "false");

      if (spamScore > 0)
      {
         numbers.Format("%d", spamScore);
         AddField(parts, "spam_score", numbers);
      }

      if (!spf.IsEmpty())
         AddField(parts, "SPF", spf);
      if (!dkim.IsEmpty())
         AddField(parts, "dkim", dkim);
      if (!dmarc.IsEmpty())
         AddField(parts, "dmarc", dmarc);

      // Every text field above is UTF-8, whatever the message was written in.
      AnsiString charsets = "{\"to\":\"UTF-8\",\"from\":\"UTF-8\",\"subject\":\"UTF-8\",\"text\":\"UTF-8\",\"html\":\"UTF-8\"";
      if (!cc.IsEmpty())
         charsets += ",\"cc\":\"UTF-8\"";
      charsets += "}";
      AddField(parts, "charsets", charsets);

      numbers.Format("%d", (int) attachments.size());
      AddField(parts, "attachments", numbers);

      if (leftOut > 0)
      {
         numbers.Format("%d", leftOut);
         AddField(parts, "attachments_left_out", numbers);
      }

      if (!attachments.empty())
      {
         AnsiString info = "{";
         AnsiString contentIds = "{";
         bool anyContentId = false;

         for (size_t i = 0; i < attachments.size(); i++)
         {
            const CarriedAttachment &attachment = attachments[i];

            AnsiString key;
            key.Format("attachment%d", (int) i + 1);

            if (i > 0)
               info += ",";

            info += "\"" + key + "\":{\"filename\":" + JsonString(attachment.filename);
            info += ",\"name\":" + JsonString(attachment.filename);
            info += ",\"type\":" + JsonString(attachment.type);
            if (!attachment.content_id.IsEmpty())
               info += ",\"content-id\":" + JsonString(attachment.content_id);
            numbers.Format(",\"size\":%I64d}", attachment.size);
            info += numbers;

            if (!attachment.content_id.IsEmpty())
            {
               if (anyContentId)
                  contentIds += ",";
               contentIds += JsonString(attachment.content_id) + ":\"" + key + "\"";
               anyContentId = true;
            }
         }

         info += "}";
         contentIds += "}";

         AddField(parts, "attachment-info", info);
         if (anyContentId)
            AddField(parts, "content-ids", contentIds);

         if (route.attachments == AttachmentsInclude)
         {
            for (size_t i = 0; i < attachments.size(); i++)
            {
               FormPart part;
               part.name.Format("attachment%d", (int) i + 1);
               part.filename = attachments[i].filename.IsEmpty() ? part.name : attachments[i].filename;
               part.content_type = attachments[i].type;
               part.value = attachments[i].bytes;
               parts.push_back(part);
            }
         }
      }

      if (route.include_raw)
         AddField(parts, "email", raw);

      // Ours, beside SendGrid's names.
      AddField(parts, "route", Utf8(route.address));

      if (delivery.queue_id > 0)
      {
         numbers.Format("%I64d", delivery.queue_id);
         AddField(parts, "queue_id", numbers);
      }

      // The boundary: the SHA-256 of the message, which the message cannot
      // contain, and the delivery's id - the same on every attempt, so a retry
      // is the same bytes. Lengthened in the vanishing case that some part does
      // contain it.
      AnsiString digest;
      if (raw.GetLength() > 0)
      {
         HashCreator hasher(HashCreator::SHA256);
         digest = hasher.GenerateHashNoSalt((unsigned char *) raw.c_str(), raw.GetLength(), HashCreator::hex);
      }

      AnsiString boundary;
      boundary.Format("hMailServer-%I64d-", delivery.id);
      boundary += digest.Mid(0, 32);

      for (int attempt = 0; attempt < 8; attempt++)
      {
         bool clash = false;
         for (const FormPart &part : parts)
         {
            if (part.value.Find(boundary) >= 0)
            {
               clash = true;
               break;
            }
         }

         if (!clash)
            break;

         boundary += "x";
      }

      contentType = "multipart/form-data; boundary=" + boundary;

      for (const FormPart &part : parts)
      {
         body += "--";
         body += boundary;
         body += "\r\nContent-Disposition: form-data; name=\"";
         body += part.name;
         body += "\"";

         if (!part.filename.IsEmpty())
         {
            body += "; filename=\"";
            body += DispositionValue(part.filename);
            body += "\"";
         }

         body += "\r\n";

         if (!part.content_type.IsEmpty())
         {
            body += "Content-Type: ";
            body += part.content_type;
            body += "\r\n";
         }

         body += "\r\n";
         body += part.value;
         body += "\r\n";
      }

      body += "--";
      body += boundary;
      body += "--\r\n";

      return true;
   }

   // ---- The pass ------------------------------------------------------------------------

   bool
   InboundWebhooks::StillQueued_(__int64 id)
   {
      // Pending, still: a row removed since the pass read it - by hand, with its
      // route, by an erasure - is gone, and one somebody retried or that another
      // server's pass settled is not this pass's any more.
      SQLCommand command(_T("select iqid from hm_inboundhookqueue where iqid = @ID and iqstate = 1"));
      command.AddParameter("@ID", id);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      return recordset && !recordset->IsEOF();
   }

   bool
   InboundWebhooks::Deliver_(const Route &route, const Delivery &delivery, String &error, bool &unreachable, bool &unreadable)
   {
      unreachable = false;
      unreadable = false;

      // The copy, as the message arrived, through the store.
      const String file = delivery.file.IsEmpty() ? String() : FileOf_(delivery.file);

      AnsiString raw;
      if (file.IsEmpty() || !MessageStore::ReadAll(file, raw))
      {
         unreadable = true;
         error = _T("the copy of the message kept for this delivery is missing or cannot be read");
         return false;
      }

      AnsiString body;
      AnsiString contentType;

      if (!BuildBody(route, delivery, raw, file, body, contentType, error))
      {
         unreadable = true;
         return false;
      }

      const __int64 timestamp = (__int64) time(nullptr);

      AnsiString secret;
      Unicode::WideToMultiByte(route.secret, secret);

      // The outbound webhooks' recipe, called rather than copied, so that a
      // receiver that verifies one verifies the other.
      const AnsiString signature = AlertManager::SignWebhook(secret, timestamp, body);

      std::vector<AnsiString> headers;

      AnsiString timestampHeader;
      timestampHeader.Format("X-hMailServer-Timestamp: %I64d", timestamp);
      headers.push_back(timestampHeader);
      headers.push_back("X-hMailServer-Signature: sha256=" + signature);
      headers.push_back("X-hMailServer-Event: message.inbound");

      AnsiString deliveryHeader;
      deliveryHeader.Format("X-hMailServer-Delivery: %I64d", delivery.id);
      headers.push_back(deliveryHeader);

      AnsiString attemptHeader;
      attemptHeader.Format("X-hMailServer-Attempt: %d", delivery.tries + 1);
      headers.push_back(attemptHeader);

      AnsiString url;
      Unicode::WideToMultiByte(route.url, url);

      HttpsClient::Response response;

      // A route the server's administrator made may reach this server's own
      // network but never a link-local or metadata address; one any other
      // credential made reaches public addresses only. Neither follows a
      // redirect. An answer longer than MaxResponseBytes is read that far and
      // judged by its status line: an endpoint that took the message and said
      // so at length has taken it.
      const bool answered = route.private_network
         ? HttpsClient::RequestNotLinkLocal("POST", url, headers, contentType, body, response, error, RequestTimeoutSeconds, MaxResponseBytes, true)
         : HttpsClient::RequestPublic("POST", url, headers, contentType, body, response, error, RequestTimeoutSeconds, MaxResponseBytes, true);

      if (!answered)
      {
         unreachable = true;
         return false;
      }

      if (response.status_code < 200 || response.status_code > 299)
      {
         error.Format(_T("the endpoint answered %d"), response.status_code);
         return false;
      }

      return true;
   }

   void
   InboundWebhooks::GiveUp_(const Route &route, const Delivery &delivery, int tries, const String &error, bool unreadable)
   {
      const String file = delivery.file.IsEmpty() ? String() : FileOf_(delivery.file);
      const bool copyThere = !file.IsEmpty() && MessageStore::Exists(file);

      // A sender that cannot be told - none, a mailer-daemon - is not sent a
      // report, as no delivery failure sends one; the message is kept instead,
      // rather than lost with nobody told.
      bool senderTold = !delivery.sender.IsEmpty() && !MailerDaemonAddressDeterminer::IsMailerDaemonAddress(delivery.sender);

      // Nor is one whose message is marked Auto-Submitted or has passed the
      // rule loop count, which is asked of the copy where there is one that
      // parses. One that does not - what HM7404 is - is returned on its sender
      // alone, as SMTPDeliverer::SubmitErrorLog_ returns a message it cannot
      // read: the sender is owed the news that it was not delivered.
      if (senderTold && copyThere)
      {
         std::shared_ptr<Message> parsed = std::shared_ptr<Message>(new Message(false));
         std::shared_ptr<MessageData> data = std::shared_ptr<MessageData>(new MessageData());

         if (data->LoadFromMessage(file, parsed))
            senderTold = RuleApplier::IsGeneratedResponseAllowed(data, true);
      }

      const bool bounce = route.on_failure == FailureBounce && senderTold;

      if (!bounce)
      {
         // A copy that is gone is not kept: the row stays, as the record of a
         // message nobody received, and says there is nothing to read.
         UpdateDelivery_(delivery.id, QueueKept, tries, 0, error, !copyThere);

         String description;

         if (unreadable)
         {
            description = Formatter::Format("Inbound webhooks: message {0} for {1} could not be posted to the endpoint of {2}, which never saw it: {3}.",
               delivery.queue_id, delivery.recipients, route.address, error);
         }
         else
         {
            description = Formatter::Format("Inbound webhooks: message {0} for {1} was not accepted by the endpoint of {2} after {3} attempt(s). The last failure was: {4}.",
               delivery.queue_id, delivery.recipients, route.address, tries, error);
         }

         if (copyThere)
            description += _T(" It is kept.");
         else
            description += _T(" Its copy is gone, so there is nothing to read or to post; the delivery is kept as the record of it.");

         if (route.on_failure == FailureBounce)
            description += _T(" It was to be returned to its sender, and the sender cannot be told: there is none, it is a mailer-daemon, or the message is itself automatic.");

         if (copyThere)
            description += Formatter::Format(" Read it, retry it or remove it at /api/v1/inbound-webhooks/deliveries/{0}.", delivery.id);
         else
            description += Formatter::Format(" Remove it at /api/v1/inbound-webhooks/deliveries/{0}.", delivery.id);

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, unreadable ? 7404 : 7401, "InboundWebhooks::RunDeliveries", description);
         return;
      }

      // Returned by the report every other failed delivery sends, for each
      // recipient whose sender did not ask, with DSN NOTIFY, not to be told.
      // The copy is the message as it arrived, and SMTPDeliverer reads it by
      // the queue file's name, which it has. 4.4.7: this server stopped trying.
      // What the endpoint answered is not repeated to the sender, who has no
      // business knowing where this server passes the mail on to.
      std::set<String> quiet;
      for (String address : StringParser::SplitString(delivery.quiet, _T(",")))
      {
         address.Trim();
         address.MakeLower();
         if (!address.IsEmpty())
            quiet.insert(address);
      }

      const String why = unreadable
         ? String(_T("The message was to be passed on to the application that receives this address's mail, and could not be."))
         : Formatter::Format(_T("The message was to be passed on to the application that receives this address's mail, and it was not accepted there in {0} attempt(s)."), tries);

      std::vector<DeliveryFailure> failures;
      for (String recipient : StringParser::SplitString(delivery.recipients, _T(",")))
      {
         recipient.Trim();
         if (recipient.IsEmpty())
            continue;

         String key = recipient;
         key.MakeLower();
         if (quiet.find(key) != quiet.end())
            continue;

         failures.push_back(DeliveryFailure(recipient, _T("4.4.7"),
            Formatter::Format(_T("{0}\r\n   Error Type: SMTP\r\n   Error Description: Delivery failed\r\n   Additional information: {1}\r\n\r\n"),
               recipient, why)));
      }

      std::shared_ptr<Message> original = std::shared_ptr<Message>(new Message(false));
      original->SetPartialFileName(delivery.file);
      original->SetFromAddress(delivery.sender);
      original->SetID(delivery.queue_id);

      if (!failures.empty())
         SMTPDeliverer::ReturnToSender(original, failures);

      UpdateDelivery_(delivery.id, QueueBounced, tries, 0, error, true);

      if (copyThere)
         MessageStore::DiscardWorkingFile(file);

      const String told = failures.empty()
         ? String(_T(" Its sender asked, with DSN NOTIFY, not to be told of a failure, and was not."))
         : String();

      if (unreadable)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7404, "InboundWebhooks::RunDeliveries",
            Formatter::Format("Inbound webhooks: message {0} for {1} could not be posted to the endpoint of {2}, which never saw it: {3}. It has been returned to its sender, as the route says.",
               delivery.queue_id, delivery.recipients, route.address, error) + told);
         return;
      }

      ErrorManager::Instance()->ReportError(ErrorManager::Low, 7402, "InboundWebhooks::RunDeliveries",
         Formatter::Format("Inbound webhooks: message {0} for {1} was not accepted by the endpoint of {2} after {3} attempt(s) and has been returned to its sender, as the route says. The last failure was: {4}.",
            delivery.queue_id, delivery.recipients, route.address, tries, error) + told);
   }

   int
   InboundWebhooks::RunDeliveries()
   {
      // One pass at a time, and nobody waits for one: a drain, the minute's
      // task and POST /run each try the lock and go when a pass holds it. A
      // drain that waited here would hold a thread of the asynchronous queue -
      // the queue SMTP's end of DATA is finished on - for as long as a pass
      // runs, and one posted per message would take them all.
      boost::unique_lock<boost::mutex> pass(pass_mutex_, boost::try_to_lock);
      if (!pass.owns_lock())
         return -1;

      {
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);

         // What was queued before this is in the rows read below; what is
         // queued from now on sets drain_wanted_ again, for PassEnded_.
         passing_ = true;
         drain_wanted_ = false;
      }

      int delivered = 0;

      try
      {
         delivered = Pass_();
      }
      catch (...)
      {
         pass.unlock();
         PassEnded_();
         throw;
      }

      pass.unlock();
      PassEnded_();

      return delivered;
   }

   void
   InboundWebhooks::PassEnded_()
   {
      bool again = false;

      {
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);
         passing_ = false;
         again = drain_wanted_;
      }

      // A message queued while the pass ran, whose drain was not posted since
      // a pass was running: its drain now.
      if (again)
         ScheduleDrain_();
   }

   int
   InboundWebhooks::Pass_()
   {
      const __int64 now = (__int64) time(nullptr);

      // The due rows of the routes that are on. A route switched off holds
      // its rows, not lost, until it is switched on again - and they are not
      // read at all meanwhile, so that however many there are they never stand
      // in front of another route's.
      SQLCommand command(_T("select hm_inboundhookqueue.* from hm_inboundhookqueue inner join hm_inboundhooks on ihid = iqhookid ")
                         _T("where iqstate = 1 and iqnext <= @NOW and ihenabled = 1 order by iqid asc"));
      command.AddParameter("@NOW", now);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return 0;

      std::vector<Delivery> due;
      while (!recordset->IsEOF() && (int) due.size() < MaxPerPass)
      {
         due.push_back(ReadDelivery_(recordset));
         recordset->MoveNext();
      }

      if (due.empty())
         return 0;

      std::vector<Route> routes;
      if (!ReadRoutes_(routes))
         return 0;

      const int maximumAttempts = Configuration::Instance()->GetWebhookMaxAttempts();
      int delivered = 0;

      // Routes whose endpoint gave no answer at all in this pass: an endpoint
      // that is down costs one deadline a pass, not one per message, and its
      // other due rows are moved to its next attempt, so that they never fill
      // the next pass's reading ahead of another route's.
      std::set<__int64> unreachableRoutes;
      int heldForNextPass = 0;

      for (const Delivery &delivery : due)
      {
         if ((__int64) time(nullptr) - now >= MaxPassSeconds)
         {
            heldForNextPass++;
            continue;
         }

         if (unreachableRoutes.find(delivery.route_id) != unreachableRoutes.end())
         {
            heldForNextPass++;
            continue;
         }

         const Route *route = nullptr;
         for (const Route &candidate : routes)
         {
            if (candidate.id == delivery.route_id)
            {
               route = &candidate;
               break;
            }
         }

         // Removed or switched off since the rows were read: held, as the
         // route's own rows are, and read again when it is on.
         if (route == nullptr || !route->enabled)
            continue;

         // Removed by hand, with its route or by an erasure since the rows were
         // read: not posted - the trail says it was removed - and not reported.
         if (!StillQueued_(delivery.id))
            continue;

         String error;
         bool unreachable = false;
         bool unreadable = false;

         if (Deliver_(*route, delivery, error, unreachable, unreadable))
         {
            UpdateDelivery_(delivery.id, QueueDelivered, delivery.tries + 1, 0, _T(""), true);

            const String file = FileOf_(delivery.file);
            if (!MessageStore::DiscardWorkingFile(file) && MessageStore::Exists(file))
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Low, 7408, "InboundWebhooks::RunDeliveries",
                  Formatter::Format("Inbound webhooks: message {0} was posted for {1}, but the copy kept for it could not be removed.",
                     delivery.queue_id, route->address));
            }

            delivered++;
            continue;
         }

         const int tries = delivery.tries + 1;

         // The attempt may have taken its whole deadline; a row removed
         // meanwhile is neither settled nor reported.
         if (!StillQueued_(delivery.id))
            continue;

         if (unreadable)
         {
            // Nothing a retry could change: the route's on_failure decides at
            // once, and GiveUp_ reports why (HM7404).
            GiveUp_(*route, delivery, tries, error, true);
            continue;
         }

         // The outbound webhooks' ladder: 0, 1, 5, 15, 60, 60 ... minutes,
         // keyed on the attempts made before this one.
         const __int64 next = now + (__int64) AlertManager::WebhookBackoffMinutes(tries - 1) * 60;

         if (unreachable)
         {
            unreachableRoutes.insert(delivery.route_id);

            // Its other due rows wait for its next attempt - a minute at the
            // least - without an attempt counted: what is tried is the endpoint,
            // and it has just been tried.
            SQLCommand defer(_T("update hm_inboundhookqueue set iqnext = @NEXT where iqhookid = @HOOKID and iqstate = 1 and iqnext <= @NOW and iqid <> @ID"));
            defer.AddParameter("@NEXT", next > now + 60 ? next : now + 60);
            defer.AddParameter("@HOOKID", delivery.route_id);
            defer.AddParameter("@NOW", now);
            defer.AddParameter("@ID", delivery.id);
            Application::Instance()->GetDBManager()->Execute(defer);
         }

         if (tries >= maximumAttempts)
         {
            GiveUp_(*route, delivery, tries, error, false);
            continue;
         }

         UpdateDelivery_(delivery.id, QueuePending, tries, next, error, false);
      }

      if (heldForNextPass > 0)
      {
         LOG_APPLICATION(Formatter::Format(_T("Inbound webhooks: {0} delivery(ies) were left for the next pass - {1} endpoint(s) gave no answer at all, or the pass reached its {2} second limit. Nothing has been lost; they are still due."),
            heldForNextPass, (int) unreachableRoutes.size(), (int) MaxPassSeconds));
      }

      return delivered;
   }

   int
   InboundWebhooks::PurgeOldDeliveries()
   {
      const int days = Configuration::Instance()->GetWebhookRetentionDays();

      if (days <= 0)
         return 0;

      const __int64 cutoff = (__int64) time(nullptr) - (__int64) days * 86400;

      // Delivered and bounced only: their copies are already gone. A kept row
      // holds a message the route asked to keep, and stays until somebody
      // retries or removes it.
      SQLCommand count(_T("select count(*) as oldcount from hm_inboundhookqueue where (iqstate = 2 or iqstate = 4) and iqcreated < @CUTOFF"));
      count.AddParameter("@CUTOFF", cutoff);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(count);
      if (!recordset || recordset->IsEOF())
         return 0;

      const int removable = recordset->GetLongValue("oldcount");
      if (removable <= 0)
         return 0;

      SQLCommand command(_T("delete from hm_inboundhookqueue where (iqstate = 2 or iqstate = 4) and iqcreated < @CUTOFF"));
      command.AddParameter("@CUTOFF", cutoff);

      if (!Application::Instance()->GetDBManager()->Execute(command))
         return 0;

      return removable;
   }

   int
   InboundWebhooks::EraseAddress(const String &address)
   {
      String lowered = address;
      lowered.Trim();
      lowered.MakeLower();

      if (lowered.IsEmpty() || lowered.Find(_T("@")) <= 0)
         return 0;

      int changed = 0;

      // What the address sent: each delivery, with the message it kept. A
      // sender is stored as the SMTP session gave it, and compared lower-cased.
      {
         std::vector<std::pair<__int64, String> > sent;

         SQLCommand command(_T("select iqid, iqfile from hm_inboundhookqueue where lower(iqsender) = @SENDER"));
         command.AddParameter("@SENDER", lowered);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return -1;

         while (!recordset->IsEOF())
         {
            sent.push_back(std::make_pair(recordset->GetInt64Value("iqid"), recordset->GetStringValue("iqfile")));
            recordset->MoveNext();
         }

         for (const std::pair<__int64, String> &row : sent)
         {
            SQLCommand remove(_T("delete from hm_inboundhookqueue where iqid = @ID"));
            remove.AddParameter("@ID", row.first);

            if (!Application::Instance()->GetDBManager()->Execute(remove))
               return -1;

            if (!row.second.IsEmpty())
               MessageStore::DiscardWorkingFile(FileOf_(row.second));

            changed++;
         }
      }

      // What it was sent. A route's recipients are addresses of the domain it
      // routes, so only that domain's routes' rows are read, and each list is
      // matched whole in memory - a match inside a longer address (ann@ in
      // joann@) is not one. The address goes from the list, and a delivery
      // left naming nobody goes with its message.
      const String domain = StringParser::ExtractDomain(lowered);

      std::vector<Route> routes;
      if (!ReadRoutes_(routes))
         return -1;

      for (const Route &route : routes)
      {
         if (DomainOf(route.address) != domain)
            continue;

         struct Rewrite
         {
            __int64 id = 0;
            String file;
            String recipients;
            String quiet;
         };

         std::vector<Rewrite> rewrites;

         SQLCommand command(_T("select iqid, iqfile, iqrecipients, iqquiet from hm_inboundhookqueue where iqhookid = @HOOKID"));
         command.AddParameter("@HOOKID", route.id);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return -1;

         while (!recordset->IsEOF())
         {
            bool found = false;
            Rewrite rewrite;
            rewrite.id = recordset->GetInt64Value("iqid");
            rewrite.file = recordset->GetStringValue("iqfile");

            for (const String &entry : StringParser::SplitString(recordset->GetStringValue("iqrecipients"), _T(",")))
            {
               if (entry.IsEmpty())
                  continue;

               if (entry.CompareNoCase(lowered) == 0)
               {
                  found = true;
                  continue;
               }

               if (!rewrite.recipients.IsEmpty())
                  rewrite.recipients += _T(",");
               rewrite.recipients += entry;
            }

            for (const String &entry : StringParser::SplitString(recordset->GetStringValue("iqquiet"), _T(",")))
            {
               if (entry.IsEmpty() || entry.CompareNoCase(lowered) == 0)
                  continue;

               if (!rewrite.quiet.IsEmpty())
                  rewrite.quiet += _T(",");
               rewrite.quiet += entry;
            }

            if (found)
               rewrites.push_back(rewrite);

            recordset->MoveNext();
         }

         for (const Rewrite &rewrite : rewrites)
         {
            bool done = false;

            if (rewrite.recipients.IsEmpty())
            {
               SQLCommand remove(_T("delete from hm_inboundhookqueue where iqid = @ID"));
               remove.AddParameter("@ID", rewrite.id);
               done = Application::Instance()->GetDBManager()->Execute(remove);

               if (done && !rewrite.file.IsEmpty())
                  MessageStore::DiscardWorkingFile(FileOf_(rewrite.file));
            }
            else
            {
               SQLCommand update(_T("update hm_inboundhookqueue set iqrecipients = @RECIPIENTS, iqquiet = @QUIET where iqid = @ID"));
               update.AddParameter("@RECIPIENTS", rewrite.recipients);
               update.AddParameter("@QUIET", rewrite.quiet);
               update.AddParameter("@ID", rewrite.id);
               done = Application::Instance()->GetDBManager()->Execute(update);
            }

            if (!done)
               return -1;

            changed++;
         }
      }

      return changed;
   }

   void
   InboundWebhooks::ScheduleDrain_()
   {
      {
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);

         // Wanted, whoever ends up running it: a pass that is running read its
         // rows before this one was queued, and posts a drain for it when it
         // ends (PassEnded_). At most one drain is on the queue at a time.
         drain_wanted_ = true;

         if (draining_ || passing_)
            return;

         draining_ = true;
      }

      // Off the delivery thread at once, so that an endpoint hears of a message
      // in seconds rather than at the next minute. A failure to post is not a
      // failure of anything: the minute's task finds the row.
      std::shared_ptr<WorkQueue> queue = Application::Instance()->GetAsyncWorkQueue();
      if (!queue)
      {
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);
         draining_ = false;
         return;
      }

      // TaskMayBlock: a drain posts to hosts outside this process, and such a
      // task is held below the pool's size, so that an endpoint that does not
      // answer can never take the threads SMTP's end of DATA is finished on. It
      // waits for no other task - a drain that finds a pass running returns at
      // once - which is the one thing WorkQueue asks of a blocking task.
      std::shared_ptr<InboundWebhookDrainTask> task = std::shared_ptr<InboundWebhookDrainTask>(new InboundWebhookDrainTask());
      queue->AddTask(task, WorkQueue::TaskMayBlock);
   }

   void
   InboundWebhooks::DrainStarting()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);
      draining_ = false;
   }

   InboundWebhookTask::InboundWebhookTask()
   {

   }

   InboundWebhookTask::~InboundWebhookTask()
   {

   }

   void
   InboundWebhookTask::DoWork()
   {
      // On the scheduler's thread, which it never holds for longer than a pass:
      // when a drain's pass is running this minute's is not made, and the
      // retention sweep is one count and at most one delete.
      InboundWebhooks::Instance()->RunDeliveries();
      InboundWebhooks::Instance()->PurgeOldDeliveries();
   }

   InboundWebhookDrainTask::InboundWebhookDrainTask()
      : Task(_T("Inbound webhook drain"))
   {
   }

   void
   InboundWebhookDrainTask::DoWork()
   {
      // Cleared before the pass is tried, so that a message queued from now on
      // posts a drain of its own - or, while a pass runs, is left to that
      // pass's end.
      InboundWebhooks::Instance()->DrainStarting();
      InboundWebhooks::Instance()->RunDeliveries();
   }
}
