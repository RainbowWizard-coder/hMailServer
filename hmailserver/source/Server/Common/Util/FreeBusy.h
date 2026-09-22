// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Free-busy: when an account is busy, and nothing else about it. One
// computation answers both GET /api/v1/me/freebusy (RestApiCalendar.cpp) and
// the CalDAV free-busy-query REPORT (RFC 4791 section 7.10, CalDavServer.cpp),
// so the page's availability strip and a client asking the server agree. Every
// VEVENT instance of the calendars asked about is expanded through the
// iCalendar module over a bounded window - recurrences included, the
// module's own ceilings holding - and each instance is a busy period unless
// the event is TRANSP:TRANSPARENT, STATUS:CANCELLED, or the account declined
// it; it is tentative when the account's PARTSTAT on it is TENTATIVE or
// NEEDS-ACTION. No title, location or description leaves this unit: a period
// is two instants and a kind.
//
// Not RFC 6638 scheduling: there is no schedule-outbox, no schedule-inbox
// and no free-busy across servers. What is answered is what this server's
// own calendars say, to an account of the same domain that asks.

#pragma once

#include <vector>

namespace HM
{
   struct FreeBusyPeriod
   {
      FreeBusyPeriod() : start(0), end(0), tentative(false) { }
      FreeBusyPeriod(__int64 periodStart, __int64 periodEnd, bool isTentative) : start(periodStart), end(periodEnd), tentative(isTentative) { }

      __int64 start;    // UTC seconds, clipped to the window asked for
      __int64 end;
      bool tentative;   // BUSY-TENTATIVE rather than BUSY
   };

   class FreeBusy
   {
   public:
      // The widest window a query may ask for.
      static const int MaximumWindowDays = 62;

      // The busy periods of one calendar over [from, to) as they concern the
      // address (the calendar owner's, as a rule): each instance clipped to
      // the window, nothing merged yet. False when the calendar could not be
      // read; an object the module cannot expand is left out, not refused.
      static bool OfCalendar(__int64 ownerAccountId, __int64 calendarId, const AnsiString &address, __int64 from, __int64 to, std::vector<FreeBusyPeriod> &periods);

      // Every calendar the account owns - not the ones shared with it - over
      // the window, merged and sorted. An account with no calendar row yet is
      // simply free; none is made by asking.
      static bool OfAccount(__int64 accountId, const AnsiString &address, __int64 from, __int64 to, std::vector<FreeBusyPeriod> &periods);

      // Sorted by start; periods of the same kind that overlap or touch are
      // joined. A busy period and a tentative one may still overlap, as RFC
      // 5545 lets two FREEBUSY lines of different types do.
      static void Merge(std::vector<FreeBusyPeriod> &periods);

      // The VFREEBUSY object (RFC 5545 section 3.6.4) a free-busy-query
      // answers: UID, DTSTAMP, DTSTART and DTEND the window, one FREEBUSY line
      // per period with FBTYPE BUSY or BUSY-TENTATIVE, and nothing that names
      // an event - no ORGANIZER and no ATTENDEE either, which RFC 4791 section
      // 7.10 leaves out and which belong to RFC 6638's scheduling object.
      static AnsiString VFreeBusy(const AnsiString &address, __int64 from, __int64 to, const std::vector<FreeBusyPeriod> &periods);

      // A window bound as a query writes it: "YYYY-MM-DD", "YYYY-MM-DDTHH:MM"
      // or "YYYY-MM-DDTHH:MM:SS" with Z, an offset (+01:00, -0500) or nothing
      // (read as UTC), or the iCalendar forms YYYYMMDD and YYYYMMDDTHHMMSSZ.
      static bool ParseInstant(const AnsiString &text, __int64 &instant);
   };
}
