// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See CalDavServer.h for the shape of the tree. This file is the request
// side of it, on the reader, the authentication and the response builders
// DavSupport shares with CardDAV: the DAV: and CALDAV: property set of each
// resource kind, the four reports, and PUT/GET/DELETE of one object, on top
// of CalendarStore, ICalendar and FreeBusy.
//
// The bits of RFC 4791 and 6578 that shape the answers:
//
//   - PROPFIND (4918 section 9.1) as CardDAV's: 207, one D:response per
//     resource in scope, a 200 propstat for the properties found and a 404
//     one for those asked for and not there. Depth infinity is treated as 1.
//   - calendar-query (4791 section 7.8): the filter is a tree of comp-filters
//     from VCALENDAR down, each matching when the component is there (or is
//     not, with is-not-defined), its time-range overlaps an instance of the
//     object - the recurrence expanded through ICalendar, bounded - and its
//     prop-filters match: a property present (or absent), and a text-match
//     a substring of one of its values under i;ascii-casemap or i;octet. A
//     param-filter is not evaluated. When the VEVENT or VTODO comp-filter
//     carries a time-range, the store narrows the candidates by the span it
//     keeps per object before any object is parsed.
//   - calendar-multiget (section 7.9): the hrefs answered from the members
//     in memory, a repeated href once, 404 for one that is not there, and
//     the whole multistatus bounded to 64 MiB (507 beyond).
//   - sync-collection (6578): the collection's counter is the token. An
//     empty token answers every live member; a token the collection has
//     reached answers the rows stamped after it, a tombstone as a 404
//     response; a token ahead of the collection is refused with
//     DAV:valid-sync-token. Tombstones are kept, so no token expires.
//   - free-busy-query (4791 section 7.10): on a calendar the caller may read
//     - its own, or one shared with it - one VFREEBUSY as text/calendar over
//     the time-range asked for (both bounds required, at most 62 days), the
//     same computation as GET /api/v1/me/freebusy (FreeBusy.h). Not RFC 6638
//     scheduling: no schedule-outbox, no inbox, no other server's calendar.
//   - ETags are strong: the SHA-256 of the bytes served. getctag and the
//     sync-token are the counter, which every write steps.
//   - PUT (section 5.3.2): the object must be one VCALENDAR of VEVENT or
//     VTODO components sharing one UID (CALDAV:valid-calendar-object-resource,
//     CALDAV:supported-calendar-component), and no other resource of the
//     calendar may hold that UID (CALDAV:no-uid-conflict, naming it). What
//     ICalendar cannot expand is refused with CALDAV:valid-calendar-data and
//     the sentence, because a stored object that cannot be answered for is
//     the worse outcome. If-None-Match: * creates and is 412 on an existing
//     resource; If-Match must equal the current ETag or the answer is 412.

#include "StdAfx.h"

#include <algorithm>
#include <set>

#include "CalDavServer.h"
#include "DavSupport.h"
#include "CalendarStore.h"
#include "ICalendar.h"
#include "FreeBusy.h"
#include "ResourceBooking.h"
#include "../BO/Account.h"
#include "../Cache/CacheContainer.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const char *CalDavServer::CalendarsPath = "/dav/calendars/";

   using namespace Dav;

   namespace
   {
      const char *NsDav = "DAV:";
      const char *NsCalDav = "urn:ietf:params:xml:ns:caldav";
      const char *NsCalendarServer = "http://calendarserver.org/ns/";
      const char *NsApple = "http://apple.com/ns/ical/";   // calendar-color and calendar-order, as every client reads them

      // How many hrefs one multiget may name; the XML ceilings are DavSupport's.
      const size_t MaxMultigetHrefs = 5000;
      const size_t MaxMultistatusBytes = 64 * 1024 * 1024;

      // What CALDAV:max-resource-size advertises and PUT enforces: the
      // listener's large-request cap.
      const int MaxObjectBytes = 1024 * 1024;

      // The instances one object is expanded to for a query: bounded by the
      // module's own ceiling as well.
      const size_t QueryInstances = ICalendar::MaxInstances;

      const char *AllowObject = "Allow: OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, REPORT\r\n";
      const char *AllowCollection = "Allow: OPTIONS, PROPFIND, PROPPATCH, REPORT, MKCALENDAR, MKCOL, DELETE\r\n";

      //------------------------------------------------------------------------
      // The resources
      //------------------------------------------------------------------------

      enum ResourceKind
      {
         KindHomeCollection,   // /dav/calendars/
         KindHome,             // /dav/calendars/<address>/
         KindCalendar,         // /dav/calendars/<address>/<calendar>/ - the account's own, or shared-<id>/ for one shared with it
         KindCalendarSlot,     // /dav/calendars/<address>/<name>/, no calendar of that name yet: MKCALENDAR may create
         KindObject,           // .../<calendar>/<name>, and the object exists
         KindObjectSlot        // .../<calendar>/<name>, nothing there yet: PUT may create
      };

      struct Resource
      {
         ResourceKind kind;
         AnsiString href;              // canonical, percent-encoded
         CalendarObjectRecord object;  // KindObject only
         AnsiString slotName;          // KindObjectSlot and KindCalendarSlot, decoded

         // KindCalendar: the calendar this resource is, whose it is and what
         // the caller may do with it - the request's own target, or one row of
         // the home's listing.
         CalendarRecord calendar;
         bool shared;
         bool canWrite;
         AnsiString ownerPrincipalHref;

         Resource() : kind(KindHomeCollection), shared(false), canWrite(true) { }
      };

      // A member of the calendar as the responses see it.
      struct Member
      {
         Member() : parsed(false), parseFailed(false) { }

         CalendarObjectRecord record;
         AnsiString data;
         AnsiString etag;
         AnsiString href;

         // The tree, read on first need by a filter.
         bool parsed;
         bool parseFailed;
         ICalComponent tree;
      };

      struct Context
      {
         Context(const HttpRequest &httpRequest) : request(httpRequest), storeAccountId(0), shared(false), canWrite(true), calendarLoaded(false), calendarFailed(false), listed(false), listFailed(false) { }

         const HttpRequest &request;
         std::shared_ptr<const Account> account;

         String addressWide;
         AnsiString addressUtf8;
         AnsiString principalHref;
         AnsiString homeHref;
         AnsiString calendarHref;

         // The calendar the target names, once Resolve found it: the store is
         // asked under its owner's account id, which is the caller's own for
         // its own calendars and the sharer's for one shared with it.
         __int64 storeAccountId;
         bool shared;
         bool canWrite;
         AnsiString ownerPrincipalHref;

         bool calendarLoaded;
         bool calendarFailed;
         CalendarRecord calendar;

         bool listed;
         bool listFailed;
         std::vector<Member> members;
      };

      bool EnsureCalendar(Context &context)
      {
         if (context.calendarLoaded)
            return !context.calendarFailed;
         context.calendarLoaded = true;
         if (!CalendarStore::EnsureDefault(context.account->GetID(), context.calendar))
         {
            context.calendarFailed = true;
            return false;
         }
         context.storeAccountId = context.account->GetID();
         return true;
      }

      // The calendar's token read again, after a write.
      bool RefreshCalendar(Context &context)
      {
         if (!EnsureCalendar(context))
            return false;
         return CalendarStore::Get(context.storeAccountId, context.calendar.id, context.calendar);
      }

      AnsiString PrincipalHrefOf(const AnsiString &addressUtf8)
      {
         return "/dav/principals/" + PercentEncodeSegment(addressUtf8) + "/";
      }

      // The principal of the account that owns a calendar.
      AnsiString OwnerPrincipalOf(const Context &context, const CalendarRecord &calendar)
      {
         if (calendar.accountId == context.account->GetID())
            return context.principalHref;
         std::shared_ptr<const Account> owner = CacheContainer::Instance()->GetAccount(calendar.accountId);
         if (!owner)
            return context.principalHref;
         String address = owner->GetAddress();
         address.ToLower();
         return PrincipalHrefOf(Utf8(address));
      }

      // The segment a calendar has under the caller's home: its own name, or
      // shared-<id> for one another account shares with it.
      AnsiString CalendarSegment(const Context &context, const CalendarRecord &calendar)
      {
         if (calendar.accountId == context.account->GetID())
            return PercentEncodeSegment(Utf8(calendar.name));
         return "shared-" + Int64Text(calendar.id);
      }

      void FillCalendarResource(const Context &context, const CalendarRecord &calendar, bool shared, bool canWrite, Resource &resource)
      {
         resource.kind = KindCalendar;
         resource.calendar = calendar;
         resource.shared = shared;
         resource.canWrite = canWrite;
         resource.ownerPrincipalHref = OwnerPrincipalOf(context, calendar);
         resource.href = context.homeHref + CalendarSegment(context, calendar) + "/";
      }

      // The target's calendar becomes the context's: every store call below
      // reads and writes it under its owner's account id.
      void AdoptCalendar(Context &context, const CalendarRecord &calendar, bool shared, bool canWrite)
      {
         context.calendar = calendar;
         context.calendarLoaded = true;
         context.calendarFailed = false;
         context.storeAccountId = calendar.accountId;
         context.shared = shared;
         context.canWrite = canWrite;
         context.ownerPrincipalHref = OwnerPrincipalOf(context, calendar);
         context.calendarHref = context.homeHref + CalendarSegment(context, calendar) + "/";
      }

      AnsiString ObjectHref(const Context &context, const CalendarObjectRecord &record)
      {
         return context.calendarHref + PercentEncodeSegment(Utf8(record.uri));
      }

      void FillMember(const Context &context, const CalendarObjectRecord &record, Member &member)
      {
         member.record = record;
         member.data = Utf8(record.data);
         member.etag = Utf8(record.etag);
         member.href = ObjectHref(context, record);
         member.parsed = false;
         member.parseFailed = false;
      }

      bool ParsedMember(Member &member)
      {
         if (member.parsed)
            return !member.parseFailed;
         member.parsed = true;
         AnsiString problem;
         if (!ICalendar::Parse(member.data, member.tree, problem))
            member.parseFailed = true;
         return !member.parseFailed;
      }

      void FillMembers(Context &context, const std::vector<CalendarObjectRecord> &records)
      {
         context.members.clear();
         for (size_t i = 0; i < records.size(); i++)
         {
            Member member;
            FillMember(context, records[i], member);
            context.members.push_back(member);
         }
      }

      bool EnsureListed(Context &context)
      {
         if (context.listed)
            return !context.listFailed;

         context.listed = true;
         if (!EnsureCalendar(context))
         {
            context.listFailed = true;
            return false;
         }

         std::vector<CalendarObjectRecord> records;
         if (!CalendarStore::List(context.storeAccountId, context.calendar.id, records))
         {
            context.listFailed = true;
            return false;
         }
         FillMembers(context, records);
         return true;
      }

      AnsiString SyncTokenFor(__int64 token)
      {
         return "urn:x-hmailserver:caldav-sync:" + Int64Text(token);
      }

      bool SameAddress(const Context &context, const AnsiString &segment)
      {
         String wide = Wide(segment);
         wide.ToLower();
         return wide == context.addressWide;
      }

      // The resource a path names, or false: a path outside the tree, or
      // into another account's part of it, is simply not found.
      bool Resolve(Context &context, const AnsiString &target, Resource &resource)
      {
         std::vector<AnsiString> segments = PathSegments(target);

         // "/dav/calendars" -> ["dav", "calendars"]; "/dav/calendars/" adds "".
         if (segments.size() < 2 || segments[0] != "dav" || segments[1] != "calendars")
            return false;

         bool trailingSlash = segments.size() > 2 && segments.back().IsEmpty();
         if (trailingSlash)
            segments.pop_back();

         if (segments.size() == 2)
         {
            resource.kind = KindHomeCollection;
            resource.href = CalDavServer::CalendarsPath;
            return true;
         }

         if (!SameAddress(context, segments[2]))
            return false;

         if (segments.size() == 3)
         {
            resource.kind = KindHome;
            resource.href = context.homeHref;
            return true;
         }

         const AnsiString &segment = segments[3];
         if (segment.IsEmpty())
            return false;

         CalendarRecord calendar;
         bool found = false;
         bool shared = false;
         bool canWrite = true;
         if (segment.StartsWith("shared-"))
         {
            AnsiString digits = segment.Mid(7);
            __int64 id = 0;
            bool numeric = !digits.IsEmpty() && digits.GetLength() < 18;
            for (int i = 0; numeric && i < digits.GetLength(); i++)
               numeric = digits[i] >= '0' && digits[i] <= '9';
            for (int i = 0; numeric && i < digits.GetLength(); i++)
               id = id * 10 + (digits[i] - '0');

            CalendarShareRecord share;
            if (numeric && CalendarStore::ShareOf(id, context.account->GetID(), share) && CalendarStore::GetAny(id, calendar))
            {
               found = true;
               shared = true;
               canWrite = share.write;
            }
         }
         else
         {
            found = CalendarStore::FindByName(context.account->GetID(), Wide(segment), calendar);
         }

         if (!found)
         {
            // A name with nothing behind it: MKCALENDAR may make it. Anything
            // below such a name is simply not there.
            if (segments.size() != 4 || shared || segment.StartsWith("shared-"))
               return false;
            if (!CalendarStore::IsUsableCalendarName(Wide(segment)))
               return false;
            resource.kind = KindCalendarSlot;
            resource.slotName = segment;
            resource.href = context.homeHref + PercentEncodeSegment(segment) + "/";
            return true;
         }

         AdoptCalendar(context, calendar, shared, canWrite);

         if (segments.size() == 4)
         {
            FillCalendarResource(context, calendar, shared, canWrite, resource);
            return true;
         }

         if (segments.size() != 5 || trailingSlash || segments[4].IsEmpty())
            return false;

         AnsiString name = segments[4];
         CalendarObjectRecord named;
         if (CalendarStore::FindByUri(context.storeAccountId, context.calendar.id, Wide(name), named))
         {
            resource.kind = KindObject;
            resource.object = named;
            resource.href = ObjectHref(context, named);
            return true;
         }

         resource.kind = KindObjectSlot;
         resource.href = context.calendarHref + PercentEncodeSegment(name);
         resource.slotName = name;
         return true;
      }

      //------------------------------------------------------------------------
      // Responses
      //------------------------------------------------------------------------

      HttpResponse DavError(int status, const AnsiString &preconditionXml, const AnsiString &description)
      {
         AnsiString xml = "<D:error xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">" + preconditionXml +
            "<D:responsedescription>" + XmlEscape(description) + "</D:responsedescription></D:error>\r\n";
         return Xml(status, xml);
      }

      HttpResponse MethodNotAllowed(const Resource &resource)
      {
         bool object = resource.kind == KindObject || resource.kind == KindObjectSlot;
         return Text(405, "method not allowed", object ? AllowObject : AllowCollection);
      }

      AnsiString MultistatusOpen()
      {
         return "<D:multistatus xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\" xmlns:CS=\"http://calendarserver.org/ns/\" xmlns:A=\"http://apple.com/ns/ical/\">\r\n";
      }

      //------------------------------------------------------------------------
      // Properties
      //------------------------------------------------------------------------

      struct PropertyName
      {
         PropertyName() { }
         PropertyName(const char *propertyNs, const char *propertyName) : ns(propertyNs), name(propertyName) { }

         AnsiString ns;
         AnsiString name;
      };

      enum PropertyMode
      {
         ModeProp,
         ModeAllProp,
         ModePropName
      };

      struct PropertyRequest
      {
         PropertyRequest() : mode(ModeAllProp) { }

         PropertyMode mode;
         std::vector<PropertyName> names;
      };

      AnsiString PropertyElement(const PropertyName &property, const AnsiString &innerXml)
      {
         AnsiString prefix;
         if (property.ns == NsDav) prefix = "D:";
         else if (property.ns == NsCalDav) prefix = "C:";
         else if (property.ns == NsCalendarServer) prefix = "CS:";
         else if (property.ns == NsApple) prefix = "A:";

         AnsiString open = "<" + prefix + property.name;
         if (prefix.IsEmpty())
            open += " xmlns=\"" + XmlEscape(property.ns) + "\"";

         if (innerXml.IsEmpty())
            return open + "/>";
         return open + ">" + innerXml + "</" + prefix + property.name + ">";
      }

      AnsiString Href(const AnsiString &href)
      {
         return "<D:href>" + XmlEscape(href) + "</D:href>";
      }

      AnsiString ContentTypeOf(const Member &member)
      {
         AnsiString component = Utf8(member.record.component);
         AnsiString type = "text/calendar; charset=utf-8";
         if (!component.IsEmpty())
            type += "; component=" + component;
         return type;
      }

      bool PropertyValue(Context &context, const Resource &resource, const Member *member, const PropertyName &property, AnsiString &innerXml)
      {
         ResourceKind kind = resource.kind;
         bool isObject = kind == KindObject;
         bool isCollection = !isObject && kind != KindObjectSlot && kind != KindCalendarSlot;

         if (property.ns == NsDav)
         {
            if (property.name == "resourcetype")
            {
               innerXml = "";
               if (isCollection)
                  innerXml += "<D:collection/>";
               if (kind == KindCalendar)
                  innerXml += "<C:calendar/>";
               return true;
            }

            if (property.name == "displayname")
            {
               switch (kind)
               {
               case KindHomeCollection: innerXml = "Calendars"; break;
               case KindHome: innerXml = XmlEscape("Calendars of " + context.addressUtf8); break;
               case KindCalendar: innerXml = XmlEscape(Utf8(resource.calendar.displayName)); break;
               case KindObject: innerXml = XmlEscape(Utf8(resource.object.uri)); break;
               default: return false;
               }
               return true;
            }

            if (property.name == "current-user-principal")
            {
               innerXml = Href(context.principalHref);
               return true;
            }

            if (property.name == "principal-collection-set")
            {
               innerXml = Href(AnsiString("/dav/principals/"));
               return true;
            }

            if (property.name == "owner" && (kind == KindHome || kind == KindCalendar || isObject))
            {
               innerXml = Href(kind == KindCalendar ? resource.ownerPrincipalHref : (isObject ? context.ownerPrincipalHref : context.principalHref));
               return true;
            }

            if (property.name == "current-user-privilege-set" && (kind == KindHome || kind == KindCalendar || isObject))
            {
               // A calendar shared read-only grants reading and nothing else;
               // everything the caller owns, or is shared with write, is writable.
               bool writable = kind == KindCalendar ? resource.canWrite : (isObject ? context.canWrite : true);
               innerXml = "<D:privilege><D:read/></D:privilege><D:privilege><D:read-current-user-privilege-set/></D:privilege>";
               if (writable)
                  innerXml += "<D:privilege><D:write/></D:privilege><D:privilege><D:write-content/></D:privilege>"
                              "<D:privilege><D:bind/></D:privilege><D:privilege><D:unbind/></D:privilege>";
               return true;
            }

            if (property.name == "supported-report-set" && kind == KindCalendar)
            {
               innerXml = "<D:supported-report><D:report><C:calendar-multiget/></D:report></D:supported-report>"
                          "<D:supported-report><D:report><C:calendar-query/></D:report></D:supported-report>"
                          "<D:supported-report><D:report><D:sync-collection/></D:report></D:supported-report>"
                          "<D:supported-report><D:report><C:free-busy-query/></D:report></D:supported-report>";
               return true;
            }

            if (property.name == "sync-token" && kind == KindCalendar)
            {
               if (resource.calendar.id != context.calendar.id && !EnsureCalendar(context))
                  return false;
               innerXml = XmlEscape(SyncTokenFor(context.calendar.syncToken));
               return true;
            }

            if (property.name == "getetag" && isObject && member)
            {
               innerXml = XmlEscape(member->etag);
               return true;
            }

            if (property.name == "getcontenttype" && isObject && member)
            {
               innerXml = XmlEscape(ContentTypeOf(*member));
               return true;
            }

            if (property.name == "getcontentlength" && isObject && member)
            {
               innerXml = IntText(member->data.GetLength());
               return true;
            }

            return false;
         }

         if (property.ns == NsCalDav)
         {
            if (property.name == "calendar-home-set" && (kind == KindHomeCollection || kind == KindHome))
            {
               innerXml = Href(context.homeHref);
               return true;
            }

            if (property.name == "calendar-user-address-set" && (kind == KindHomeCollection || kind == KindHome))
            {
               innerXml = Href("mailto:" + context.addressUtf8);
               return true;
            }

            if (kind == KindCalendar)
            {
               if (property.name == "supported-calendar-component-set")
               {
                  innerXml = "<C:comp name=\"VEVENT\"/><C:comp name=\"VTODO\"/>";
                  return true;
               }
               if (property.name == "supported-calendar-data")
               {
                  innerXml = "<C:calendar-data content-type=\"text/calendar\" version=\"2.0\"/>";
                  return true;
               }
               if (property.name == "calendar-description")
               {
                  innerXml = XmlEscape(Utf8(resource.calendar.displayName) + (resource.shared ? AnsiString(", shared with ") + context.addressUtf8 : AnsiString(" of ") + context.addressUtf8));
                  return true;
               }
               if (property.name == "max-resource-size")
               {
                  innerXml = IntText(MaxObjectBytes);
                  return true;
               }
               if (property.name == "supported-collation-set")
               {
                  innerXml = "<C:supported-collation>i;ascii-casemap</C:supported-collation>"
                             "<C:supported-collation>i;octet</C:supported-collation>";
                  return true;
               }
               // calendar-timezone is not kept: a client that asks is told
               // it is absent, and reads times by the objects' own zones.
            }

            if (property.name == "calendar-data" && isObject && member)
            {
               innerXml = XmlEscape(member->data);
               return true;
            }

            return false;
         }

         if (property.ns == NsApple && kind == KindCalendar)
         {
            if (property.name == "calendar-color")
            {
               AnsiString color = Utf8(resource.calendar.color);
               if (color.IsEmpty())
                  return false;
               innerXml = XmlEscape(color);
               return true;
            }
            if (property.name == "calendar-order")
            {
               innerXml = resource.calendar.name.CompareNoCase(String(CalendarStore::DefaultName)) == 0 ? "0" : "1";
               return true;
            }
            return false;
         }

         if (property.ns == NsCalendarServer)
         {
            if (property.name == "getctag" && kind == KindCalendar)
            {
               innerXml = XmlEscape(Int64Text(resource.calendar.syncToken));
               return true;
            }
         }

         return false;
      }

      void KnownProperties(ResourceKind kind, bool includeCalendarData, std::vector<PropertyName> &names)
      {
         names.push_back(PropertyName(NsDav, "resourcetype"));
         names.push_back(PropertyName(NsDav, "displayname"));
         names.push_back(PropertyName(NsDav, "current-user-principal"));
         names.push_back(PropertyName(NsDav, "principal-collection-set"));

         switch (kind)
         {
         case KindHomeCollection:
            names.push_back(PropertyName(NsCalDav, "calendar-home-set"));
            break;
         case KindHome:
            names.push_back(PropertyName(NsDav, "owner"));
            names.push_back(PropertyName(NsDav, "current-user-privilege-set"));
            names.push_back(PropertyName(NsCalDav, "calendar-home-set"));
            names.push_back(PropertyName(NsCalDav, "calendar-user-address-set"));
            break;
         case KindCalendar:
            names.push_back(PropertyName(NsDav, "owner"));
            names.push_back(PropertyName(NsDav, "current-user-privilege-set"));
            names.push_back(PropertyName(NsDav, "supported-report-set"));
            names.push_back(PropertyName(NsDav, "sync-token"));
            names.push_back(PropertyName(NsCalDav, "calendar-description"));
            names.push_back(PropertyName(NsCalDav, "supported-calendar-component-set"));
            names.push_back(PropertyName(NsCalDav, "supported-calendar-data"));
            names.push_back(PropertyName(NsCalDav, "max-resource-size"));
            names.push_back(PropertyName(NsCalDav, "supported-collation-set"));
            names.push_back(PropertyName(NsCalendarServer, "getctag"));
            names.push_back(PropertyName(NsApple, "calendar-color"));
            names.push_back(PropertyName(NsApple, "calendar-order"));
            break;
         case KindObject:
            names.push_back(PropertyName(NsDav, "getetag"));
            names.push_back(PropertyName(NsDav, "getcontenttype"));
            names.push_back(PropertyName(NsDav, "getcontentlength"));
            names.push_back(PropertyName(NsDav, "owner"));
            names.push_back(PropertyName(NsDav, "current-user-privilege-set"));
            if (includeCalendarData)
               names.push_back(PropertyName(NsCalDav, "calendar-data"));
            break;
         default:
            break;
         }
      }

      AnsiString ResponseFor(Context &context, const Resource &resource, const Member *member, const PropertyRequest &request)
      {
         std::vector<PropertyName> names;
         if (request.mode == ModeProp)
            names = request.names;
         else
         {
            KnownProperties(resource.kind, request.mode == ModePropName, names);
            for (size_t i = 0; i < request.names.size(); i++)
            {
               bool known = false;
               for (size_t j = 0; !known && j < names.size(); j++)
                  known = names[j].ns == request.names[i].ns && names[j].name == request.names[i].name;
               if (!known)
                  names.push_back(request.names[i]);
            }
         }

         AnsiString found;
         AnsiString missing;
         for (size_t i = 0; i < names.size(); i++)
         {
            if (request.mode == ModePropName)
            {
               found += PropertyElement(names[i], "");
               continue;
            }

            AnsiString innerXml;
            if (PropertyValue(context, resource, member, names[i], innerXml))
               found += PropertyElement(names[i], innerXml);
            else
               missing += PropertyElement(names[i], "");
         }

         AnsiString xml = " <D:response>\r\n  " + Href(resource.href) + "\r\n";
         if (!found.IsEmpty() || missing.IsEmpty())
            xml += "  <D:propstat><D:prop>" + found + "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\r\n";
         if (!missing.IsEmpty())
            xml += "  <D:propstat><D:prop>" + missing + "</D:prop><D:status>HTTP/1.1 404 Not Found</D:status></D:propstat>\r\n";
         xml += " </D:response>\r\n";
         return xml;
      }

      AnsiString ResponseForMember(Context &context, const Member &member, const PropertyRequest &request)
      {
         Resource resource;
         resource.kind = KindObject;
         resource.object = member.record;
         resource.href = member.href;
         return ResponseFor(context, resource, &member, request);
      }

      AnsiString NotFoundResponse(const AnsiString &href)
      {
         return " <D:response>\r\n  " + Href(href) + "\r\n  <D:status>HTTP/1.1 404 Not Found</D:status>\r\n </D:response>\r\n";
      }

      // The D:prop element of a request. The children a calendar-data may
      // carry - comp, expand, limit-recurrence-set - are read past: the
      // object is served whole, as stored.
      void ReadPropertyRequest(const XmlElement *prop, PropertyRequest &request)
      {
         request.mode = ModeProp;
         request.names.clear();
         if (!prop)
            return;

         for (size_t i = 0; i < prop->children.size(); i++)
         {
            PropertyName name;
            name.ns = prop->children[i].ns;
            name.name = prop->children[i].name;
            request.names.push_back(name);
         }
      }

      //------------------------------------------------------------------------
      // The methods
      //------------------------------------------------------------------------

      bool ReadBody(const HttpRequest &request, XmlElement &root, AnsiString &problem)
      {
         XmlReader reader(request.body);
         return reader.Read(root, problem);
      }

      int DepthOf(const HttpRequest &request)
      {
         AnsiString depth = Lower(Trimmed(request.Header("depth")));
         if (depth == "0")
            return 0;
         return 1;
      }

      HttpResponse HandleOptions(const Resource &resource)
      {
         bool object = resource.kind == KindObject || resource.kind == KindObjectSlot;
         return Respond(200, "text/plain", "", object ? AllowObject : AllowCollection);
      }

      // The displayname and calendar-color a MKCALENDAR, extended MKCOL or
      // PROPPATCH body carries under DAV:set/DAV:prop.
      void ReadCalendarProperties(const XmlElement &root, String &displayName, String &color, bool &hasDisplayName, bool &hasColor, AnsiString &others)
      {
         hasDisplayName = false;
         hasColor = false;
         for (size_t i = 0; i < root.children.size(); i++)
         {
            const XmlElement &action = root.children[i];
            if (!action.Is(NsDav, "set") && !action.Is(NsDav, "remove"))
               continue;
            const XmlElement *prop = action.Child(NsDav, "prop");
            if (!prop)
               continue;
            for (size_t j = 0; j < prop->children.size(); j++)
            {
               const XmlElement &value = prop->children[j];
               if (value.Is(NsDav, "displayname"))
               {
                  hasDisplayName = true;
                  displayName = action.Is(NsDav, "set") ? Wide(Trimmed(value.text)) : String();
               }
               else if (value.Is(NsApple, "calendar-color"))
               {
                  hasColor = true;
                  color = action.Is(NsDav, "set") ? Wide(Trimmed(value.text)) : String();
               }
               else
               {
                  PropertyName name;
                  name.ns = value.ns;
                  name.name = value.name;
                  others += PropertyElement(name, "");
               }
            }
         }
      }

      bool IsColor(const String &color)
      {
         if (color.IsEmpty())
            return true;
         // #RRGGBB, or #RRGGBBAA as Apple's clients write it.
         if ((color.GetLength() != 7 && color.GetLength() != 9) || color.c_str()[0] != '#')
            return false;
         for (int i = 1; i < color.GetLength(); i++)
         {
            wchar_t c = color.c_str()[i];
            bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!hex)
               return false;
         }
         return true;
      }

      // MKCALENDAR (RFC 4791 section 5.3.1), and MKCOL with a calendar
      // resourcetype (RFC 5689): a calendar under the caller's home, its
      // display name and colour from the body when it carries them.
      HttpResponse HandleMakeCalendar(Context &context, const Resource &resource)
      {
         if (resource.kind == KindCalendar)
            return Text(405, "a calendar of that name exists", AllowCollection);
         if (resource.kind != KindCalendarSlot)
            return Text(403, "a calendar can be made under the home only: /dav/calendars/<address>/<name>/");

         String displayName;
         String color;
         bool hasDisplayName = false;
         bool hasColor = false;
         AnsiString others;
         if (!Trimmed(context.request.body).IsEmpty())
         {
            XmlElement root;
            AnsiString problem;
            if (!ReadBody(context.request, root, problem))
               return Text(400, "the body is not well-formed XML: " + problem);
            if (!root.Is(NsCalDav, "mkcalendar") && !root.Is(NsDav, "mkcol"))
               return Text(400, "the body must be a CALDAV:mkcalendar or a DAV:mkcol element");
            ReadCalendarProperties(root, displayName, color, hasDisplayName, hasColor, others);
         }

         if (!IsColor(color))
            return Text(403, "calendar-color must be #RRGGBB");

         CalendarRecord created;
         if (!CalendarStore::CreateCalendar(context.account->GetID(), Wide(resource.slotName), displayName, color, created))
            return Text(500, "the calendar could not be made");

         LOG_DEBUG("CalDAV: " + String(context.addressUtf8) + " made the calendar " + String(created.name) + ".");
         return Respond(201, "text/plain", "", "Location: " + context.homeHref + PercentEncodeSegment(Utf8(created.name)) + "/\r\n");
      }

      HttpResponse HandlePropfind(Context &context, const Resource &resource)
      {
         if (resource.kind == KindObjectSlot || resource.kind == KindCalendarSlot)
            return NotFound();

         PropertyRequest request;
         if (!Trimmed(context.request.body).IsEmpty())
         {
            XmlElement root;
            AnsiString problem;
            if (!ReadBody(context.request, root, problem))
               return Text(400, "the PROPFIND body is not well-formed XML: " + problem);
            if (!root.Is(NsDav, "propfind"))
               return Text(400, "the PROPFIND body must be a DAV:propfind element");

            if (root.Child(NsDav, "propname"))
               request.mode = ModePropName;
            else if (const XmlElement *prop = root.Child(NsDav, "prop"))
               ReadPropertyRequest(prop, request);
            else
            {
               request.mode = ModeAllProp;
               if (const XmlElement *include = root.Child(NsDav, "include"))
               {
                  PropertyRequest included;
                  ReadPropertyRequest(include, included);
                  request.names = included.names;
               }
            }
         }

         int depth = DepthOf(context.request);

         AnsiString xml = MultistatusOpen();

         const Member *self = nullptr;
         Member selfMember;
         if (resource.kind == KindObject)
         {
            FillMember(context, resource.object, selfMember);
            self = &selfMember;
         }
         xml += ResponseFor(context, resource, self, request);

         if (depth == 1)
         {
            switch (resource.kind)
            {
            case KindHomeCollection:
            {
               Resource home;
               home.kind = KindHome;
               home.href = context.homeHref;
               xml += ResponseFor(context, home, nullptr, request);
               break;
            }
            case KindHome:
            {
               std::vector<CalendarRecord> own;
               if (!CalendarStore::ListCalendars(context.account->GetID(), own))
                  return Text(500, "the calendars could not be read");
               for (size_t i = 0; i < own.size(); i++)
               {
                  Resource calendar;
                  FillCalendarResource(context, own[i], false, true, calendar);
                  xml += ResponseFor(context, calendar, nullptr, request);
               }

               std::vector<CalendarRecord> sharedCalendars;
               std::vector<CalendarShareRecord> shares;
               if (CalendarStore::ListSharedWith(context.account->GetID(), sharedCalendars, shares))
               {
                  for (size_t i = 0; i < sharedCalendars.size(); i++)
                  {
                     Resource calendar;
                     FillCalendarResource(context, sharedCalendars[i], true, shares[i].write, calendar);
                     xml += ResponseFor(context, calendar, nullptr, request);
                  }
               }
               break;
            }
            case KindCalendar:
            {
               if (!EnsureListed(context))
                  return Text(500, "the calendar could not be read");
               for (size_t i = 0; i < context.members.size(); i++)
               {
                  if (xml.GetLength() > MaxMultistatusBytes)
                     return Text(507, "the listing would be too large; use a sync-collection or multiget report");
                  xml += ResponseForMember(context, context.members[i], request);
               }
               break;
            }
            default:
               break;
            }
         }

         xml += "</D:multistatus>\r\n";
         return Xml(207, xml);
      }

      // Nothing here is writable by PROPPATCH: the one calendar has the name
      // it has, and no colour or order is kept. Each property asked for is
      // answered 403 in the 207, per RFC 4918 section 9.2.
      HttpResponse HandleProppatch(Context &context, const Resource &resource)
      {
         if (resource.kind == KindObjectSlot || resource.kind == KindCalendarSlot)
            return NotFound();

         XmlElement root;
         AnsiString problem;
         if (!ReadBody(context.request, root, problem))
            return Text(400, "the PROPPATCH body is not well-formed XML: " + problem);
         if (!root.Is(NsDav, "propertyupdate"))
            return Text(400, "the PROPPATCH body must be a DAV:propertyupdate element");

         // The caller's own calendar: its display name and colour are the
         // two properties a client may set; anything else asked for is 403.
         if (resource.kind == KindCalendar && !resource.shared)
         {
            String displayName;
            String color;
            bool hasDisplayName = false;
            bool hasColor = false;
            AnsiString others;
            ReadCalendarProperties(root, displayName, color, hasDisplayName, hasColor, others);

            AnsiString accepted;
            AnsiString failed;
            if (hasDisplayName || hasColor)
            {
               if (hasColor && !IsColor(color))
                  failed += PropertyElement(PropertyName(NsApple, "calendar-color"), "");
               else if (CalendarStore::UpdateCalendar(context.account->GetID(), resource.calendar.id, hasDisplayName ? displayName : resource.calendar.displayName, hasColor ? color : resource.calendar.color))
               {
                  if (hasDisplayName)
                     accepted += PropertyElement(PropertyName(NsDav, "displayname"), "");
                  if (hasColor)
                     accepted += PropertyElement(PropertyName(NsApple, "calendar-color"), "");
               }
               else
               {
                  if (hasDisplayName)
                     failed += PropertyElement(PropertyName(NsDav, "displayname"), "");
                  if (hasColor)
                     failed += PropertyElement(PropertyName(NsApple, "calendar-color"), "");
               }
            }

            AnsiString xml = MultistatusOpen();
            xml += " <D:response>\r\n  " + Href(resource.href) + "\r\n";
            if (!accepted.IsEmpty())
               xml += "  <D:propstat><D:prop>" + accepted + "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\r\n";
            if (!failed.IsEmpty())
               xml += "  <D:propstat><D:prop>" + failed + "</D:prop><D:status>HTTP/1.1 500 Internal Server Error</D:status></D:propstat>\r\n";
            if (!others.IsEmpty())
               xml += "  <D:propstat><D:prop>" + others + "</D:prop><D:status>HTTP/1.1 403 Forbidden</D:status>"
                      "<D:responsedescription>The display name and the colour of a calendar can be set; nothing else can.</D:responsedescription></D:propstat>\r\n";
            xml += " </D:response>\r\n</D:multistatus>\r\n";
            return Xml(207, xml);
         }

         AnsiString refused;
         for (size_t i = 0; i < root.children.size(); i++)
         {
            const XmlElement &action = root.children[i];
            if (!action.Is(NsDav, "set") && !action.Is(NsDav, "remove"))
               continue;
            const XmlElement *prop = action.Child(NsDav, "prop");
            if (!prop)
               continue;
            for (size_t j = 0; j < prop->children.size(); j++)
            {
               PropertyName name;
               name.ns = prop->children[j].ns;
               name.name = prop->children[j].name;
               refused += PropertyElement(name, "");
            }
         }

         AnsiString xml = MultistatusOpen();
         xml += " <D:response>\r\n  " + Href(resource.href) + "\r\n";
         xml += "  <D:propstat><D:prop>" + refused + "</D:prop><D:status>HTTP/1.1 403 Forbidden</D:status>"
                "<D:responsedescription>The properties of this resource are fixed; none can be set or removed.</D:responsedescription></D:propstat>\r\n";
         xml += " </D:response>\r\n</D:multistatus>\r\n";
         return Xml(207, xml);
      }

      HttpResponse HandleGet(Context &context, const Resource &resource, bool head)
      {
         if (resource.kind == KindObjectSlot)
            return NotFound();

         if (resource.kind != KindObject)
            return MethodNotAllowed(resource);

         Member member;
         FillMember(context, resource.object, member);

         AnsiString headers = "ETag: " + member.etag + "\r\n";

         AnsiString ifNoneMatch = Trimmed(context.request.Header("if-none-match"));
         if (!ifNoneMatch.IsEmpty() && ifNoneMatch != "*")
         {
            std::vector<AnsiString> tags = StringParser::SplitString(ifNoneMatch, ",");
            for (size_t i = 0; i < tags.size(); i++)
            {
               AnsiString tag = Trimmed(tags[i]);
               if (tag.StartsWith("W/"))
                  tag = tag.Mid(2);
               if (tag == member.etag)
                  return Respond(304, ContentTypeOf(member), "", headers);
            }
         }

         return Respond(200, ContentTypeOf(member), head ? AnsiString("") : member.data, headers);
      }

      bool IsCalendarContentType(const AnsiString &contentType)
      {
         AnsiString type = Lower(Trimmed(contentType));
         int semicolon = type.Find(";");
         if (semicolon >= 0)
            type = Trimmed(type.Mid(0, semicolon));
         return type.IsEmpty() || type == "text/calendar";
      }

      bool IsUsableName(const AnsiString &name)
      {
         if (name.IsEmpty() || name.GetLength() > CalendarStore::MaximumUriLength)
            return false;
         for (int i = 0; i < name.GetLength(); i++)
         {
            unsigned char c = static_cast<unsigned char>(name[i]);
            if (c < 0x20 || c == 0x7f || c == '/' || c == '\\')
               return false;
         }
         return name != "." && name != "..";
      }

      HttpResponse HandlePut(Context &context, const Resource &resource)
      {
         if (resource.kind != KindObject && resource.kind != KindObjectSlot)
            return MethodNotAllowed(resource);

         const HttpRequest &request = context.request;

         if (!IsCalendarContentType(request.Header("content-type")))
            return DavError(415, "<C:supported-calendar-data/>", "The body must be an iCalendar object: text/calendar.");

         if (request.body.GetLength() > MaxObjectBytes)
            return DavError(403, "<C:max-resource-size/>", "The object is larger than " + IntText(MaxObjectBytes) + " bytes.");

         if (resource.kind == KindObjectSlot && !IsUsableName(resource.slotName))
            return Text(403, "the resource name must be one path segment of printable characters, at most 255 bytes");

         ICalComponent tree;
         AnsiString problem;
         if (!ICalendar::Parse(request.body, tree, problem))
            return DavError(403, "<C:valid-calendar-data/>", "The body is not one iCalendar object: " + problem + ".");

         ICalendar::Summary summary;
         if (!ICalendar::Summarize(tree, summary, problem))
         {
            // The precondition that names the fault: a component kind this
            // calendar does not hold, an object that is not one resource's
            // worth, or data the module cannot place.
            AnsiString precondition = "<C:valid-calendar-data/>";
            if (problem.Find("VEVENT and VTODO") >= 0)
               precondition = "<C:supported-calendar-component/>";
            else if (problem.Find("UID") >= 0 || problem.Find("RECURRENCE-ID") >= 0 || problem.Find("more than one") >= 0)
               precondition = "<C:valid-calendar-object-resource/>";
            return DavError(403, precondition, "The object cannot be stored: " + problem + ".");
         }

         if (!EnsureCalendar(context))
            return Text(500, "the calendar could not be read");
         if (!context.canWrite)
            return Text(403, "this calendar is shared with you read-only");

         __int64 accountId = context.storeAccountId;
         __int64 calendarId = context.calendar.id;

         // A room's or a piece of equipment's calendar is written under the
         // lock its bookings are decided under (ResourceBooking.h), and the
         // UID's uniqueness and the ETag precondition are read under it too, so
         // that a decision written in between is never overwritten on a read
         // made before it. Held until the write is done, and no longer: a
         // delegate's decline is mailed after it is released.
         std::unique_ptr<ResourceCalendarGuard> guard(new ResourceCalendarGuard(accountId));

         // One UID, one resource (RFC 4791 section 5.3.2.1). The other
         // resource is named, so a client that lost track of where it put an
         // event finds it rather than storing a second copy.
         CalendarObjectRecord other;
         if (CalendarStore::FindByUid(accountId, calendarId, Wide(summary.uid), other) &&
             (resource.kind != KindObject || other.id != resource.object.id))
         {
            return DavError(409, "<C:no-uid-conflict>" + Href(ObjectHref(context, other)) + "</C:no-uid-conflict>",
               "The UID " + summary.uid + " is already the object at " + ObjectHref(context, other) + "; an object is one UID in a calendar.");
         }

         // The object is kept as sent: what a GET returns is what the client
         // wrote, so the ETag answered here is the ETag of the stored bytes.
         CalendarObjectRecord record;
         record.uid = Wide(summary.uid);
         record.component = Wide(summary.component);
         record.data = Wide(request.body);
         record.etag = Wide(EtagOf(request.body));
         record.start = summary.start;
         record.end = summary.end;
         record.first = summary.first;
         record.last = summary.last;

         if (resource.kind == KindObject)
         {
            // The ETag as it is now, under the lock, not as it was when the
            // request was resolved.
            CalendarObjectRecord current;
            const bool currentRead = CalendarStore::FindById(resource.object.id, current);
            AnsiString currentEtag = currentRead && !current.etag.IsEmpty() ? Utf8(current.etag) : Utf8(resource.object.etag);
            if (!PreconditionsHold(request, currentEtag))
               return Text(412, "precondition failed: the object has changed since it was read, or If-None-Match: * named an existing resource",
                  "ETag: " + currentEtag + "\r\n");

            // A room holds what it agreed to and nothing it refused: a booking
            // declined from a CalDAV client - the room's PARTSTAT written as
            // DECLINED - leaves its calendar, as one declined through the REST
            // reply or by the room itself does, since everything its calendar
            // holds that is not cancelled is busy. Anybody who may write to the
            // calendar may take a booking out of it (a DELETE does as much).
            //
            // Its organiser is told only when a delegate declined it - a share
            // with delegate set, as the REST reply requires; a plain write share
            // does not answer for the room, and the room itself never signs in
            // - and only the organiser the room recorded when it accepted the
            // booking, never one a PUT wrote (ResourceBooking::
            // AnswerableOrganiser). A CalDAV client schedules nothing here, and
            // the organiser would otherwise still hold the room's acceptance.
            std::shared_ptr<const Account> owner = CacheContainer::Instance()->GetAccount(accountId);
            if (owner && owner->IsResource() && ResourceBooking::DeclinedBy(tree, *owner))
            {
               ICalComponent held;
               AnsiString heldProblem;
               const bool heldRead = currentRead && ICalendar::Parse(Utf8(current.data), held, heldProblem);

               CalendarShareRecord share;
               const bool delegated = context.account &&
                  (context.account->GetID() == accountId ||
                   (CalendarStore::ShareOf(calendarId, context.account->GetID(), share) && share.delegate));

               __int64 removedToken = 0;
               if (!CalendarStore::Delete(accountId, calendarId, resource.object.id, removedToken))
                  return Text(500, "the declined booking could not be taken out of the calendar");
               if (delegated)
               {
                  LOG_DEBUG("CalDAV: " + String(context.addressUtf8) + " declined " + String(summary.uid) + " for " + owner->GetAddress() + ", and it left the calendar.");
               }
               else
               {
                  LOG_DEBUG("CalDAV: " + String(context.addressUtf8) + " declined " + String(summary.uid) + " for " + owner->GetAddress() +
                     ", and it left the calendar; not a delegate, so nobody was told.");
               }

               // Whom to tell is read under the room's lock, and the room's
               // record of the booking goes with it; the mail, built from the
               // booking as the room held it before this PUT, is queued once
               // the lock is released.
               const AnsiString organiser = heldRead && delegated ? ResourceBooking::AnswerableOrganiser(*owner, held) : AnsiString();
               const String declinedUid = currentRead && !current.uid.IsEmpty() ? current.uid : record.uid;
               ResourceBooking::ForgetDecision(accountId, declinedUid);
               guard.reset();
               if (heldRead && delegated)
                  ResourceBooking::TellOrganiserOfDecline(owner, context.account, held, organiser);
               return Respond(204, "text/plain", "");
            }

            __int64 token = 0;
            if (!CalendarStore::Update(accountId, calendarId, resource.object.id, record, token))
               return Text(500, "the object could not be saved");

            // An object written back under another UID is another booking: the
            // room's record of the one it replaced goes.
            if (currentRead && !current.uid.IsEmpty() && current.uid != record.uid)
               ResourceBooking::ForgetDecision(accountId, current.uid);

            LOG_DEBUG("CalDAV: " + String(context.addressUtf8) + " updated " + String(summary.component) + " " + String(summary.uid) + ".");
            return Respond(204, "text/plain", "", "ETag: " + Utf8(record.etag) + "\r\n");
         }

         if (!PreconditionsHold(request, ""))
            return Text(412, "precondition failed: nothing exists at this URL for If-Match to match");

         record.uri = Wide(resource.slotName);
         CalendarObjectRecord inserted;
         __int64 token = 0;
         if (!CalendarStore::Insert(accountId, calendarId, record, inserted, token))
         {
            // Two clients creating the same name at once: the second is
            // refused by the store, and answered as if the first had come a
            // moment earlier.
            CalendarObjectRecord existing;
            if (CalendarStore::FindByUri(accountId, calendarId, record.uri, existing))
               return Text(412, "precondition failed: an object was created at this URL a moment ago", "ETag: " + Utf8(existing.etag) + "\r\n");

            // A name this database compares as equal to an existing one - a
            // different case, or on MySQL different accents - cannot be created
            // beside it. Said, rather than a 500 that says nothing.
            CalendarObjectRecord twin;
            if (CalendarStore::FindCollationTwin(accountId, calendarId, record.uri, twin))
               return Text(409, "an object whose name differs from this one only in case or accents already exists in this calendar, and this server's database treats the two names as one: " + Utf8(twin.uri));
            return Text(500, "the object could not be saved");
         }

         AnsiString href = ObjectHref(context, inserted);
         LOG_DEBUG("CalDAV: " + String(context.addressUtf8) + " created " + String(summary.component) + " " + String(summary.uid) + " at " + String(href) + ".");
         return Respond(201, "text/plain", "", "ETag: " + Utf8(inserted.etag) + "\r\nLocation: " + href + "\r\n");
      }

      HttpResponse HandleDelete(Context &context, const Resource &resource)
      {
         if (resource.kind == KindObjectSlot || resource.kind == KindCalendarSlot)
            return NotFound();

         if (resource.kind == KindCalendar)
         {
            // A calendar shared with the caller: the share is given back and
            // the calendar stays its owner's. The caller's own: gone with its
            // objects, except the default, which every client expects to find.
            if (resource.shared)
            {
               // A share through a group is the group's, not any member's.
               CalendarShareRecord direct;
               if (!CalendarStore::DirectShareOf(resource.calendar.id, context.account->GetID(), direct))
                  return Text(403, "this calendar is shared with a group you are a member of; the share is the group's, and only its owner takes it back");
               if (!CalendarStore::Unshare(resource.calendar.id, context.account->GetID()))
                  return Text(500, "the share could not be removed");
               // The caller's own row is gone; a group's row may still reach it.
               CalendarShareRecord still;
               if (CalendarStore::ShareOf(resource.calendar.id, context.account->GetID(), still) && still.groupId != 0)
                  return Text(403, "your own share is given back, but this calendar is still shared with a group you are a member of; the group's share is the group's, and only its owner takes it back");
               LOG_DEBUG("CalDAV: " + String(context.addressUtf8) + " left the shared calendar " + String(Int64Text(resource.calendar.id)) + ".");
               return Respond(204, "text/plain", "");
            }
            if (resource.calendar.name.CompareNoCase(String(CalendarStore::DefaultName)) == 0)
               return Text(403, "the calendar Calendar is the account's default and cannot be deleted; the collections above it cannot either");
            if (!CalendarStore::DeleteCalendar(context.account->GetID(), resource.calendar.id))
               return Text(500, "the calendar could not be deleted");
            // A room's records of the bookings that went with it (an account
            // signs in to delete one only while it is a person).
            ResourceBooking::ForgetGoneBookings(context.account->GetID());
            LOG_DEBUG("CalDAV: " + String(context.addressUtf8) + " deleted the calendar " + String(resource.calendar.name) + ".");
            return Respond(204, "text/plain", "");
         }

         if (resource.kind != KindObject)
            return Text(403, "the collections above a calendar cannot be deleted");

         if (!context.canWrite)
            return Text(403, "this calendar is shared with you read-only");

         AnsiString currentEtag = Utf8(resource.object.etag);
         if (!PreconditionsHold(context.request, currentEtag))
            return Text(412, "precondition failed: the object has changed since it was read", "ETag: " + currentEtag + "\r\n");

         if (!EnsureCalendar(context))
            return Text(500, "the calendar could not be read");

         __int64 token = 0;
         ResourceCalendarGuard guard(context.storeAccountId);
         if (!CalendarStore::Delete(context.storeAccountId, context.calendar.id, resource.object.id, token))
            return NotFound();
         ResourceBooking::ForgetDecision(context.storeAccountId, resource.object.uid);

         LOG_DEBUG("CalDAV: " + String(context.addressUtf8) + " deleted " + String(Utf8(resource.object.uri)) + ".");
         return Respond(204, "text/plain", "");
      }

      //------------------------------------------------------------------------
      // The reports
      //------------------------------------------------------------------------

      void DefaultReportProperties(const XmlElement &report, PropertyRequest &request)
      {
         if (const XmlElement *prop = report.Child(NsDav, "prop"))
            ReadPropertyRequest(prop, request);
         else
         {
            request.mode = ModeProp;
            request.names.push_back(PropertyName(NsDav, "getetag"));
            request.names.push_back(PropertyName(NsCalDav, "calendar-data"));
         }
      }

      HttpResponse HandleMultiget(Context &context, const XmlElement &report)
      {
         PropertyRequest request;
         DefaultReportProperties(report, request);

         if (!EnsureListed(context))
            return Text(500, "the calendar could not be read");

         AnsiString xml = MultistatusOpen();
         size_t hrefs = 0;
         std::set<AnsiString> answered;
         for (size_t i = 0; i < report.children.size(); i++)
         {
            if (!report.children[i].Is(NsDav, "href"))
               continue;
            if (++hrefs > MaxMultigetHrefs)
               return Text(400, "too many hrefs in one multiget");
            if (xml.GetLength() > MaxMultistatusBytes)
               return Text(507, "the response would be too large; ask for fewer objects in one multiget");

            AnsiString href = Trimmed(report.children[i].text);
            int scheme = href.Find("://");
            if (scheme >= 0)
            {
               int slash = href.Find("/", scheme + 3);
               href = slash >= 0 ? href.Mid(slash) : AnsiString("/");
            }

            AnsiString wanted = PercentDecode(href);
            if (!answered.insert(wanted).second)
               continue;

            const Member *found = nullptr;
            for (size_t j = 0; !found && j < context.members.size(); j++)
            {
               if (PercentDecode(context.members[j].href) == wanted)
                  found = &context.members[j];
            }

            if (found)
               xml += ResponseForMember(context, *found, request);
            else
               xml += NotFoundResponse(href);
         }

         xml += "</D:multistatus>\r\n";
         return Xml(207, xml);
      }

      // The filter of a calendar-query, as a tree.
      struct TextMatch
      {
         TextMatch() : negate(false), octet(false) { }

         AnsiString text;
         bool negate;
         bool octet;   // i;octet: case-sensitive
      };

      struct PropFilter
      {
         PropFilter() : isNotDefined(false), hasTimeRange(false), rangeStart(0), rangeEnd(0) { }

         AnsiString name;   // upper-cased property name
         bool isNotDefined;
         std::vector<TextMatch> textMatches;
         bool hasTimeRange;
         __int64 rangeStart;
         __int64 rangeEnd;
      };

      struct CompFilter
      {
         CompFilter() : isNotDefined(false), hasTimeRange(false), rangeStart(0), rangeEnd(ICalendar::Forever) { }

         AnsiString name;   // upper-cased component name
         bool isNotDefined;
         bool hasTimeRange;
         __int64 rangeStart;
         __int64 rangeEnd;
         std::vector<PropFilter> propFilters;
         std::vector<CompFilter> compFilters;
      };

      bool ReadTimeRange(const XmlElement &element, __int64 &start, __int64 &end, AnsiString &problem)
      {
         AnsiString startText = Trimmed(element.Attribute("start"));
         AnsiString endText = Trimmed(element.Attribute("end"));
         start = 0;
         end = ICalendar::Forever;
         if (!startText.IsEmpty() && !ICalendar::ParseUtc(startText, start))
         {
            problem = "the time-range start is not a UTC date-time: " + startText;
            return false;
         }
         if (!endText.IsEmpty() && !ICalendar::ParseUtc(endText, end))
         {
            problem = "the time-range end is not a UTC date-time: " + endText;
            return false;
         }
         if (end <= start)
         {
            problem = "the time-range end is not after its start";
            return false;
         }
         return true;
      }

      bool ReadCompFilter(const XmlElement &element, CompFilter &filter, int depth, AnsiString &problem)
      {
         if (depth > 4)
         {
            problem = "comp-filters nested too deeply";
            return false;
         }
         filter.name = Trimmed(element.Attribute("name"));
         filter.name.ToUpper();
         if (filter.name.IsEmpty())
         {
            problem = "a comp-filter without a name";
            return false;
         }

         for (size_t i = 0; i < element.children.size(); i++)
         {
            const XmlElement &child = element.children[i];
            if (child.Is(NsCalDav, "is-not-defined"))
               filter.isNotDefined = true;
            else if (child.Is(NsCalDav, "time-range"))
            {
               if (!ReadTimeRange(child, filter.rangeStart, filter.rangeEnd, problem))
                  return false;
               filter.hasTimeRange = true;
            }
            else if (child.Is(NsCalDav, "comp-filter"))
            {
               CompFilter nested;
               if (!ReadCompFilter(child, nested, depth + 1, problem))
                  return false;
               filter.compFilters.push_back(nested);
            }
            else if (child.Is(NsCalDav, "prop-filter"))
            {
               PropFilter propFilter;
               propFilter.name = Trimmed(child.Attribute("name"));
               propFilter.name.ToUpper();
               if (propFilter.name.IsEmpty())
               {
                  problem = "a prop-filter without a name";
                  return false;
               }
               for (size_t j = 0; j < child.children.size(); j++)
               {
                  const XmlElement &condition = child.children[j];
                  if (condition.Is(NsCalDav, "is-not-defined"))
                     propFilter.isNotDefined = true;
                  else if (condition.Is(NsCalDav, "text-match"))
                  {
                     TextMatch match;
                     match.text = condition.text;
                     match.negate = Lower(condition.Attribute("negate-condition")) == "yes";
                     match.octet = Lower(condition.Attribute("collation")) == "i;octet";
                     propFilter.textMatches.push_back(match);
                  }
                  else if (condition.Is(NsCalDav, "time-range"))
                  {
                     if (!ReadTimeRange(condition, propFilter.rangeStart, propFilter.rangeEnd, problem))
                        return false;
                     propFilter.hasTimeRange = true;
                  }
                  // param-filter is not evaluated: a filter that cannot be
                  // applied does not narrow the answer.
               }
               filter.propFilters.push_back(propFilter);
            }
         }

         if (filter.propFilters.size() + filter.compFilters.size() > 64)
         {
            problem = "a comp-filter with more than 64 children";
            return false;
         }
         return true;
      }

      bool TextMatches(const TextMatch &match, const AnsiString &value)
      {
         AnsiString haystack = match.octet ? value : Lower(value);
         AnsiString needle = match.octet ? match.text : Lower(match.text);
         bool result = haystack.Find(needle) >= 0;
         return match.negate ? !result : result;
      }

      // The values of a property across the object's components of a kind,
      // unescaped, for a text-match.
      void PropertyValues(const ICalComponent &tree, const AnsiString &componentName, const AnsiString &propertyName, std::vector<const ICalProperty *> &out)
      {
         for (size_t i = 0; i < tree.children.size(); i++)
         {
            const ICalComponent &component = tree.children[i];
            if (component.name != componentName)
               continue;
            for (size_t p = 0; p < component.properties.size(); p++)
            {
               if (component.properties[p].name == propertyName)
                  out.push_back(&component.properties[p]);
            }
         }
      }

      bool PropFilterMatches(const PropFilter &filter, const ICalComponent &tree, const AnsiString &componentName)
      {
         std::vector<const ICalProperty *> properties;
         PropertyValues(tree, componentName, filter.name, properties);

         if (filter.isNotDefined)
            return properties.empty();
         if (properties.empty())
            return false;

         if (filter.hasTimeRange)
         {
            bool inRange = false;
            for (size_t i = 0; !inRange && i < properties.size(); i++)
            {
               ICalTime when;
               AnsiString problem;
               if (ICalendar::ParseTime(*properties[i], &tree, when, problem))
                  inRange = when.instant >= filter.rangeStart && when.instant < filter.rangeEnd;
            }
            if (!inRange)
               return false;
         }

         for (size_t i = 0; i < filter.textMatches.size(); i++)
         {
            bool matched = false;
            for (size_t j = 0; !matched && j < properties.size(); j++)
               matched = TextMatches(filter.textMatches[i], ICalendar::Unescape(properties[j]->value));
            if (!matched)
               return false;
         }

         return true;
      }

      // Whether a comp-filter below VCALENDAR matches the object.
      bool ComponentFilterMatches(const CompFilter &filter, Member &member)
      {
         bool present = false;
         for (size_t i = 0; !present && i < member.tree.children.size(); i++)
            present = member.tree.children[i].name == filter.name;

         if (filter.isNotDefined)
            return !present;
         if (!present)
            return false;

         if (filter.hasTimeRange)
         {
            if (filter.name == "VEVENT" || filter.name == "VTODO")
            {
               if (!ICalendar::OverlapsTimeRange(member.tree, filter.rangeStart, filter.rangeEnd))
                  return false;
            }
            // A time-range on VALARM or VTIMEZONE is not evaluated.
         }

         for (size_t i = 0; i < filter.propFilters.size(); i++)
         {
            if (!PropFilterMatches(filter.propFilters[i], member.tree, filter.name))
               return false;
         }

         // Nested comp-filters (a VALARM under a VEVENT): presence only.
         for (size_t i = 0; i < filter.compFilters.size(); i++)
         {
            const CompFilter &nested = filter.compFilters[i];
            bool nestedPresent = false;
            for (size_t c = 0; !nestedPresent && c < member.tree.children.size(); c++)
            {
               const ICalComponent &component = member.tree.children[c];
               if (component.name != filter.name)
                  continue;
               nestedPresent = component.Child(nested.name.c_str()) != nullptr;
            }
            if (nested.isNotDefined ? nestedPresent : !nestedPresent)
               return false;
         }

         return true;
      }

      bool FilterMatches(const CompFilter &root, Member &member)
      {
         // The root names VCALENDAR; is-not-defined there can match nothing.
         if (root.isNotDefined)
            return false;
         if (!ParsedMember(member))
            return false;

         for (size_t i = 0; i < root.propFilters.size(); i++)
         {
            // A prop-filter on the VCALENDAR itself: its own properties.
            const PropFilter &filter = root.propFilters[i];
            const ICalProperty *property = member.tree.Property(filter.name.c_str());
            if (filter.isNotDefined ? property != nullptr : property == nullptr)
               return false;
            for (size_t j = 0; property && j < filter.textMatches.size(); j++)
            {
               if (!TextMatches(filter.textMatches[j], ICalendar::Unescape(property->value)))
                  return false;
            }
         }

         for (size_t i = 0; i < root.compFilters.size(); i++)
         {
            if (!ComponentFilterMatches(root.compFilters[i], member))
               return false;
         }
         return true;
      }

      HttpResponse HandleQuery(Context &context, const XmlElement &report)
      {
         PropertyRequest request;
         DefaultReportProperties(report, request);

         CompFilter root;
         bool haveFilter = false;
         if (const XmlElement *filterElement = report.Child(NsCalDav, "filter"))
         {
            const XmlElement *top = filterElement->Child(NsCalDav, "comp-filter");
            if (!top)
               return DavError(403, "<C:valid-filter/>", "The filter must hold one comp-filter for VCALENDAR.");
            AnsiString problem;
            if (!ReadCompFilter(*top, root, 0, problem))
               return DavError(403, "<C:valid-filter/>", "The filter cannot be applied: " + problem + ".");
            if (root.name != "VCALENDAR")
               return DavError(403, "<C:valid-filter/>", "The top comp-filter must name VCALENDAR, not " + root.name + ".");
            haveFilter = true;
         }

         if (!EnsureCalendar(context))
            return Text(500, "the calendar could not be read");

         // The store narrows by span when the component filter carries a
         // time-range: only objects whose instances could touch the range
         // are read and expanded.
         __int64 narrowStart = 0;
         __int64 narrowEnd = ICalendar::Forever;
         bool narrow = false;
         for (size_t i = 0; haveFilter && i < root.compFilters.size(); i++)
         {
            const CompFilter &filter = root.compFilters[i];
            if ((filter.name == "VEVENT" || filter.name == "VTODO") && filter.hasTimeRange && !filter.isNotDefined)
            {
               narrowStart = filter.rangeStart;
               narrowEnd = filter.rangeEnd;
               narrow = true;
               break;
            }
         }

         std::vector<CalendarObjectRecord> records;
         bool read = narrow
            ? CalendarStore::ListInRange(context.storeAccountId, context.calendar.id, narrowStart - 86400, narrowEnd + 86400, records)
            : CalendarStore::List(context.storeAccountId, context.calendar.id, records);
         if (!read)
            return Text(500, "the calendar could not be read");
         context.listed = true;
         FillMembers(context, records);

         AnsiString xml = MultistatusOpen();
         for (size_t i = 0; i < context.members.size(); i++)
         {
            if (haveFilter && !FilterMatches(root, context.members[i]))
               continue;
            if (xml.GetLength() > MaxMultistatusBytes)
               return Text(507, "the response would be too large; narrow the query");
            xml += ResponseForMember(context, context.members[i], request);
         }

         xml += "</D:multistatus>\r\n";
         return Xml(207, xml);
      }

      HttpResponse HandleSyncCollection(Context &context, const XmlElement &report)
      {
         PropertyRequest request;
         if (const XmlElement *prop = report.Child(NsDav, "prop"))
            ReadPropertyRequest(prop, request);
         else
         {
            request.mode = ModeProp;
            request.names.push_back(PropertyName(NsDav, "getetag"));
         }

         AnsiString presented;
         if (const XmlElement *token = report.Child(NsDav, "sync-token"))
            presented = Trimmed(token->text);

         if (!EnsureCalendar(context))
            return Text(500, "the calendar could not be read");
         __int64 current = context.calendar.syncToken;

         AnsiString xml = MultistatusOpen();

         if (presented.IsEmpty())
         {
            if (!EnsureListed(context))
               return Text(500, "the calendar could not be read");
            for (size_t i = 0; i < context.members.size(); i++)
            {
               if (xml.GetLength() > MaxMultistatusBytes)
                  return Text(507, "the response would be too large");
               xml += ResponseForMember(context, context.members[i], request);
            }
         }
         else
         {
            const char *prefix = "urn:x-hmailserver:caldav-sync:";
            __int64 since = -1;
            if (presented.StartsWith(prefix))
            {
               AnsiString digits = presented.Mid(static_cast<int>(strlen(prefix)));
               bool numeric = !digits.IsEmpty() && digits.GetLength() < 19;
               for (int i = 0; numeric && i < digits.GetLength(); i++)
                  numeric = digits[i] >= '0' && digits[i] <= '9';
               if (numeric)
               {
                  since = 0;
                  for (int i = 0; i < digits.GetLength(); i++)
                     since = since * 10 + (digits[i] - '0');
               }
            }

            if (since < 0 || since > current)
            {
               return DavError(403, "<D:valid-sync-token/>",
                  "The sync token is not one this calendar issued; ask again without a token for the whole calendar.");
            }

            std::vector<CalendarObjectRecord> changed;
            if (!CalendarStore::ListChangedSince(context.storeAccountId, context.calendar.id, since, changed))
               return Text(500, "the calendar could not be read");

            for (size_t i = 0; i < changed.size(); i++)
            {
               if (xml.GetLength() > MaxMultistatusBytes)
                  return Text(507, "the response would be too large");
               if (changed[i].deleted)
               {
                  xml += NotFoundResponse(ObjectHref(context, changed[i]));
                  continue;
               }
               Member member;
               FillMember(context, changed[i], member);
               xml += ResponseForMember(context, member, request);
            }
         }

         xml += " <D:sync-token>" + XmlEscape(SyncTokenFor(current)) + "</D:sync-token>\r\n";
         xml += "</D:multistatus>\r\n";
         return Xml(207, xml);
      }

      // free-busy-query (RFC 4791 section 7.10): the answer is not a
      // multistatus but one VFREEBUSY as text/calendar - the busy periods of
      // this one calendar as they concern its owner, over the time-range asked
      // for, which needs both bounds and spans at most 62 days. The reader is
      // whoever may read the calendar; the periods carry no title. The same
      // computation answers GET /api/v1/me/freebusy, so the two agree.
      HttpResponse HandleFreeBusyQuery(Context &context, const XmlElement &report)
      {
         const XmlElement *range = report.Child(NsCalDav, "time-range");
         if (!range)
            return DavError(400, "<C:valid-filter/>", "A free-busy-query carries one time-range with a start and an end.");

         __int64 start = 0;
         __int64 end = ICalendar::Forever;
         AnsiString problem;
         if (!ReadTimeRange(*range, start, end, problem))
            return DavError(400, "<C:valid-filter/>", problem + ".");
         if (Trimmed(range->Attribute("start")).IsEmpty() || Trimmed(range->Attribute("end")).IsEmpty())
            return DavError(400, "<C:valid-filter/>", "The time-range of a free-busy-query needs both a start and an end.");
         if (end - start > static_cast<__int64>(FreeBusy::MaximumWindowDays) * 86400)
            return DavError(400, "<C:valid-filter/>", "The time-range of a free-busy-query is at most 62 days.");

         if (!EnsureCalendar(context))
            return Text(500, "the calendar could not be read");

         std::shared_ptr<const Account> owner = context.storeAccountId == context.account->GetID()
            ? context.account : CacheContainer::Instance()->GetAccount(context.storeAccountId);
         AnsiString address = owner ? Lower(Utf8(owner->GetAddress())) : context.addressUtf8;

         std::vector<FreeBusyPeriod> periods;
         if (!FreeBusy::OfCalendar(context.storeAccountId, context.calendar.id, address, start, end, periods))
            return Text(500, "the calendar could not be read");
         FreeBusy::Merge(periods);

         return Respond(200, "text/calendar; charset=utf-8", FreeBusy::VFreeBusy(address, start, end, periods));
      }

      HttpResponse HandleReport(Context &context, const Resource &resource)
      {
         if (resource.kind == KindObjectSlot || resource.kind == KindCalendarSlot)
            return NotFound();

         XmlElement root;
         AnsiString problem;
         if (!ReadBody(context.request, root, problem))
            return Text(400, "the REPORT body is not well-formed XML: " + problem);

         bool multiget = root.Is(NsCalDav, "calendar-multiget");
         bool query = root.Is(NsCalDav, "calendar-query");
         bool sync = root.Is(NsDav, "sync-collection");
         bool freeBusy = root.Is(NsCalDav, "free-busy-query");

         if (!multiget && !query && !sync && !freeBusy)
            return DavError(403, "<D:supported-report/>", "The reports here are calendar-multiget, calendar-query, sync-collection and free-busy-query.");

         bool onCalendar = resource.kind == KindCalendar;
         bool onObject = resource.kind == KindObject;
         if (!onCalendar && !(multiget && onObject))
            return DavError(403, "<D:supported-report/>", "This report applies to the calendar, " + context.calendarHref + ".");

         if (multiget)
            return HandleMultiget(context, root);
         if (query)
            return HandleQuery(context, root);
         if (freeBusy)
            return HandleFreeBusyQuery(context, root);
         return HandleSyncCollection(context, root);
      }
   }

   bool
   CalDavServer::IsCalendarTarget(const AnsiString &target)
   {
      AnsiString path = target;
      int query = path.Find("?");
      if (query >= 0)
         path = path.Mid(0, query);

      return path == "/dav/calendars" || path.StartsWith(CalendarsPath);
   }

   bool
   CalDavServer::IsLargeRequest(const AnsiString &method, const AnsiString &target)
   {
      if (!IsCalendarTarget(target))
         return false;

      return method == "PUT" || method == "REPORT";
   }

   AnsiString
   CalDavServer::HomeHref(const AnsiString &addressUtf8)
   {
      return AnsiString(CalendarsPath) + PercentEncodeSegment(addressUtf8) + "/";
   }

   HttpResponse
   CalDavServer::Handle(const HttpRequest &request, bool over_https)
   {
      if (!over_https)
      {
         return Text(403,
            "CalDAV is served over HTTPS only: HTTP Basic authentication would send the account's password in clear. "
            "Use the https:// address of this server (WebServicesHttpsPort), or put a TLS-terminating proxy in front "
            "of this listener that sets X-Forwarded-Proto: https.");
      }

      const AnsiString &method = request.method;

      Context context(request);
      bool disconnect = false;
      if (!Authenticate(request, "CalDAV", SignInStore::ProtocolDav, context.account, disconnect))
      {
         HttpResponse refusal = Unauthorized();
         refusal.close = true;
         return refusal;
      }

      context.addressWide = context.account->GetAddress();
      context.addressWide.ToLower();
      context.addressUtf8 = Utf8(context.addressWide);
      AnsiString segment = PercentEncodeSegment(context.addressUtf8);
      context.principalHref = "/dav/principals/" + segment + "/";
      context.ownerPrincipalHref = context.principalHref;
      context.homeHref = HomeHref(context.addressUtf8);
      context.calendarHref = context.homeHref + CalendarStore::DefaultName + "/";
      context.storeAccountId = context.account->GetID();

      Resource resource;
      if (!Resolve(context, request.target, resource))
         return NotFound();

      HttpResponse response;
      try
      {
         if (method == "OPTIONS")
            response = HandleOptions(resource);
         else if (method == "PROPFIND")
            response = HandlePropfind(context, resource);
         else if (method == "PROPPATCH")
            response = HandleProppatch(context, resource);
         else if (method == "REPORT")
            response = HandleReport(context, resource);
         else if (method == "GET")
            response = HandleGet(context, resource, false);
         else if (method == "HEAD")
            response = HandleGet(context, resource, true);
         else if (method == "PUT")
            response = HandlePut(context, resource);
         else if (method == "DELETE")
            response = HandleDelete(context, resource);
         else if (method == "MKCOL" || method == "MKCALENDAR")
            response = HandleMakeCalendar(context, resource);
         else
            response = MethodNotAllowed(resource);
      }
      catch (...)
      {
         response = Text(500, "internal error");
      }

      response.close = response.close || disconnect;

      LOG_DEBUG("CalDAV: " + String(context.addressUtf8) + " " + String(method) + " " + String(request.target) + " -> " +
         StringParser::IntToString(response.status) + ".");

      return response;
   }
}
