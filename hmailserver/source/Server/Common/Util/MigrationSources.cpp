// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See MigrationSources.h.

#include "StdAfx.h"
#include "MigrationSources.h"

#include "DavSupport.h"
#include "HttpsClient.h"
#include "ICalendar.h"
#include "JsonDocument.h"
#include "VCard.h"
#include "Strings/Formatter.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const char *NsDav = "DAV:";
      const char *NsCardDav = "urn:ietf:params:xml:ns:carddav";
      const char *NsCalDav = "urn:ietf:params:xml:ns:caldav";

      // The three kinds, and everything each needs. The scopes are the ones each
      // provider documents for the protocol it is read with: Gmail's IMAP takes
      // only https://mail.google.com/; Google's CardDAV and CalDAV take their
      // own; Microsoft's IMAP takes IMAP.AccessAsUser.All on the Outlook
      // resource and Graph takes Contacts.Read and Calendars.Read on its own -
      // one refresh token, redeemed once per resource. openid and email are
      // what makes the sign-in say which address signed in (ReaderOAuth).
      const MigrationProvider Providers[] =
      {
         { 0, "imap", "IMAP server", "", "", 993,
           "", "", "",
           "",
           false, "", "" },
         { 1, "google", "Google Workspace", "google", "imap.gmail.com", 993,
           "https://mail.google.com/",
           "https://www.googleapis.com/auth/carddav",
           "https://www.googleapis.com/auth/calendar",
           "https://mail.google.com/ https://www.googleapis.com/auth/carddav https://www.googleapis.com/auth/calendar openid email",
           false,
           "https://www.googleapis.com/carddav/v1/principals/{user}/lists/default/",
           "https://apidata.googleusercontent.com/caldav/v2/{user}/events/" },
         { 2, "microsoft365", "Microsoft 365", "microsoft", "outlook.office365.com", 993,
           "https://outlook.office.com/IMAP.AccessAsUser.All offline_access",
           "https://graph.microsoft.com/Contacts.Read offline_access",
           "https://graph.microsoft.com/Calendars.Read offline_access",
           "https://outlook.office.com/IMAP.AccessAsUser.All https://graph.microsoft.com/Contacts.Read https://graph.microsoft.com/Calendars.Read offline_access openid email",
           true,
           "https://graph.microsoft.com/v1.0/me/contacts?$top=100",
           "https://graph.microsoft.com/v1.0/me/events?$top=50" }
      };

      // What one DAV answer may be, and one Graph page: an address book with
      // photos in it is large, and it is held whole to be read.
      const size_t MaxDavBytes = 64 * 1024 * 1024;
      const size_t MaxGraphPageBytes = 8 * 1024 * 1024;

      // Graph pages followed at most: fifty thousand contacts, twenty-five
      // thousand events.
      const int MaxGraphPages = 500;

      void Expect(bool condition, const char *message)
      {
         if (!condition)
            throw std::logic_error(message);
      }

      String Wide(const AnsiString &utf8)
      {
         return Dav::Wide(utf8);
      }

      AnsiString Base64(const AnsiString &bytes)
      {
         static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
         std::string out;
         const unsigned char *data = (const unsigned char *) bytes.c_str();
         const size_t length = (size_t) bytes.GetLength();

         size_t i = 0;
         for (; i + 2 < length; i += 3)
         {
            const unsigned int n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
            out += alphabet[(n >> 18) & 63];
            out += alphabet[(n >> 12) & 63];
            out += alphabet[(n >> 6) & 63];
            out += alphabet[n & 63];
         }

         if (length - i == 1)
         {
            const unsigned int n = data[i] << 16;
            out += alphabet[(n >> 18) & 63];
            out += alphabet[(n >> 12) & 63];
            out += "==";
         }
         else if (length - i == 2)
         {
            const unsigned int n = (data[i] << 16) | (data[i + 1] << 8);
            out += alphabet[(n >> 18) & 63];
            out += alphabet[(n >> 12) & 63];
            out += alphabet[(n >> 6) & 63];
            out += "=";
         }

         return AnsiString(out.c_str());
      }

      AnsiString HeaderValue(const AnsiString &headers, const char *name)
      {
         AnsiString lower = headers;
         lower.MakeLower();

         const AnsiString key = AnsiString("\r\n") + name + ":";
         const int at = lower.Find(key);
         if (at < 0)
            return "";

         const int start = at + key.GetLength();
         const int end = headers.Find("\r\n", start);
         AnsiString value = end >= 0 ? headers.Mid(start, end - start) : headers.Mid(start);
         value.Trim();
         return value;
      }

      AnsiString Authority(const AnsiString &host, const AnsiString &port)
      {
         AnsiString authority = host.Find(":") >= 0 ? "[" + host + "]" : host;
         return authority + ":" + port;
      }

      // Why a request got no answer at all, which decides what the phase does
      // next.
      enum Unanswered
      {
         UnansweredNetwork,       // lost, reset, timed out: another attempt may do better
         UnansweredUnreachable,   // nothing listens there, or the name does not resolve
         UnansweredPermanent,     // not a URL, plain http to another machine, too large, not HTTP
         UnansweredStopped        // the job was told to stop
      };

      // HttpsClient's sentences for a request that got no response
      // (HttpsClient.cpp), sorted.
      Unanswered Classify(const String &error)
      {
         AnsiString text = AnsiString(error);
         text.MakeLower();

         if (text.Find("does not resolve") >= 0)
            return UnansweredUnreachable;

         if (text.Find(" failed: ") >= 0)
         {
            // A connection the host refused outright - ECONNREFUSED, 111 on
            // Linux and 10061 on Windows - is a host with nothing listening.
            if (text.Find("refused") >= 0 || text.Find(":111]") >= 0 || text.Find(":10061]") >= 0)
               return UnansweredUnreachable;

            // A name nothing answers for: "No such host is known" on Windows,
            // "Host not found" from the resolver elsewhere.
            if (text.Find("no such host") >= 0 || text.Find("host not found") >= 0 || text.Find("asio.netdb") >= 0)
               return UnansweredUnreachable;
            return UnansweredNetwork;
         }

         // A connection dropped before the first byte of the answer.
         if (text.Find("no http headers") >= 0 || text.Find("sent no http response") >= 0)
            return UnansweredNetwork;

         return UnansweredPermanent;
      }

      // One request, following up to five redirects. A redirect to the same
      // host and port keeps the scheme it was asked with: a DAV server behind a
      // TLS-terminating proxy writes https:// into a redirect to its own
      // address, and the proxy is what is being spoken to. The credentials go
      // only to the origin they were given for (MigrationSources.h), and a
      // redirect from https to plain http is refused.
      bool Send(const AnsiString &method, const AnsiString &startUrl, const AnsiString &depth,
                const AnsiString &contentType, const AnsiString &body, const MigrationSources::Credentials &credentials,
                const AnsiString &accept, size_t maxBytes, const std::function<bool()> &stop,
                HttpsClient::Response &response, AnsiString &finalUrl, String &error, Unanswered &unanswered)
      {
         AnsiString url = startUrl;
         unanswered = UnansweredNetwork;

         for (int hop = 0; hop < 6; hop++)
         {
            if (stop && stop())
            {
               error = _T("The job was told to stop.");
               unanswered = UnansweredStopped;
               return false;
            }

            bool https = false;
            AnsiString host, port, path;
            if (!HttpsClient::ParseUrl(url, https, host, port, path))
            {
               error = _T("Not an http or https URL: ") + Wide(url);
               unanswered = UnansweredPermanent;
               return false;
            }

            std::vector<AnsiString> headers;
            const bool mayAuthorize = credentials.origin.IsEmpty() || MigrationSources::OriginOf(url) == credentials.origin;
            if (mayAuthorize && !credentials.bearer.IsEmpty())
               headers.push_back("Authorization: Bearer " + credentials.bearer);
            else if (mayAuthorize && !credentials.user.IsEmpty())
               headers.push_back("Authorization: Basic " + Base64(credentials.user + ":" + credentials.password));
            if (!depth.IsEmpty())
               headers.push_back("Depth: " + depth);
            if (!accept.IsEmpty())
               headers.push_back("Accept: " + accept);

            // Plain http is taken only to this machine (HttpsClient's rule), and a
            // DAV server on this machine reached over plain http is one behind a
            // TLS-terminating proxy on its plain side, which is told the scheme
            // the client came in on as the proxy would tell it. DAV refuses Basic
            // over plain http otherwise, with the reason (WebServicesServer's
            // RequestArrivedOverHttps_, which believes this header from loopback).
            if (!https && HttpsClient::IsLoopbackHost(host))
               headers.push_back("X-Forwarded-Proto: https");

            response = HttpsClient::Response();
            if (!HttpsClient::Request(method, url, headers, contentType, body, response, error, 60, maxBytes))
            {
               unanswered = Classify(error);
               return false;
            }

            finalUrl = url;

            const int status = response.status_code;
            if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308)
               return true;

            const AnsiString location = HeaderValue(response.headers, "location");
            if (location.IsEmpty())
               return true;

            AnsiString next = MigrationSources::ResolveUrl(url, location);

            bool nextHttps = false;
            AnsiString nextHost, nextPort, nextPath;
            if (HttpsClient::ParseUrl(next, nextHttps, nextHost, nextPort, nextPath) &&
                nextHost.CompareNoCase(host) == 0 && nextPort == port && nextHttps != https)
            {
               next = AnsiString(https ? "https://" : "http://") + Authority(host, port) + nextPath;
            }

            // What was asked over TLS is not asked again in the clear - least of
            // all of this machine, whose DAV believes a scheme it is told.
            bool finalHttps = false;
            AnsiString finalHost, finalPort, finalPath;
            if (https && HttpsClient::ParseUrl(next, finalHttps, finalHost, finalPort, finalPath) && !finalHttps)
            {
               error = _T("The source redirected from https to plain http: ") + Wide(next);
               unanswered = UnansweredPermanent;
               return false;
            }

            url = next;
         }

         error = _T("The source redirected more than five times.");
         unanswered = UnansweredPermanent;
         return false;
      }

      // What a request that got no answer means for the phase: a stop and a
      // lost connection are tried again; a host with nothing on it at the first
      // request is a source without DAV, and the phase is skipped; anything no
      // attempt will change skips it too.
      void NoAnswer(Unanswered unanswered, bool first, const String &what, const AnsiString &url, const String &error,
                    String &skipReason, bool &transient)
      {
         if (unanswered == UnansweredStopped || unanswered == UnansweredNetwork || (unanswered == UnansweredUnreachable && !first))
         {
            transient = true;
            return;
         }

         skipReason = unanswered == UnansweredUnreachable
            ? Formatter::Format(_T("the source's {0} could not be reached at {1}: {2}"), what, Wide(url), error)
            : Formatter::Format(_T("the source's {0} could not be read at {1}: {2}"), what, Wide(url), error);
      }

      enum Verdict
      {
         VerdictOk,
         VerdictSkip,
         VerdictTransient
      };

      // What an HTTP status means for a phase: the answer, a reason to skip the
      // phase, or a failure another attempt might not meet.
      Verdict Judge(int status, const String &what, String &reason)
      {
         if (status == 207 || status == 200)
            return VerdictOk;

         if (status == 401 || status == 403)
         {
            reason = Formatter::Format(_T("the source refused the credentials for {0} (HTTP {1})"), what, status);
            return VerdictSkip;
         }

         if (status == 429 || status >= 500)
         {
            reason = Formatter::Format(_T("the source answered HTTP {0} for {1}"), status, what);
            return VerdictTransient;
         }

         reason = Formatter::Format(_T("the source has no {0} there (HTTP {1})"), what, status);
         return VerdictSkip;
      }

      // A verdict other than OK applied to the phase.
      bool Refused(Verdict verdict, const String &reason, String &skipReason, String &error, bool &transient)
      {
         if (verdict == VerdictSkip)
         {
            skipReason = reason;
            return true;
         }
         if (verdict == VerdictTransient)
         {
            error = reason;
            transient = true;
            return true;
         }
         return false;
      }

      const Dav::XmlElement *Descendant(const Dav::XmlElement &element, const char *ns, const char *name)
      {
         for (size_t i = 0; i < element.children.size(); i++)
         {
            if (element.children[i].Is(ns, name))
               return &element.children[i];

            const Dav::XmlElement *below = Descendant(element.children[i], ns, name);
            if (below)
               return below;
         }

         return nullptr;
      }

      AnsiString PropfindBody(const AnsiString &props)
      {
         return AnsiString("<?xml version=\"1.0\" encoding=\"utf-8\"?>")
              + "<D:propfind xmlns:D=\"DAV:\" xmlns:A=\"urn:ietf:params:xml:ns:carddav\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">"
              + "<D:prop>" + props + "</D:prop></D:propfind>";
      }

      // PROPFIND for one href-valued property: the href, resolved. A multistatus
      // that does not name it answers an empty string. first is the request
      // that says whether the host has DAV at all.
      bool FindHref(const AnsiString &url, bool first, const char *ns, const char *property, const AnsiString &props,
                    const MigrationSources::Credentials &credentials, const String &what, const std::function<bool()> &stop,
                    AnsiString &href, AnsiString &finalUrl, String &skipReason, String &error, bool &transient)
      {
         HttpsClient::Response response;
         Unanswered unanswered = UnansweredNetwork;
         if (!Send("PROPFIND", url, "0", "application/xml; charset=utf-8", PropfindBody(props), credentials,
                   "application/xml, text/xml", 1024 * 1024, stop, response, finalUrl, error, unanswered))
         {
            NoAnswer(unanswered, first, what, url, error, skipReason, transient);
            return false;
         }

         String reason;
         if (Refused(Judge(response.status_code, what, reason), reason, skipReason, error, transient))
            return false;

         Dav::XmlElement root;
         AnsiString problem;
         Dav::XmlReader reader(response.body);
         if (!reader.Read(root, problem))
         {
            skipReason = _T("the source's answer for ") + what + _T(" is not XML: ") + Wide(problem);
            return false;
         }

         href = "";
         if (const Dav::XmlElement *found = Descendant(root, ns, property))
         {
            if (const Dav::XmlElement *inner = found->Child(NsDav, "href"))
               href = Dav::Trimmed(inner->text);
         }

         if (!href.IsEmpty())
            href = MigrationSources::ResolveUrl(finalUrl, href);

         return true;
      }

      AnsiString LastSegment(const AnsiString &href)
      {
         AnsiString path = href;
         while (path.EndsWith("/"))
            path = path.Mid(0, path.GetLength() - 1);

         const int slash = path.ReverseFind('/');
         return Dav::PercentDecode(slash >= 0 ? path.Mid(slash + 1) : path);
      }

      // The collections one level below a home: every response whose
      // resourcetype names an address book (or a calendar).
      bool ListCollections(const AnsiString &home, bool calendar, const MigrationSources::Credentials &credentials,
                           const std::function<bool()> &stop,
                           std::vector<MigrationSources::Collection> &collections, String &skipReason, String &error, bool &transient)
      {
         const String what = calendar ? _T("CalDAV") : _T("CardDAV");

         HttpsClient::Response response;
         AnsiString finalUrl;
         Unanswered unanswered = UnansweredNetwork;
         if (!Send("PROPFIND", home, "1", "application/xml; charset=utf-8", PropfindBody("<D:resourcetype/><D:displayname/>"),
                   credentials, "application/xml, text/xml", 4 * 1024 * 1024, stop, response, finalUrl, error, unanswered))
         {
            NoAnswer(unanswered, false, what, home, error, skipReason, transient);
            return false;
         }

         String reason;
         if (Refused(Judge(response.status_code, what, reason), reason, skipReason, error, transient))
            return false;

         Dav::XmlElement root;
         AnsiString problem;
         Dav::XmlReader reader(response.body);
         if (!reader.Read(root, problem))
         {
            skipReason = _T("the source's list of collections is not XML: ") + Wide(problem);
            return false;
         }

         for (size_t i = 0; i < root.children.size(); i++)
         {
            const Dav::XmlElement &item = root.children[i];
            if (!item.Is(NsDav, "response"))
               continue;

            const Dav::XmlElement *href = item.Child(NsDav, "href");
            const Dav::XmlElement *type = Descendant(item, NsDav, "resourcetype");
            if (!href || !type)
               continue;

            const bool isCollection = calendar ? type->Child(NsCalDav, "calendar") != nullptr
                                               : type->Child(NsCardDav, "addressbook") != nullptr;
            if (!isCollection)
               continue;

            MigrationSources::Collection collection;
            collection.href = MigrationSources::ResolveUrl(finalUrl, Dav::Trimmed(href->text));
            collection.segment = LastSegment(collection.href);

            // What a source generates rather than what the person keeps: this
            // server's own organisation directory and the books and calendars
            // other people share with the person (whose owners' own jobs copy
            // them), and Nextcloud's system address book. Copying them would put
            // somebody else's data in this person's own collections.
            if (collection.segment.CompareNoCase("directory") == 0 || collection.segment.StartsWith("shared-") ||
                collection.segment.CompareNoCase("z-server-generated--system") == 0)
               continue;

            if (const Dav::XmlElement *name = Descendant(item, NsDav, "displayname"))
               collection.displayName = Dav::Trimmed(name->text);
            if (collection.displayName.IsEmpty())
               collection.displayName = collection.segment;

            collections.push_back(collection);
         }

         return true;
      }

      // The items of one collection: a query for all of them; where the source
      // does not answer the query, the listing and a multiget of fifty at a time.
      bool ReadCollection(MigrationSources::Collection &collection, bool calendar, const MigrationSources::Credentials &credentials,
                          const std::function<bool()> &stop, String &skipReason, String &error, bool &transient)
      {
         const String what = calendar ? _T("CalDAV") : _T("CardDAV");
         const char *dataElement = calendar ? "calendar-data" : "address-data";

         const AnsiString query = calendar
            ? AnsiString("<?xml version=\"1.0\" encoding=\"utf-8\"?><C:calendar-query xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">"
                         "<D:prop><D:getetag/><C:calendar-data/></D:prop><C:filter><C:comp-filter name=\"VCALENDAR\"/></C:filter></C:calendar-query>")
            : AnsiString("<?xml version=\"1.0\" encoding=\"utf-8\"?><A:addressbook-query xmlns:D=\"DAV:\" xmlns:A=\"urn:ietf:params:xml:ns:carddav\">"
                         "<D:prop><D:getetag/><A:address-data/></D:prop><A:filter/></A:addressbook-query>");

         HttpsClient::Response response;
         AnsiString finalUrl;
         Unanswered unanswered = UnansweredNetwork;
         if (!Send("REPORT", collection.href, "1", "application/xml; charset=utf-8", query, credentials,
                   "application/xml, text/xml", MaxDavBytes, stop, response, finalUrl, error, unanswered))
         {
            NoAnswer(unanswered, false, what, collection.href, error, skipReason, transient);
            return false;
         }

         if (response.status_code == 207)
         {
            MigrationSources::ExtractItems(response.body, dataElement, collection.items);
            return true;
         }

         String reason;
         if (Judge(response.status_code, what, reason) == VerdictTransient)
         {
            error = reason;
            transient = true;
            return false;
         }

         // The query refused: list the members, then fetch them by name.
         HttpsClient::Response listing;
         if (!Send("PROPFIND", collection.href, "1", "application/xml; charset=utf-8", PropfindBody("<D:getetag/>"),
                   credentials, "application/xml, text/xml", 16 * 1024 * 1024, stop, listing, finalUrl, error, unanswered))
         {
            NoAnswer(unanswered, false, what, collection.href, error, skipReason, transient);
            return false;
         }

         if (Refused(Judge(listing.status_code, what, reason), reason, skipReason, error, transient))
            return false;

         std::vector<AnsiString> hrefs;
         {
            std::vector<MigrationSources::Item> members;
            MigrationSources::ExtractItems(listing.body, "getetag", members);
            for (const MigrationSources::Item &member : members)
            {
               if (!member.href.EndsWith("/"))
                  hrefs.push_back(member.href);
            }
         }

         for (size_t at = 0; at < hrefs.size(); at += 50)
         {
            AnsiString multiget = calendar
               ? AnsiString("<?xml version=\"1.0\" encoding=\"utf-8\"?><C:calendar-multiget xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\"><D:prop><D:getetag/><C:calendar-data/></D:prop>")
               : AnsiString("<?xml version=\"1.0\" encoding=\"utf-8\"?><A:addressbook-multiget xmlns:D=\"DAV:\" xmlns:A=\"urn:ietf:params:xml:ns:carddav\"><D:prop><D:getetag/><A:address-data/></D:prop>");

            for (size_t i = at; i < hrefs.size() && i < at + 50; i++)
               multiget += "<D:href>" + Dav::XmlEscape(hrefs[i]) + "</D:href>";

            multiget += calendar ? "</C:calendar-multiget>" : "</A:addressbook-multiget>";

            HttpsClient::Response part;
            if (!Send("REPORT", collection.href, "1", "application/xml; charset=utf-8", multiget, credentials,
                      "application/xml, text/xml", MaxDavBytes, stop, part, finalUrl, error, unanswered))
            {
               NoAnswer(unanswered, false, what, collection.href, error, skipReason, transient);
               return false;
            }

            // Judged as the query is: a refusal no attempt changes skips the
            // phase, a busy source is tried again.
            if (Refused(Judge(part.status_code, what, reason), reason, skipReason, error, transient))
               return false;

            MigrationSources::ExtractItems(part.body, dataElement, collection.items);
         }

         return true;
      }

      // Graph, a page at a time. The next page is taken only from Graph itself,
      // whose token it is.
      bool ReadGraph(const AnsiString &first, const AnsiString &token, const String &what, bool events,
                     const std::function<bool()> &stop,
                     std::vector<MigrationSources::Collection> &collections, String &skipReason, String &error, bool &transient)
      {
         MigrationSources::Collection collection;
         collection.href = first;
         collection.segment = events ? "calendar" : "contacts";
         collection.displayName = events ? "Calendar" : "Contacts";

         MigrationSources::Credentials credentials;
         credentials.bearer = token;
         credentials.origin = MigrationSources::OriginOf(first);

         AnsiString url = first;
         for (int page = 0; page < MaxGraphPages && !url.IsEmpty(); page++)
         {
            HttpsClient::Response response;
            AnsiString finalUrl;
            Unanswered unanswered = UnansweredNetwork;
            if (!Send("GET", url, "", "", "", credentials, "application/json", MaxGraphPageBytes, stop, response, finalUrl, error, unanswered))
            {
               NoAnswer(unanswered, false, what, url, error, skipReason, transient);
               return false;
            }

            String reason;
            if (Refused(Judge(response.status_code, what, reason), reason, skipReason, error, transient))
               return false;

            JsonValue answer;
            std::string parseError;
            std::string body(response.body.c_str(), (size_t) response.body.GetLength());
            if (!JsonValue::Parse(body, answer, parseError) || !answer.IsObject())
            {
               skipReason = _T("Microsoft Graph answered with something that is not JSON");
               return false;
            }

            if (const JsonValue *value = answer.Get("value"))
            {
               for (const JsonValue &entry : value->Items())
               {
                  MigrationSources::Item item;
                  item.href = AnsiString(entry.GetString("id").c_str());
                  item.data = events ? MigrationSources::GraphEventToICalendar(entry) : MigrationSources::GraphContactToVCard(entry);
                  if (!item.data.IsEmpty())
                     collection.items.push_back(item);
               }
            }

            url = AnsiString(answer.GetString("@odata.nextLink").c_str());
            if (!url.IsEmpty() && MigrationSources::OriginOf(url) != credentials.origin)
            {
               skipReason = _T("Microsoft Graph named a next page on another host, which is not followed with the token: ") + Wide(url.Mid(0, 200));
               return false;
            }
         }

         collections.push_back(collection);
         return true;
      }

      AnsiString Line(const AnsiString &name, const AnsiString &value)
      {
         return VCard::Fold(name + ":" + value) + "\r\n";
      }

      AnsiString CalendarLine(const AnsiString &name, const AnsiString &value)
      {
         return ICalendar::Fold(name + ":" + value) + "\r\n";
      }

      AnsiString Utf8Of(const std::string &value)
      {
         return AnsiString(value.c_str());
      }

      // A Graph address object as the seven fields of a vCard ADR.
      AnsiString PostalAddress(const JsonValue *address)
      {
         if (!address || !address->IsObject())
            return "";

         const AnsiString street = VCard::Escape(Utf8Of(address->GetString("street")));
         const AnsiString city = VCard::Escape(Utf8Of(address->GetString("city")));
         const AnsiString state = VCard::Escape(Utf8Of(address->GetString("state")));
         const AnsiString code = VCard::Escape(Utf8Of(address->GetString("postalCode")));
         const AnsiString country = VCard::Escape(Utf8Of(address->GetString("countryOrRegion")));
         if (street.IsEmpty() && city.IsEmpty() && state.IsEmpty() && code.IsEmpty() && country.IsEmpty())
            return "";

         return ";;" + street + ";" + city + ";" + state + ";" + code + ";" + country;
      }

      AnsiString Weekday(const std::string &day)
      {
         if (day == "monday") return "MO";
         if (day == "tuesday") return "TU";
         if (day == "wednesday") return "WE";
         if (day == "thursday") return "TH";
         if (day == "friday") return "FR";
         if (day == "saturday") return "SA";
         if (day == "sunday") return "SU";
         return "";
      }

      AnsiString Days(const JsonValue *days)
      {
         AnsiString list;
         if (!days)
            return list;

         for (const JsonValue &day : days->Items())
         {
            const AnsiString word = Weekday(day.AsString());
            if (word.IsEmpty())
               continue;
            if (!list.IsEmpty())
               list += ",";
            list += word;
         }

         return list;
      }

      // A Graph dateTimeTimeZone (in UTC, which is how Graph answers by
      // default) as DTSTART or DTEND: in the event's own zone when the
      // iCalendar module knows it - so a weekly meeting at 09:00 stays at 09:00
      // across a change to summer time - else in UTC.
      AnsiString TimeProperty(const char *name, const JsonValue *when, const AnsiString &zone, bool allDay)
      {
         if (!when)
            return "";

         const AnsiString utc = MigrationSources::GraphTimeToUtc(Utf8Of(when->GetString("dateTime")));
         if (utc.IsEmpty())
            return "";

         __int64 instant = 0;
         if (!ICalendar::ParseUtc(utc, instant))
            return "";

         int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
         const bool local = !zone.IsEmpty() && ICalendar::KnowsTimeZone(zone) &&
                            ICalendar::InstantToLocal(zone, instant, year, month, day, hour, minute, second);

         if (allDay)
         {
            AnsiString date;
            if (local)
               date.Format("%04d%02d%02d", year, month, day);
            else
               date = utc.Mid(0, 8);
            return CalendarLine(AnsiString(name) + ";VALUE=DATE", date);
         }

         if (local)
         {
            AnsiString wall;
            wall.Format("%04d%02d%02dT%02d%02d%02d", year, month, day, hour, minute, second);
            return CalendarLine(AnsiString(name) + ";TZID=" + zone, wall);
         }

         return CalendarLine(name, utc);
      }

      // A name as an iCalendar parameter value: quoted, so it may hold a comma
      // or a colon, and without the one character a quoted value cannot hold.
      AnsiString ParamValue(const AnsiString &value)
      {
         AnsiString safe;
         for (int i = 0; i < value.GetLength(); i++)
         {
            const char c = value.GetAt(i);
            if (c == '"')
               safe += '\'';
            else if ((unsigned char) c >= 0x20 || c == '\t')
               safe += c;
         }
         return "\"" + safe + "\"";
      }

      AnsiString PartStat(const std::string &response)
      {
         if (response == "accepted" || response == "organizer") return "ACCEPTED";
         if (response == "declined") return "DECLINED";
         if (response == "tentativelyAccepted") return "TENTATIVE";
         return "NEEDS-ACTION";
      }

      // Tags out of an HTML body, for DESCRIPTION: a calendar's description is
      // text, and a client shows it as such.
      AnsiString TextOfHtml(const AnsiString &html)
      {
         AnsiString text;
         bool inTag = false;
         for (int i = 0; i < html.GetLength(); i++)
         {
            const char c = html.GetAt(i);
            if (c == '<')
               inTag = true;
            else if (c == '>')
               inTag = false;
            else if (!inTag)
               text += c;
         }
         return Dav::Trimmed(MigrationSources::DecodeXmlText(text));
      }

      void AppendUtf8(std::string &out, unsigned int codePoint)
      {
         if (codePoint < 0x80)
            out += (char) codePoint;
         else if (codePoint < 0x800)
         {
            out += (char) (0xC0 | (codePoint >> 6));
            out += (char) (0x80 | (codePoint & 0x3F));
         }
         else if (codePoint < 0x10000)
         {
            out += (char) (0xE0 | (codePoint >> 12));
            out += (char) (0x80 | ((codePoint >> 6) & 0x3F));
            out += (char) (0x80 | (codePoint & 0x3F));
         }
         else if (codePoint < 0x110000)
         {
            out += (char) (0xF0 | (codePoint >> 18));
            out += (char) (0x80 | ((codePoint >> 12) & 0x3F));
            out += (char) (0x80 | ((codePoint >> 6) & 0x3F));
            out += (char) (0x80 | (codePoint & 0x3F));
         }
      }

      bool IsSpace(char c)
      {
         return c == ' ' || c == '\t' || c == '\r' || c == '\n';
      }

      // The next element named `name` (its local name, whatever the prefix) at
      // or after `from`: where its content starts and ends, and where the
      // element ends. A tolerant scanner rather than the DAV reader, because a
      // multistatus of every card of an address book is larger than that
      // reader's element ceiling is meant to allow.
      bool NextElement(const std::string &xml, size_t from, const char *name, size_t &contentStart, size_t &contentEnd, size_t &after)
      {
         size_t at = from;
         while ((at = xml.find('<', at)) != std::string::npos)
         {
            if (xml.compare(at, 4, "<!--") == 0)
            {
               const size_t end = xml.find("-->", at);
               if (end == std::string::npos)
                  return false;
               at = end + 3;
               continue;
            }

            if (xml.compare(at, 9, "<![CDATA[") == 0)
            {
               const size_t end = xml.find("]]>", at);
               if (end == std::string::npos)
                  return false;
               at = end + 3;
               continue;
            }

            if (at + 1 < xml.size() && (xml[at + 1] == '/' || xml[at + 1] == '?' || xml[at + 1] == '!'))
            {
               at++;
               continue;
            }

            size_t nameEnd = at + 1;
            while (nameEnd < xml.size() && !IsSpace(xml[nameEnd]) && xml[nameEnd] != '>' && xml[nameEnd] != '/')
               nameEnd++;

            const std::string qualified = xml.substr(at + 1, nameEnd - at - 1);
            const size_t colon = qualified.find(':');
            const std::string local = colon == std::string::npos ? qualified : qualified.substr(colon + 1);

            const size_t tagEnd = xml.find('>', nameEnd);
            if (tagEnd == std::string::npos)
               return false;

            if (local != name)
            {
               at = tagEnd + 1;
               continue;
            }

            if (xml[tagEnd - 1] == '/')
            {
               contentStart = contentEnd = after = tagEnd + 1;
               return true;
            }

            contentStart = tagEnd + 1;
            const std::string closing = "</" + qualified;
            size_t search = contentStart;
            for (;;)
            {
               const size_t close = xml.find(closing, search);
               if (close == std::string::npos)
                  return false;

               size_t k = close + closing.size();
               while (k < xml.size() && IsSpace(xml[k]))
                  k++;

               if (k < xml.size() && xml[k] == '>')
               {
                  contentEnd = close;
                  after = k + 1;
                  return true;
               }

               search = close + 1;
            }
         }

         return false;
      }
   }

   const MigrationProvider *
   MigrationSources::ByKind(int kind)
   {
      for (size_t i = 0; i < sizeof(Providers) / sizeof(Providers[0]); i++)
      {
         if (Providers[i].kind == kind)
            return &Providers[i];
      }
      return nullptr;
   }

   const MigrationProvider *
   MigrationSources::ByWord(const AnsiString &word)
   {
      for (size_t i = 0; i < sizeof(Providers) / sizeof(Providers[0]); i++)
      {
         if (word.CompareNoCase(Providers[i].word) == 0)
            return &Providers[i];
      }
      return nullptr;
   }

   AnsiString
   MigrationSources::DavStart(const MigrationProvider &provider, const AnsiString &host, const AnsiString &davUrl,
                              const AnsiString &user, bool calendar)
   {
      const char *own = calendar ? provider.calendarsUrl : provider.contactsUrl;
      if (own && *own)
      {
         AnsiString url = own;
         url.Replace("{user}", Dav::PercentEncodeSegment(user));
         return url;
      }

      AnsiString base = davUrl;
      base.Trim();
      if (base.IsEmpty())
         base = "https://" + host + "/";
      while (base.EndsWith("/"))
         base = base.Mid(0, base.GetLength() - 1);

      return base + (calendar ? "/.well-known/caldav" : "/.well-known/carddav");
   }

   bool
   MigrationSources::ReadDav(const AnsiString &start, bool calendar, const Credentials &credentials,
                             std::vector<Collection> &collections, String &skipReason, String &error, bool &transient,
                             const std::function<bool()> &stopRequested)
   {
      collections.clear();
      transient = false;

      const String what = calendar ? _T("CalDAV") : _T("CardDAV");

      // The credentials are for where the reader begins, and for nowhere else.
      Credentials scoped = credentials;
      scoped.origin = OriginOf(start);

      if (start.Find("/.well-known/") < 0)
      {
         // A collection's own URL: read as it is.
         Collection collection;
         collection.href = start;
         collection.segment = calendar ? "calendar" : "contacts";
         collection.displayName = calendar ? "Calendar" : "Contacts";
         if (!ReadCollection(collection, calendar, scoped, stopRequested, skipReason, error, transient))
            return false;
         collections.push_back(collection);
         return true;
      }

      // RFC 6764: the well-known URI, then who am I, then where are my
      // collections. The first request is the one that says whether the host
      // has DAV at all: nothing listening there, a name that does not resolve
      // or a 404 skips the phase; a host that is there and busy - a timeout, a
      // 429, a 5xx - is tried again.
      AnsiString principal;
      AnsiString finalUrl;
      if (!FindHref(start, true, NsDav, "current-user-principal", "<D:current-user-principal/>", scoped, what, stopRequested,
                    principal, finalUrl, skipReason, error, transient))
         return false;

      if (principal.IsEmpty())
         principal = finalUrl;

      AnsiString home;
      const char *homeProperty = calendar ? "calendar-home-set" : "addressbook-home-set";
      const AnsiString homeProps = calendar ? "<C:calendar-home-set/>" : "<A:addressbook-home-set/>";
      if (!FindHref(principal, false, calendar ? NsCalDav : NsCardDav, homeProperty, homeProps, scoped, what, stopRequested,
                    home, finalUrl, skipReason, error, transient))
         return false;

      if (home.IsEmpty())
      {
         skipReason = Formatter::Format(_T("the source's {0} names no home for the account"), what);
         return false;
      }

      if (!ListCollections(home, calendar, scoped, stopRequested, collections, skipReason, error, transient))
         return false;

      for (Collection &collection : collections)
      {
         if (!ReadCollection(collection, calendar, scoped, stopRequested, skipReason, error, transient))
            return false;
      }

      return true;
   }

   bool
   MigrationSources::ReadGraphContacts(const AnsiString &token, std::vector<Collection> &collections,
                                       String &skipReason, String &error, bool &transient,
                                       const std::function<bool()> &stopRequested)
   {
      collections.clear();
      transient = false;
      return ReadGraph(ByKind(2)->contactsUrl, token, _T("Microsoft Graph contacts"), false, stopRequested, collections, skipReason, error, transient);
   }

   bool
   MigrationSources::ReadGraphEvents(const AnsiString &token, std::vector<Collection> &collections,
                                     String &skipReason, String &error, bool &transient,
                                     const std::function<bool()> &stopRequested)
   {
      collections.clear();
      transient = false;
      return ReadGraph(ByKind(2)->calendarsUrl, token, _T("Microsoft Graph events"), true, stopRequested, collections, skipReason, error, transient);
   }

   AnsiString
   MigrationSources::ResolveUrl(const AnsiString &base, const AnsiString &reference)
   {
      if (reference.StartsWith("http://") || reference.StartsWith("https://"))
         return reference;

      bool https = false;
      AnsiString host, port, path;
      if (!HttpsClient::ParseUrl(base, https, host, port, path))
         return reference;

      const AnsiString scheme = https ? "https:" : "http:";
      if (reference.StartsWith("//"))
         return scheme + reference;

      const AnsiString origin = scheme + "//" + Authority(host, port);
      if (reference.StartsWith("/"))
         return origin + reference;

      // Relative to the base's directory.
      AnsiString directory = path;
      const int query = directory.Find("?");
      if (query >= 0)
         directory = directory.Mid(0, query);
      const int slash = directory.ReverseFind('/');
      directory = slash >= 0 ? directory.Mid(0, slash + 1) : AnsiString("/");
      return origin + directory + reference;
   }

   AnsiString
   MigrationSources::OriginOf(const AnsiString &url)
   {
      bool https = false;
      AnsiString host, port, path;
      if (!HttpsClient::ParseUrl(url, https, host, port, path))
         return "";

      host.MakeLower();
      return AnsiString(https ? "https://" : "http://") + Authority(host, port);
   }

   AnsiString
   MigrationSources::DecodeXmlText(const AnsiString &text)
   {
      const std::string in(text.c_str(), (size_t) text.GetLength());
      std::string out;
      out.reserve(in.size());

      size_t i = 0;
      while (i < in.size())
      {
         if (in.compare(i, 9, "<![CDATA[") == 0)
         {
            const size_t end = in.find("]]>", i + 9);
            if (end == std::string::npos)
            {
               out.append(in, i + 9, std::string::npos);
               break;
            }
            out.append(in, i + 9, end - i - 9);
            i = end + 3;
            continue;
         }

         if (in[i] == '&')
         {
            const size_t semicolon = in.find(';', i);
            if (semicolon != std::string::npos && semicolon - i <= 10)
            {
               const std::string entity = in.substr(i + 1, semicolon - i - 1);
               bool known = true;
               if (entity == "lt") out += '<';
               else if (entity == "gt") out += '>';
               else if (entity == "amp") out += '&';
               else if (entity == "quot") out += '"';
               else if (entity == "apos") out += '\'';
               else if (entity.size() > 1 && entity[0] == '#')
               {
                  const bool hex = entity[1] == 'x' || entity[1] == 'X';
                  const unsigned long codePoint = strtoul(entity.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10);
                  if (codePoint > 0 && codePoint < 0x110000)
                     AppendUtf8(out, (unsigned int) codePoint);
                  else
                     known = false;
               }
               else
                  known = false;

               if (known)
               {
                  i = semicolon + 1;
                  continue;
               }
            }
         }

         out += in[i];
         i++;
      }

      return AnsiString(out.c_str());
   }

   bool
   MigrationSources::ExtractItems(const AnsiString &multistatus, const char *dataElement, std::vector<Item> &items)
   {
      const std::string xml(multistatus.c_str(), (size_t) multistatus.GetLength());

      size_t position = 0;
      size_t contentStart = 0, contentEnd = 0, after = 0;
      bool any = false;
      while (NextElement(xml, position, "response", contentStart, contentEnd, after))
      {
         any = true;
         const std::string response = xml.substr(contentStart, contentEnd - contentStart);
         position = after;

         Item item;
         size_t s = 0, e = 0, a = 0;
         if (NextElement(response, 0, "href", s, e, a))
            item.href = Dav::Trimmed(DecodeXmlText(AnsiString(response.substr(s, e - s).c_str())));

         if (NextElement(response, 0, dataElement, s, e, a))
            item.data = DecodeXmlText(AnsiString(response.substr(s, e - s).c_str()));

         // The listing for the fallback asks for getetag, and wants the href of
         // every member whatever its etag holds.
         if (!item.data.IsEmpty() || strcmp(dataElement, "getetag") == 0)
            items.push_back(item);
      }

      return any;
   }

   AnsiString
   MigrationSources::GraphTimeToUtc(const AnsiString &graphDateTime)
   {
      // "2026-09-24T10:00:00.0000000" -> "20260924T100000Z"
      if (graphDateTime.GetLength() < 19 || graphDateTime.GetAt(4) != '-' || graphDateTime.GetAt(10) != 'T')
         return "";

      AnsiString utc;
      utc += graphDateTime.Mid(0, 4);
      utc += graphDateTime.Mid(5, 2);
      utc += graphDateTime.Mid(8, 2);
      utc += "T";
      utc += graphDateTime.Mid(11, 2);
      utc += graphDateTime.Mid(14, 2);
      utc += graphDateTime.Mid(17, 2);
      utc += "Z";

      for (int i = 0; i < utc.GetLength(); i++)
      {
         const char c = utc.GetAt(i);
         if (i != 8 && i != 15 && (c < '0' || c > '9'))
            return "";
      }

      return utc;
   }

   AnsiString
   MigrationSources::RecurrenceRule(const JsonValue &recurrence)
   {
      const JsonValue *pattern = recurrence.Get("pattern");
      const JsonValue *range = recurrence.Get("range");
      if (!pattern || !pattern->IsObject())
         return "";

      const std::string type = pattern->GetString("type");
      __int64 interval = pattern->GetInt64("interval", 1);
      if (interval < 1)
         interval = 1;

      AnsiString rule;
      AnsiString setPosition;
      const std::string index = pattern->GetString("index");
      if (index == "first") setPosition = "1";
      else if (index == "second") setPosition = "2";
      else if (index == "third") setPosition = "3";
      else if (index == "fourth") setPosition = "4";
      else if (index == "last") setPosition = "-1";

      AnsiString number;
      if (type == "daily")
         rule = "FREQ=DAILY";
      else if (type == "weekly")
      {
         rule = "FREQ=WEEKLY";
         const AnsiString days = Days(pattern->Get("daysOfWeek"));
         if (!days.IsEmpty())
            rule += ";BYDAY=" + days;
         const AnsiString start = Weekday(pattern->GetString("firstDayOfWeek"));
         if (!start.IsEmpty())
            rule += ";WKST=" + start;
      }
      else if (type == "absoluteMonthly")
      {
         number.Format("%I64d", pattern->GetInt64("dayOfMonth", 1));
         rule = "FREQ=MONTHLY;BYMONTHDAY=" + number;
      }
      else if (type == "relativeMonthly")
      {
         rule = "FREQ=MONTHLY;BYDAY=" + Days(pattern->Get("daysOfWeek"));
         if (!setPosition.IsEmpty())
            rule += ";BYSETPOS=" + setPosition;
      }
      else if (type == "absoluteYearly")
      {
         AnsiString month;
         month.Format("%I64d", pattern->GetInt64("month", 1));
         number.Format("%I64d", pattern->GetInt64("dayOfMonth", 1));
         rule = "FREQ=YEARLY;BYMONTH=" + month + ";BYMONTHDAY=" + number;
      }
      else if (type == "relativeYearly")
      {
         AnsiString month;
         month.Format("%I64d", pattern->GetInt64("month", 1));
         rule = "FREQ=YEARLY;BYMONTH=" + month + ";BYDAY=" + Days(pattern->Get("daysOfWeek"));
         if (!setPosition.IsEmpty())
            rule += ";BYSETPOS=" + setPosition;
      }
      else
         return "";

      if (interval > 1)
      {
         number.Format("%I64d", interval);
         rule += ";INTERVAL=" + number;
      }

      if (range && range->IsObject())
      {
         const std::string rangeType = range->GetString("type");
         if (rangeType == "numbered")
         {
            number.Format("%I64d", range->GetInt64("numberOfOccurrences", 1));
            rule += ";COUNT=" + number;
         }
         else if (rangeType == "endDate")
         {
            // "2026-12-31" -> the end of that day, in UTC.
            const AnsiString end = Utf8Of(range->GetString("endDate"));
            if (end.GetLength() == 10)
               rule += ";UNTIL=" + end.Mid(0, 4) + end.Mid(5, 2) + end.Mid(8, 2) + "T235959Z";
         }
      }

      return rule;
   }

   AnsiString
   MigrationSources::GraphContactToVCard(const JsonValue &contact)
   {
      if (!contact.IsObject())
         return "";

      const AnsiString given = Utf8Of(contact.GetString("givenName"));
      const AnsiString family = Utf8Of(contact.GetString("surname"));
      const AnsiString middle = Utf8Of(contact.GetString("middleName"));
      AnsiString display = Utf8Of(contact.GetString("displayName"));

      std::vector<AnsiString> addresses;
      if (const JsonValue *emails = contact.Get("emailAddresses"))
      {
         for (const JsonValue &email : emails->Items())
         {
            const AnsiString address = Dav::Trimmed(Utf8Of(email.GetString("address")));
            if (!address.IsEmpty())
               addresses.push_back(address);
         }
      }

      if (display.IsEmpty())
         display = Dav::Trimmed(given + " " + family);
      if (display.IsEmpty() && !addresses.empty())
         display = addresses[0];
      if (display.IsEmpty())
         return "";

      AnsiString uid = Utf8Of(contact.GetString("id"));
      if (uid.IsEmpty())
         uid = Dav::Sha256Hex(display).Mid(0, 32);

      AnsiString card;
      card += "BEGIN:VCARD\r\n";
      card += "VERSION:3.0\r\n";
      card += "PRODID:-//hMailServer//Migration//EN\r\n";
      card += Line("UID", VCard::Escape(uid));
      card += Line("FN", VCard::Escape(display));
      card += Line("N", VCard::Escape(family) + ";" + VCard::Escape(given) + ";" + VCard::Escape(middle) + ";" +
                        VCard::Escape(Utf8Of(contact.GetString("title"))) + ";" + VCard::Escape(Utf8Of(contact.GetString("generation"))));

      for (size_t i = 0; i < addresses.size(); i++)
         card += Line(i == 0 ? "EMAIL;TYPE=INTERNET,PREF" : "EMAIL;TYPE=INTERNET", VCard::Escape(addresses[i]));

      if (const JsonValue *phones = contact.Get("businessPhones"))
      {
         for (const JsonValue &phone : phones->Items())
         {
            if (!phone.AsString().empty())
               card += Line("TEL;TYPE=WORK,VOICE", VCard::Escape(Utf8Of(phone.AsString())));
         }
      }

      const AnsiString mobile = Utf8Of(contact.GetString("mobilePhone"));
      if (!mobile.IsEmpty())
         card += Line("TEL;TYPE=CELL,VOICE", VCard::Escape(mobile));

      if (const JsonValue *phones = contact.Get("homePhones"))
      {
         for (const JsonValue &phone : phones->Items())
         {
            if (!phone.AsString().empty())
               card += Line("TEL;TYPE=HOME,VOICE", VCard::Escape(Utf8Of(phone.AsString())));
         }
      }

      const AnsiString company = Utf8Of(contact.GetString("companyName"));
      const AnsiString department = Utf8Of(contact.GetString("department"));
      if (!company.IsEmpty() || !department.IsEmpty())
         card += Line("ORG", VCard::Escape(company) + (department.IsEmpty() ? AnsiString() : ";" + VCard::Escape(department)));

      const AnsiString jobTitle = Utf8Of(contact.GetString("jobTitle"));
      if (!jobTitle.IsEmpty())
         card += Line("TITLE", VCard::Escape(jobTitle));

      const AnsiString nickname = Utf8Of(contact.GetString("nickName"));
      if (!nickname.IsEmpty())
         card += Line("NICKNAME", VCard::Escape(nickname));

      const AnsiString birthday = Utf8Of(contact.GetString("birthday"));
      if (birthday.GetLength() >= 10)
         card += Line("BDAY", birthday.Mid(0, 10));

      const AnsiString work = PostalAddress(contact.Get("businessAddress"));
      if (!work.IsEmpty())
         card += Line("ADR;TYPE=WORK", work);
      const AnsiString home = PostalAddress(contact.Get("homeAddress"));
      if (!home.IsEmpty())
         card += Line("ADR;TYPE=HOME", home);

      const AnsiString notes = Utf8Of(contact.GetString("personalNotes"));
      if (!notes.IsEmpty())
         card += Line("NOTE", VCard::Escape(notes));

      card += "END:VCARD\r\n";
      return card;
   }

   AnsiString
   MigrationSources::GraphEventToICalendar(const JsonValue &event)
   {
      if (!event.IsObject())
         return "";

      AnsiString uid = Utf8Of(event.GetString("iCalUId"));
      if (uid.IsEmpty())
         uid = Utf8Of(event.GetString("id"));
      if (uid.IsEmpty())
         return "";

      const bool allDay = event.GetBool("isAllDay");
      const AnsiString startZone = Utf8Of(event.GetString("originalStartTimeZone"));
      const AnsiString endZone = Utf8Of(event.GetString("originalEndTimeZone"));

      const AnsiString start = TimeProperty("DTSTART", event.Get("start"), startZone, allDay);
      if (start.IsEmpty())
         return "";

      AnsiString calendar;
      calendar += "BEGIN:VCALENDAR\r\n";
      calendar += "VERSION:2.0\r\n";
      calendar += "PRODID:-//hMailServer//Migration//EN\r\n";
      calendar += "BEGIN:VEVENT\r\n";
      calendar += CalendarLine("UID", uid);
      calendar += CalendarLine("DTSTAMP", ICalendar::FormatUtc((__int64) time(nullptr)));
      calendar += start;
      calendar += TimeProperty("DTEND", event.Get("end"), endZone.IsEmpty() ? startZone : endZone, allDay);

      const AnsiString subject = Utf8Of(event.GetString("subject"));
      if (!subject.IsEmpty())
         calendar += CalendarLine("SUMMARY", ICalendar::Escape(subject));

      if (const JsonValue *body = event.Get("body"))
      {
         const AnsiString content = Utf8Of(body->GetString("content"));
         const AnsiString text = AnsiString(body->GetString("contentType").c_str()).CompareNoCase("html") == 0 ? TextOfHtml(content) : content;
         if (!text.IsEmpty())
            calendar += CalendarLine("DESCRIPTION", ICalendar::Escape(text));
      }

      if (const JsonValue *location = event.Get("location"))
      {
         const AnsiString where = Utf8Of(location->GetString("displayName"));
         if (!where.IsEmpty())
            calendar += CalendarLine("LOCATION", ICalendar::Escape(where));
      }

      if (const JsonValue *recurrence = event.Get("recurrence"))
      {
         if (recurrence->IsObject())
         {
            const AnsiString rule = RecurrenceRule(*recurrence);
            if (!rule.IsEmpty())
               calendar += CalendarLine("RRULE", rule);
         }
      }

      if (const JsonValue *organizer = event.Get("organizer"))
      {
         if (const JsonValue *who = organizer->Get("emailAddress"))
         {
            const AnsiString address = Utf8Of(who->GetString("address"));
            const AnsiString name = Utf8Of(who->GetString("name"));
            if (!address.IsEmpty())
               calendar += CalendarLine(name.IsEmpty() ? AnsiString("ORGANIZER") : "ORGANIZER;CN=" + ParamValue(name), "mailto:" + address);
         }
      }

      if (const JsonValue *attendees = event.Get("attendees"))
      {
         for (const JsonValue &attendee : attendees->Items())
         {
            const JsonValue *who = attendee.Get("emailAddress");
            if (!who)
               continue;

            const AnsiString address = Utf8Of(who->GetString("address"));
            if (address.IsEmpty())
               continue;

            const AnsiString name = Utf8Of(who->GetString("name"));
            const std::string type = attendee.GetString("type");
            AnsiString response;
            if (const JsonValue *status = attendee.Get("status"))
               response = PartStat(status->GetString("response"));
            else
               response = "NEEDS-ACTION";

            AnsiString property = "ATTENDEE";
            if (!name.IsEmpty())
               property += ";CN=" + ParamValue(name);
            property += type == "optional" ? ";ROLE=OPT-PARTICIPANT" : (type == "resource" ? ";CUTYPE=RESOURCE" : ";ROLE=REQ-PARTICIPANT");
            property += ";PARTSTAT=" + response;
            calendar += CalendarLine(property, "mailto:" + address);
         }
      }

      if (event.GetBool("isCancelled"))
         calendar += "STATUS:CANCELLED\r\n";

      if (event.GetString("showAs") == "free")
         calendar += "TRANSP:TRANSPARENT\r\n";

      calendar += "END:VEVENT\r\n";
      calendar += "END:VCALENDAR\r\n";
      return calendar;
   }

   void
   MigrationSourcesTester::Test()
   {
      typedef MigrationSources S;

      // The table: three kinds, each with the host, the scopes and the
      // endpoints its documentation names.
      const MigrationProvider *imap = S::ByKind(0);
      const MigrationProvider *google = S::ByKind(1);
      const MigrationProvider *microsoft = S::ByKind(2);
      Expect(imap && google && microsoft && !S::ByKind(3), "Three kinds of source, and no fourth.");
      Expect(S::ByWord("google") == google && S::ByWord("MICROSOFT365") == microsoft && S::ByWord("imap") == imap &&
             !S::ByWord("yahoo"), "The REST words name the kinds.");
      Expect(AnsiString(imap->oauthProvider).IsEmpty() && AnsiString(imap->mailScope).IsEmpty() && !imap->graph,
             "An IMAP server is a password and DAV discovered from the host.");
      Expect(AnsiString(google->oauthProvider) == "google" && AnsiString(google->imapHost) == "imap.gmail.com" && google->imapPort == 993,
             "Google: Gmail's IMAP on 993 with the google sign-in.");
      Expect(AnsiString(google->mailScope) == "https://mail.google.com/", "Gmail's IMAP takes the one mail scope.");
      Expect(AnsiString(google->contactsScope) == "https://www.googleapis.com/auth/carddav", "Google's CardDAV scope.");
      Expect(AnsiString(google->calendarsScope) == "https://www.googleapis.com/auth/calendar", "Google's CalDAV scope.");
      Expect(AnsiString(google->consentScope).Find("https://www.googleapis.com/auth/carddav") >= 0 &&
             AnsiString(google->consentScope).Find("https://mail.google.com/") >= 0 &&
             AnsiString(google->consentScope).Find("email") >= 0, "Google's sign-in asks for every scope the job uses.");
      Expect(AnsiString(google->contactsUrl).StartsWith("https://www.googleapis.com/carddav/v1/principals/") && !google->graph,
             "Google's contacts are its CardDAV endpoint, not the People API.");
      Expect(AnsiString(google->calendarsUrl).StartsWith("https://apidata.googleusercontent.com/caldav/v2/"), "Google's CalDAV endpoint.");
      Expect(AnsiString(microsoft->oauthProvider) == "microsoft" && AnsiString(microsoft->imapHost) == "outlook.office365.com" && microsoft->graph,
             "Microsoft 365: Outlook's IMAP, and Graph for the rest.");
      Expect(AnsiString(microsoft->mailScope).Find("IMAP.AccessAsUser.All") >= 0 &&
             AnsiString(microsoft->contactsScope).Find("Contacts.Read") >= 0 &&
             AnsiString(microsoft->calendarsScope).Find("Calendars.Read") >= 0, "Microsoft's three scopes, one resource each.");
      Expect(AnsiString(microsoft->contactsUrl) == "https://graph.microsoft.com/v1.0/me/contacts?$top=100" &&
             AnsiString(microsoft->calendarsUrl) == "https://graph.microsoft.com/v1.0/me/events?$top=50", "Graph's two lists.");

      Expect(S::DavStart(*imap, "mail.example.com", "", "a@b", false) == "https://mail.example.com/.well-known/carddav",
             "An IMAP server's CardDAV is found on its own host.");
      Expect(S::DavStart(*imap, "mail.example.com", "http://127.0.0.1:9105/", "a@b", true) == "http://127.0.0.1:9105/.well-known/caldav",
             "The migration's DAV URL wins.");
      Expect(S::DavStart(*google, "", "", "a@example.com", false) == "https://www.googleapis.com/carddav/v1/principals/a@example.com/lists/default/",
             "Google's CardDAV names the user.");

      Expect(S::ResolveUrl("http://127.0.0.1:9105/.well-known/carddav", "/dav/") == "http://127.0.0.1:9105/dav/", "An absolute path.");
      Expect(S::ResolveUrl("https://h/dav/addressbooks/a/", "contacts/") == "https://h:443/dav/addressbooks/a/contacts/", "A relative path.");
      Expect(S::ResolveUrl("https://h/x", "https://other/y") == "https://other/y", "An absolute URL.");
      Expect(S::OriginOf("https://Mail.Example.com/dav/x") == "https://mail.example.com:443" &&
             S::OriginOf("http://127.0.0.1:9957/.well-known/carddav") == "http://127.0.0.1:9957" &&
             S::OriginOf("https://graph.microsoft.com/v1.0/me/contacts?$top=100") == S::OriginOf("https://graph.microsoft.com/v1.0/me/contacts?$skip=100"),
             "The origin the credentials are kept to: scheme, host and port.");

      Expect(S::DecodeXmlText("a &lt;b&gt; &amp; &#233; &#x263A;") == "a <b> & \xC3\xA9 \xE2\x98\xBA", "Entities decoded to UTF-8.");
      Expect(S::DecodeXmlText("<![CDATA[BEGIN:VCARD & <x>]]>") == "BEGIN:VCARD & <x>", "CDATA kept as it is.");

      std::vector<MigrationSources::Item> items;
      S::ExtractItems("<d:multistatus xmlns:d=\"DAV:\" xmlns:card=\"urn:ietf:params:xml:ns:carddav\">"
                      "<d:response><d:href>/ab/1.vcf</d:href><d:propstat><d:prop><card:address-data>BEGIN:VCARD&#13;\nFN:A&amp;B&#13;\nEND:VCARD&#13;\n</card:address-data></d:prop></d:propstat></d:response>"
                      "<d:response><d:href>/ab/2.vcf</d:href><d:propstat><d:prop><card:address-data/></d:prop></d:propstat></d:response>"
                      "</d:multistatus>", "address-data", items);
      Expect(items.size() == 1 && items[0].href == "/ab/1.vcf" && items[0].data == "BEGIN:VCARD\r\nFN:A&B\r\nEND:VCARD\r\n",
             "A multistatus read for its hrefs and card text; an empty one is left out.");

      Expect(S::GraphTimeToUtc("2026-09-24T10:00:00.0000000") == "20260924T100000Z", "Graph's time as iCalendar UTC.");
      Expect(S::GraphTimeToUtc("not a time").IsEmpty(), "Anything else is refused.");

      JsonValue contact;
      std::string parseError;
      Expect(JsonValue::Parse("{\"id\":\"c1\",\"displayName\":\"Ann Example\",\"givenName\":\"Ann\",\"surname\":\"Example\","
                              "\"emailAddresses\":[{\"address\":\"ann@example.com\"},{\"address\":\"ann@home.example\"}],"
                              "\"mobilePhone\":\"+44 1\",\"companyName\":\"Example; Ltd\"}", contact, parseError), "The contact parses.");
      const AnsiString card = S::GraphContactToVCard(contact);
      std::vector<VCardProperty> properties;
      AnsiString problem;
      AnsiString name, address;
      Expect(VCard::Parse(card, properties, problem) && VCard::ExtractNameAndAddress(properties, name, address) &&
             name == "Ann Example" && address == "ann@example.com", "A Graph contact is a vCard the address book takes, the first address preferred.");
      Expect(card.Find("ORG:Example\\; Ltd") >= 0 && card.Find("TEL;TYPE=CELL,VOICE:+44 1") >= 0, "Its other fields, escaped.");

      JsonValue event;
      Expect(JsonValue::Parse("{\"id\":\"e1\",\"iCalUId\":\"uid-1\",\"subject\":\"Stand-up\",\"isAllDay\":false,"
                              "\"start\":{\"dateTime\":\"2026-09-24T08:00:00.0000000\",\"timeZone\":\"UTC\"},"
                              "\"end\":{\"dateTime\":\"2026-09-24T08:15:00.0000000\",\"timeZone\":\"UTC\"},"
                              "\"originalStartTimeZone\":\"UTC\",\"originalEndTimeZone\":\"UTC\","
                              "\"recurrence\":{\"pattern\":{\"type\":\"weekly\",\"interval\":1,\"daysOfWeek\":[\"monday\",\"thursday\"],\"firstDayOfWeek\":\"monday\"},"
                              "\"range\":{\"type\":\"numbered\",\"numberOfOccurrences\":10}}}", event, parseError), "The event parses.");
      const AnsiString text = S::GraphEventToICalendar(event);
      ICalComponent tree;
      ICalendar::Summary summary;
      Expect(ICalendar::Parse(text, tree, problem) && ICalendar::Summarize(tree, summary, problem) && summary.uid == "uid-1" &&
             summary.recurring, "A Graph event is an iCalendar object the calendar store takes, its recurrence kept.");
      Expect(text.Find("RRULE:FREQ=WEEKLY;BYDAY=MO,TH;WKST=MO;COUNT=10") >= 0, "The weekly pattern as an RRULE.");

      // A weekly meeting at 09:00 in London, read in July: Graph answers 08:00
      // UTC, and the event keeps its own zone's wall clock, which summer time
      // moves off UTC.
      JsonValue london;
      Expect(JsonValue::Parse("{\"id\":\"e2\",\"iCalUId\":\"uid-2\",\"subject\":\"Planning\",\"isAllDay\":false,"
                              "\"start\":{\"dateTime\":\"2026-07-09T08:00:00.0000000\",\"timeZone\":\"UTC\"},"
                              "\"end\":{\"dateTime\":\"2026-07-09T08:30:00.0000000\",\"timeZone\":\"UTC\"},"
                              "\"originalStartTimeZone\":\"GMT Standard Time\",\"originalEndTimeZone\":\"GMT Standard Time\","
                              "\"recurrence\":{\"pattern\":{\"type\":\"weekly\",\"interval\":1,\"daysOfWeek\":[\"thursday\"]},"
                              "\"range\":{\"type\":\"noEnd\"}}}", london, parseError), "The London event parses.");
      const AnsiString londonText = S::GraphEventToICalendar(london);
      ICalComponent londonTree;
      ICalendar::Summary londonSummary;
      Expect(londonText.Find("DTSTART;TZID=GMT Standard Time:20260709T090000") >= 0 &&
             londonText.Find("DTEND;TZID=GMT Standard Time:20260709T093000") >= 0,
             "An event in a zone the iCalendar module knows keeps its wall clock there: 09:00 in London in July.");
      Expect(ICalendar::Parse(londonText, londonTree, problem) && ICalendar::Summarize(londonTree, londonSummary, problem) &&
             londonSummary.uid == "uid-2" && londonSummary.recurring, "And the calendar store takes it.");

      JsonValue monthly;
      Expect(JsonValue::Parse("{\"pattern\":{\"type\":\"relativeMonthly\",\"interval\":2,\"daysOfWeek\":[\"friday\"],\"index\":\"last\"},"
                              "\"range\":{\"type\":\"endDate\",\"endDate\":\"2027-06-30\"}}", monthly, parseError), "The recurrence parses.");
      Expect(S::RecurrenceRule(monthly) == "FREQ=MONTHLY;BYDAY=FR;BYSETPOS=-1;INTERVAL=2;UNTIL=20270630T235959Z",
             "The last Friday of every other month, until a date.");
   }
}
