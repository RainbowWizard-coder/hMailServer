// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The account's calendars, hm_calendars and hm_calendarobjects (schema 6041):
// one row per calendar collection - one per account for now, "Calendar",
// made on first use - and one row per calendar object under it, holding the
// iCalendar text as the client sent it beside what the queries need without
// reading it: the UID, the component kind, the master's start and end and
// the first and last instant of any instance, all as UTC seconds, the ETag,
// and the sync token the collection was at when the row last changed. A
// deleted object stays as a tombstone (objectdeleted = 1, its text emptied)
// so that a sync-collection report can say it was removed; a later PUT to
// the same name takes the row back. The collection's token is a counter,
// stepped on every write under one mutex, so that a client holding token N
// is answered exactly the rows stamped after N. The one surface that reads
// and writes this today is CalDavServer.cpp.

#pragma once

#include <vector>

namespace HM
{
   struct CalendarRecord
   {
      CalendarRecord() : id(0), accountId(0), syncToken(0), created(0) { }

      __int64 id;
      __int64 accountId;
      String name;          // the URI segment: "calendar"
      String displayName;   // "Calendar"
      __int64 syncToken;    // the collection's counter; also its ctag
      __int64 created;      // UTC seconds
      String color;         // "#RRGGBB" as the page and Apple's calendar-color carry it, or ""
   };

   // A calendar shared with another account of this server.
   struct CalendarShareRecord
   {
      CalendarShareRecord() : id(0), calendarId(0), accountId(0), write(false) { }

      __int64 id;
      __int64 calendarId;
      __int64 accountId;    // the account the calendar is shared with
      bool write;           // may add, change and delete objects; else read-only
      String address;       // that account's address, when the listing joined it
   };

   struct CalendarObjectRecord
   {
      CalendarObjectRecord() : id(0), calendarId(0), syncToken(0), start(0), end(0), first(0), last(0), deleted(false), modified(0) { }

      __int64 id;
      __int64 calendarId;
      String uri;           // the resource name the client chose
      String uid;
      String component;     // "VEVENT" or "VTODO"
      String data;          // the iCalendar text as sent; empty for a tombstone
      String etag;          // quoted, as served
      __int64 syncToken;    // the collection's counter when this row last changed
      __int64 start;        // the master's DTSTART, UTC seconds
      __int64 end;          // the master's end
      __int64 first;        // the earliest instance start
      __int64 last;         // the latest instance end; ICalendar::Forever for a recurrence with no end
      bool deleted;
      __int64 modified;     // UTC seconds
   };

   class CalendarStore
   {
   public:
      static const char *DefaultName;          // "calendar"
      static const char *DefaultDisplayName;   // "Calendar"

      // The account's one calendar, created the first time it is asked for.
      // False when the table could not be read or the row not made.
      static bool EnsureDefault(__int64 accountId, CalendarRecord &calendar);

      // The calendar again, for its current token.
      static bool Get(__int64 accountId, __int64 calendarId, CalendarRecord &calendar);

      // A calendar by id alone, whoever owns it: what a share resolves through.
      static bool GetAny(__int64 calendarId, CalendarRecord &calendar);

      // The account's own calendars, the default first and the rest by name.
      // The default is made if the account has none yet.
      static bool ListCalendars(__int64 accountId, std::vector<CalendarRecord> &calendars);

      // One of the account's own calendars by its URI segment.
      static bool FindByName(__int64 accountId, const String &name, CalendarRecord &calendar);

      // A second, third calendar: a task list, a shared project. False when
      // the name is taken or the row could not be made.
      static bool CreateCalendar(__int64 accountId, const String &name, const String &displayName, const String &color, CalendarRecord &created);

      // The display name and colour changed; the token is stepped so a client
      // re-reads the collection's properties.
      static bool UpdateCalendar(__int64 accountId, __int64 calendarId, const String &displayName, const String &color);

      // The calendar with its objects and shares. The default cannot be
      // deleted, and a calendar that is not the account's own is not found.
      static bool DeleteCalendar(__int64 accountId, __int64 calendarId);

      // The shares of a calendar, each with the address it is shared with.
      static bool ListShares(__int64 calendarId, std::vector<CalendarShareRecord> &shares);

      // The calendars other accounts share with this one, in parallel with the
      // share rows that grant them.
      static bool ListSharedWith(__int64 accountId, std::vector<CalendarRecord> &calendars, std::vector<CalendarShareRecord> &shares);

      // The share of a calendar with an account, if there is one.
      static bool ShareOf(__int64 calendarId, __int64 accountId, CalendarShareRecord &share);

      // Share, or change what a share allows; and take a share back.
      static bool Share(__int64 calendarId, __int64 accountId, bool write);
      static bool Unshare(__int64 calendarId, __int64 accountId);

      // Every share granted to the account: what goes when the account does.
      static bool UnshareAll(__int64 accountId);

      // A URI segment a calendar may be named: printable, no slash, not the
      // reserved "shared-" prefix the sharee's home uses.
      static bool IsUsableCalendarName(const String &name);

      // The live objects of the calendar (tombstones left out), by uri.
      static bool List(__int64 accountId, __int64 calendarId, std::vector<CalendarObjectRecord> &objects);

      // The live objects whose span touches [rangeStart, rangeEnd): last >
      // rangeStart and first < rangeEnd. What a time-range query narrows to
      // before expanding each.
      static bool ListInRange(__int64 accountId, __int64 calendarId, __int64 rangeStart, __int64 rangeEnd, std::vector<CalendarObjectRecord> &objects);

      // Every row, tombstones included, stamped after the token.
      static bool ListChangedSince(__int64 accountId, __int64 calendarId, __int64 sinceToken, std::vector<CalendarObjectRecord> &objects);

      // One live object by its resource name, or by its UID.
      static bool FindByUri(__int64 accountId, __int64 calendarId, const String &uri, CalendarObjectRecord &object);
      static bool FindByUid(__int64 accountId, __int64 calendarId, const String &uid, CalendarObjectRecord &object);

      // One live object by its row id, whichever calendar holds it: the page
      // names events by it, and the caller checks the calendar is theirs.
      static bool FindById(__int64 objectId, CalendarObjectRecord &object);

      // A live object whose name the database's collation takes for this one while
      // it is not exactly this one: a different case on every backend but
      // PostgreSQL, and also different accents or trailing spaces on MySQL. The
      // unique index on the name is compared the same way, so such a name cannot
      // be created beside it; CalDavServer answers 409 and names the object.
      static bool FindCollationTwin(__int64 accountId, __int64 calendarId, const String &uri, CalendarObjectRecord &object);

      // A new object under the client's name, reviving a tombstone of that
      // name if there is one. The collection's token is stepped and answered.
      static bool Insert(__int64 accountId, __int64 calendarId, const CalendarObjectRecord &object, CalendarObjectRecord &inserted, __int64 &token);

      // The object replaced in place: text, UID, kind, span and ETag. The
      // collection's token is stepped and answered.
      static bool Update(__int64 accountId, __int64 calendarId, __int64 objectId, const CalendarObjectRecord &object, __int64 &token);

      // The object turned into a tombstone. False when there was no such live
      // object of this calendar.
      static bool Delete(__int64 accountId, __int64 calendarId, __int64 objectId, __int64 &token);

      static const int MaximumUriLength = 255;
   };
}
