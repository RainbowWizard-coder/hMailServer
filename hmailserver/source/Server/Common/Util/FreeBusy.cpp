// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See FreeBusy.h.

#include "StdAfx.h"

#include <ctime>
#include <algorithm>

#include "FreeBusy.h"
#include "CalendarStore.h"
#include "ICalendar.h"
#include "Imip.h"
#include "Unicode.h"
#include "GUIDCreator.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const int FreeBusy::MaximumWindowDays;

   namespace
   {
      // The instances one object is expanded to for a window of at most 62
      // days; the module's own ceiling holds above this.
      const size_t InstancesPerObject = 2000;

      AnsiString Upper(const AnsiString &value)
      {
         AnsiString upper = value;
         upper.TrimLeft();
         upper.TrimRight();
         upper.ToUpper();
         return upper;
      }

      AnsiString U8(const String &value)
      {
         AnsiString out;
         Unicode::WideToMultiByte(value, out);
         return out;
      }

      // The address's PARTSTAT on the component, upper-cased; "" when the
      // address is not among its attendees.
      AnsiString PartstatOf(const ICalComponent &component, const AnsiString &address, bool &attendee)
      {
         attendee = false;
         for (size_t i = 0; i < component.properties.size(); i++)
         {
            const ICalProperty &property = component.properties[i];
            if (property.name != "ATTENDEE" || Imip::AddressOf(property.value) != address)
               continue;
            attendee = true;
            AnsiString partstat = Upper(property.Parameter("PARTSTAT"));
            return partstat.IsEmpty() ? AnsiString("NEEDS-ACTION") : partstat;
         }
         return "";
      }

      bool Digits(const AnsiString &text, int from, int count, int &value)
      {
         if (from + count > text.GetLength())
            return false;
         value = 0;
         for (int i = from; i < from + count; i++)
         {
            char c = text.c_str()[i];
            if (c < '0' || c > '9')
               return false;
            value = value * 10 + (c - '0');
         }
         return true;
      }

      bool ByStart(const FreeBusyPeriod &a, const FreeBusyPeriod &b)
      {
         if (a.start != b.start)
            return a.start < b.start;
         if (a.tentative != b.tentative)
            return !a.tentative;
         return a.end < b.end;
      }
   }

   bool
   FreeBusy::OfCalendar(__int64 ownerAccountId, __int64 calendarId, const AnsiString &address, __int64 from, __int64 to, std::vector<FreeBusyPeriod> &periods)
   {
      if (to <= from)
         return true;

      AnsiString wanted = address;
      wanted.ToLower();

      std::vector<CalendarObjectRecord> records;
      if (!CalendarStore::ListInRange(ownerAccountId, calendarId, from - 86400, to + 86400, records))
         return false;

      for (size_t r = 0; r < records.size(); r++)
      {
         if (records[r].component.CompareNoCase(_T("VEVENT")) != 0)
            continue;

         ICalComponent tree;
         AnsiString problem;
         if (!ICalendar::Parse(U8(records[r].data), tree, problem))
            continue;

         std::vector<ICalInstance> instances;
         bool truncated = false;
         if (!ICalendar::Expand(tree, from, to, InstancesPerObject, instances, truncated, problem))
            continue;

         for (size_t i = 0; i < instances.size(); i++)
         {
            const ICalInstance &instance = instances[i];
            const ICalComponent *component = instance.component < tree.children.size() ? &tree.children[instance.component] : nullptr;
            if (!component || component->name != "VEVENT")
               continue;

            if (Upper(component->Value("TRANSP")) == "TRANSPARENT")
               continue;
            if (Upper(component->Value("STATUS")) == "CANCELLED")
               continue;

            bool attendee = false;
            AnsiString partstat = PartstatOf(*component, wanted, attendee);
            if (attendee && partstat == "DECLINED")
               continue;
            bool tentative = attendee && (partstat == "TENTATIVE" || partstat == "NEEDS-ACTION");

            __int64 start = std::max(instance.start, from);
            __int64 end = std::min(instance.end, to);
            if (end <= start)
               continue;
            periods.push_back(FreeBusyPeriod(start, end, tentative));
         }
      }
      return true;
   }

   bool
   FreeBusy::OfAccount(__int64 accountId, const AnsiString &address, __int64 from, __int64 to, std::vector<FreeBusyPeriod> &periods)
   {
      periods.clear();

      std::vector<CalendarRecord> calendars;
      if (!CalendarStore::ListCalendarsIfAny(accountId, calendars))
         return false;

      for (size_t c = 0; c < calendars.size(); c++)
      {
         if (!OfCalendar(accountId, calendars[c].id, address, from, to, periods))
            return false;
      }

      Merge(periods);
      return true;
   }

   void
   FreeBusy::Merge(std::vector<FreeBusyPeriod> &periods)
   {
      std::sort(periods.begin(), periods.end(), ByStart);

      std::vector<FreeBusyPeriod> merged;
      for (size_t i = 0; i < periods.size(); i++)
      {
         bool joined = false;
         // The last period of the same kind is the only one this can touch,
         // the list being sorted by start.
         for (size_t j = merged.size(); j > 0 && !joined; j--)
         {
            FreeBusyPeriod &last = merged[j - 1];
            if (last.tentative != periods[i].tentative)
               continue;
            if (periods[i].start <= last.end)
            {
               last.end = std::max(last.end, periods[i].end);
               joined = true;
            }
            break;
         }
         if (!joined)
            merged.push_back(periods[i]);
      }

      std::sort(merged.begin(), merged.end(), ByStart);
      periods = merged;
   }

   AnsiString
   FreeBusy::VFreeBusy(const AnsiString &address, __int64 from, __int64 to, const std::vector<FreeBusyPeriod> &periods)
   {
      const char *crlf = "\r\n";
      AnsiString text;
      text += "BEGIN:VCALENDAR" + AnsiString(crlf);
      text += "VERSION:2.0" + AnsiString(crlf);
      text += "PRODID:-//hMailServer//caldav//EN" + AnsiString(crlf);
      text += "BEGIN:VFREEBUSY" + AnsiString(crlf);
      AnsiString guid = U8(GUIDCreator::GetGUID());
      guid.ToLower();
      text += ICalendar::Fold("UID:" + guid + "-freebusy") + crlf;
      text += "DTSTAMP:" + ICalendar::FormatUtc(static_cast<__int64>(time(nullptr))) + crlf;
      text += "DTSTART:" + ICalendar::FormatUtc(from) + crlf;
      text += "DTEND:" + ICalendar::FormatUtc(to) + crlf;
      if (!address.IsEmpty())
         text += ICalendar::Fold("ATTENDEE:mailto:" + address) + crlf;
      for (size_t i = 0; i < periods.size(); i++)
      {
         text += "FREEBUSY;FBTYPE=" + AnsiString(periods[i].tentative ? "BUSY-TENTATIVE" : "BUSY") + ":" +
            ICalendar::FormatUtc(periods[i].start) + "/" + ICalendar::FormatUtc(periods[i].end) + crlf;
      }
      text += "END:VFREEBUSY" + AnsiString(crlf);
      text += "END:VCALENDAR" + AnsiString(crlf);
      return text;
   }

   bool
   FreeBusy::ParseInstant(const AnsiString &raw, __int64 &instant)
   {
      AnsiString text = raw;
      text.TrimLeft();
      text.TrimRight();
      if (text.IsEmpty())
         return false;

      // The iCalendar forms first: no dashes.
      if (text.GetLength() >= 8 && text.c_str()[4] != '-')
         return ICalendar::ParseUtc(text, instant);

      int y, m, d, hh = 0, mm = 0, ss = 0;
      if (text.GetLength() < 10 || text.c_str()[4] != '-' || text.c_str()[7] != '-')
         return false;
      if (!Digits(text, 0, 4, y) || !Digits(text, 5, 2, m) || !Digits(text, 8, 2, d))
         return false;
      if (m < 1 || m > 12 || d < 1 || d > 31)
         return false;

      int offsetSeconds = 0;
      if (text.GetLength() > 10)
      {
         char separator = text.c_str()[10];
         if (separator != 'T' && separator != ' ')
            return false;
         if (!Digits(text, 11, 2, hh) || text.GetLength() < 16 || text.c_str()[13] != ':' || !Digits(text, 14, 2, mm))
            return false;
         int at = 16;
         if (text.GetLength() >= 19 && text.c_str()[16] == ':')
         {
            if (!Digits(text, 17, 2, ss))
               return false;
            at = 19;
         }
         if (hh > 23 || mm > 59 || ss > 59)
            return false;
         AnsiString rest = text.Mid(at);
         if (rest == "Z" || rest == "z")
            offsetSeconds = 0;
         else if (!rest.IsEmpty())
         {
            char sign = rest.c_str()[0];
            if (sign != '+' && sign != '-')
               return false;
            int oh = 0, om = 0;
            if (!Digits(rest, 1, 2, oh))
               return false;
            if (rest.GetLength() == 6 && rest.c_str()[3] == ':')
            {
               if (!Digits(rest, 4, 2, om))
                  return false;
            }
            else if (rest.GetLength() == 5)
            {
               if (!Digits(rest, 3, 2, om))
                  return false;
            }
            else if (rest.GetLength() != 3)
               return false;
            if (oh > 14 || om > 59)
               return false;
            offsetSeconds = (oh * 3600 + om * 60) * (sign == '-' ? -1 : 1);
         }
      }

      __int64 utc = 0;
      if (!ICalendar::LocalToInstant("UTC", y, m, d, hh, mm, ss, utc))
         return false;
      instant = utc - offsetSeconds;
      return true;
   }
}
