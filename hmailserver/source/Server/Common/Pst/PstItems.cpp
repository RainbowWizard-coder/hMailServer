// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See PstItems.h.

#include "StdAfx.h"

#include "PstItems.h"
#include "PstCodePages.h"
#include "PstMime.h"
#include "PstRtf.h"
#include "../Util/ICalendar.h"
#include "../Util/VCard.h"

#include <cstdio>
#include <cstring>
#include <set>

namespace HM
{
   const size_t PstItems::MaximumRecurrenceBytes;
   const size_t PstItems::MaximumPhotoBytes;
   const size_t PstItems::MaximumItemTextBytes;
   const size_t PstItems::MaximumRtfBytes;

   namespace
   {
      // Minutes from 1601-01-01 to 1970-01-01: the recurrence blob counts in
      // minutes since 1601, in the series' own zone.
      const int64_t MinutesTo1970 = 11644473600LL / 60;

      const char *const DayNames[] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };

      int64_t SecondsOfMinutes(uint64_t minutes)
      {
         return ((int64_t) minutes - MinutesTo1970) * 60;
      }

      // Days since 1970 of a civil date (Howard Hinnant's algorithm, the
      // inverse of the one PstFormat::UnixToCivil uses).
      int64_t DaysFromCivil(int year, int month, int day)
      {
         year -= month <= 2 ? 1 : 0;
         const int64_t era = (year >= 0 ? year : year - 399) / 400;
         const int64_t yearOfEra = year - era * 400;
         const int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
         const int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
         return era * 146097 + dayOfEra - 719468;
      }

      int DaysInMonth(int year, int month)
      {
         static const int lengths[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
         if (month < 1 || month > 12)
            return 31;
         if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
            return 29;
         return lengths[month - 1];
      }

      void Civil(int64_t seconds, int &year, int &month, int &day, int &hour, int &minute, int &second)
      {
         int weekday = 0;
         PstFormat::UnixToCivil(seconds, year, month, day, hour, minute, second, weekday);
      }

      // "YYYYMMDDTHHMMSS", a Z after it for UTC.
      std::string DateTimeText(int64_t seconds, bool utc)
      {
         int year, month, day, hour, minute, second;
         Civil(seconds, year, month, day, hour, minute, second);
         char text[32];
         snprintf(text, sizeof(text), "%04d%02d%02dT%02d%02d%02d%s", year, month, day, hour, minute, second, utc ? "Z" : "");
         return text;
      }

      std::string DateText(int64_t seconds)
      {
         int year, month, day, hour, minute, second;
         Civil(seconds, year, month, day, hour, minute, second);
         char text[16];
         snprintf(text, sizeof(text), "%04d%02d%02d", year, month, day);
         return text;
      }

      std::string ReadableText(int64_t seconds)
      {
         int year, month, day, hour, minute, second;
         Civil(seconds, year, month, day, hour, minute, second);
         char text[32];
         snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d UTC", year, month, day, hour, minute);
         return text;
      }

      // A FILETIME as seconds since 1970, false for none - 0, out of range, or
      // Outlook's 4501-01-01 for "no date".
      bool Seconds(uint64_t fileTime, int64_t &seconds)
      {
         return PstFormat::FileTimeToUnix(fileTime, seconds) && !PstItems::IsNoDate(fileTime);
      }

      // "+0100", "-0530": minutes east of UTC as TZOFFSETFROM and TZOFFSETTO
      // write them.
      std::string OffsetText(int minutes)
      {
         const char sign = minutes < 0 ? '-' : '+';
         const int absolute = minutes < 0 ? -minutes : minutes;
         char text[16];
         snprintf(text, sizeof(text), "%c%02d%02d", sign, absolute / 60, absolute % 60);
         return text;
      }

      std::string Number(int64_t value)
      {
         char text[32];
         snprintf(text, sizeof(text), "%lld", (long long) value);
         return text;
      }

      std::string Hex16(uint32_t value)
      {
         char text[16];
         snprintf(text, sizeof(text), "0x%x", (unsigned int) value);
         return text;
      }

      std::string Trim(const std::string &text)
      {
         const size_t first = text.find_first_not_of(" \t\r\n");
         if (first == std::string::npos)
            return std::string();
         const size_t last = text.find_last_not_of(" \t\r\n");
         return text.substr(first, last - first + 1);
      }

      // A name that goes into a parameter or a TZID: printable, no quote, no
      // character a parameter or a list would take for its own, at most 100
      // bytes, cut at a character (each byte replaced is ASCII, and replaced by
      // one, so the cut stays where it was made).
      std::string SafeName(const std::string &utf8)
      {
         const std::string cut = PstFormat::CutUtf8(utf8, 100);
         std::string out;
         for (size_t i = 0; i < cut.size(); i++)
         {
            const unsigned char c = (unsigned char) cut[i];
            if (c < 0x20 || c == 0x7F || c == '"' || c == ';' || c == ',' || c == ':' || c == '\\')
               out += ' ';
            else
               out += (char) c;
         }
         return Trim(out);
      }

      // Eight-bit text in a code page as UTF-8: the table's conversion, else
      // its ASCII.
      std::string EightBit(const std::string &bytes, int codePage)
      {
         std::string utf8;
         if (!PstCodePages::Decode(bytes, codePage, utf8))
         {
            utf8.clear();
            for (size_t i = 0; i < bytes.size(); i++)
            {
               const unsigned char c = (unsigned char) bytes[i];
               if (c < 0x80)
                  utf8 += (char) c;
               else
                  PstFormat::AppendUtf8(utf8, 0xFFFD);
            }
         }
         const size_t end = utf8.find('\0');
         return end == std::string::npos ? utf8 : utf8.substr(0, end);
      }

      // A recurrence blob, read field by field; a read past its end throws
      // PstError, which ParseRecurrence turns into its reason.
      class Cursor
      {
      public:
         explicit Cursor(const std::string &data) : data_(data), at_(0) { }

         uint16_t U16()
         {
            const uint16_t value = PstFormat::U16(data_, at_, "a recurrence pattern");
            at_ += 2;
            return value;
         }

         uint32_t U32()
         {
            const uint32_t value = PstFormat::U32(data_, at_, "a recurrence pattern");
            at_ += 4;
            return value;
         }

         std::string Take(uint64_t length)
         {
            if (!PstFormat::Fits(at_, length, data_.size()))
               throw PstError("A recurrence pattern runs past its end.");
            const std::string value = data_.substr(at_, (size_t) length);
            at_ += (size_t) length;
            return value;
         }

         size_t Left() const { return data_.size() - at_; }

      private:
         const std::string &data_;
         size_t at_;
      };

      PstSystemTime SystemTimeAt(const std::string &blob, size_t at)
      {
         PstSystemTime time;
         time.year = PstFormat::U16(blob, at, "a time zone");
         time.month = PstFormat::U16(blob, at + 2, "a time zone");
         time.dayOfWeek = PstFormat::U16(blob, at + 4, "a time zone");
         time.day = PstFormat::U16(blob, at + 6, "a time zone");
         time.hour = PstFormat::U16(blob, at + 8, "a time zone");
         time.minute = PstFormat::U16(blob, at + 10, "a time zone");
         time.second = PstFormat::U16(blob, at + 12, "a time zone");
         return time;
      }

      bool ValidTransition(const PstSystemTime &time)
      {
         if (time.month == 0)
            return true;
         if (time.month > 12 || time.dayOfWeek > 6 || time.hour > 23 || time.minute > 59 || time.second > 59)
            return false;
         if (time.year == 0)
            return time.day >= 1 && time.day <= 5;
         return time.year >= 1601 && time.year <= 9999 && time.day >= 1 && time.day <= DaysInMonth(time.year, time.month);
      }

      bool ValidZone(PstTimeZone &zone)
      {
         if (zone.bias < -1440 || zone.bias > 1440 || zone.standardBias < -1440 || zone.standardBias > 1440 ||
             zone.daylightBias < -1440 || zone.daylightBias > 1440)
            return false;
         if (zone.StandardOffset() < -18 * 60 || zone.StandardOffset() > 18 * 60 || zone.DaylightOffset() < -18 * 60 || zone.DaylightOffset() > 18 * 60)
            return false;
         if (!ValidTransition(zone.standardDate) || !ValidTransition(zone.daylightDate))
            return false;

         // One transition without the other is no daylight saving at all.
         if (zone.standardDate.month == 0 || zone.daylightDate.month == 0)
         {
            zone.standardDate.month = 0;
            zone.daylightDate.month = 0;
         }
         return true;
      }

      // When a transition falls in a year, as wall-clock seconds in the time
      // it leaves.
      int64_t TransitionIn(const PstSystemTime &time, int year)
      {
         int day = time.day;
         if (time.year == 0)
            day = PstItems::NthWeekday(year, time.month, time.dayOfWeek, time.day);
         else if (day > DaysInMonth(year, time.month))
            day = DaysInMonth(year, time.month);
         return DaysFromCivil(year, time.month, day) * 86400 + time.hour * 3600 + time.minute * 60 + time.second;
      }

      bool DaylightAtLocal(const PstTimeZone &zone, int64_t local)
      {
         if (!zone.HasDaylight())
            return false;

         int year, month, day, hour, minute, second;
         Civil(local, year, month, day, hour, minute, second);
         const int64_t daylightStart = TransitionIn(zone.daylightDate, year);
         const int64_t standardStart = TransitionIn(zone.standardDate, year);
         if (daylightStart < standardStart)
            return local >= daylightStart && local < standardStart;
         return local >= daylightStart || local < standardStart;
      }

      // ------------------------------------------------------------ iCalendar

      ICalProperty IcsProperty(const char *name, const std::string &value)
      {
         ICalProperty property;
         property.name = name;
         property.value = AnsiString(value);
         return property;
      }

      ICalProperty TextProperty(const char *name, const std::string &utf8)
      {
         return IcsProperty(name, std::string(ICalendar::Escape(AnsiString(utf8))));
      }

      void Parameter(ICalProperty &property, const char *name, const std::string &value)
      {
         property.parameters.push_back(std::make_pair(AnsiString(name), AnsiString(value)));
      }

      std::string EscapedList(const std::vector<std::string> &values)
      {
         std::string out;
         for (const std::string &value : values)
         {
            const std::string item = Trim(value);
            if (item.empty())
               continue;
            if (!out.empty())
               out += ",";
            out += std::string(ICalendar::Escape(AnsiString(item)));
         }
         return out;
      }

      // A local time in the series' zone: DTSTART, DTEND, EXDATE,
      // RECURRENCE-ID - a DATE for an all-day series.
      ICalProperty LocalProperty(const char *name, int64_t localSeconds, bool allDay, const std::string &tzid)
      {
         if (allDay)
         {
            ICalProperty property = IcsProperty(name, DateText(localSeconds));
            Parameter(property, "VALUE", "DATE");
            return property;
         }

         ICalProperty property = IcsProperty(name, DateTimeText(localSeconds, false));
         Parameter(property, "TZID", tzid);
         return property;
      }

      ICalComponent TimeZoneComponent(const PstTimeZone &zone, const std::string &tzid)
      {
         ICalComponent component;
         component.name = "VTIMEZONE";
         component.properties.push_back(TextProperty("TZID", tzid));

         auto observance = [&](const char *name, const PstSystemTime &time, int from, int to)
         {
            ICalComponent child;
            child.name = name;

            if (time.month == 0)
               child.properties.push_back(IcsProperty("DTSTART", "16010101T000000"));
            else
               child.properties.push_back(IcsProperty("DTSTART", DateTimeText(TransitionIn(time, 1601), false)));

            child.properties.push_back(IcsProperty("TZOFFSETFROM", OffsetText(from)));
            child.properties.push_back(IcsProperty("TZOFFSETTO", OffsetText(to)));

            if (time.month != 0 && time.year == 0)
            {
               const int ordinal = time.day >= 5 ? -1 : time.day;
               child.properties.push_back(IcsProperty("RRULE", "FREQ=YEARLY;BYMONTH=" + Number(time.month) + ";BYDAY=" + Number(ordinal) +
                                                             DayNames[time.dayOfWeek % 7]));
            }
            else if (time.month != 0)
            {
               // A transition given as a date is read as that date every
               // year, as TransitionIn reads it for the offsets: a day past a
               // month's end is its last day (the 29th of February, in a year
               // without one, the 28th). Only a day past the 28th can be past
               // an end: the 28th of February is the 28th in a leap year too.
               const bool lastDay = time.day > 28 && time.day == DaysInMonth(time.year, time.month);
               child.properties.push_back(IcsProperty("RRULE", "FREQ=YEARLY;BYMONTH=" + Number(time.month) + ";BYMONTHDAY=" +
                                                             (lastDay ? std::string("-1") : Number(time.day))));
            }
            component.children.push_back(child);
         };

         if (zone.HasDaylight())
         {
            observance("STANDARD", zone.standardDate, zone.DaylightOffset(), zone.StandardOffset());
            observance("DAYLIGHT", zone.daylightDate, zone.StandardOffset(), zone.DaylightOffset());
         }
         else
         {
            PstSystemTime none;
            observance("STANDARD", none, zone.StandardOffset(), zone.StandardOffset());
         }

         return component;
      }

      // The RRULE of a pattern ParseRecurrence has checked.
      std::string RuleOf(const PstRecurrence &recurrence, const PstTimeZone &zone, bool allDay)
      {
         std::string rule;
         std::string days;
         for (int bit = 0; bit < 7; bit++)
         {
            if (recurrence.dayMask & (1u << bit))
               days += (days.empty() ? "" : ",") + std::string(DayNames[bit]);
         }

         if (recurrence.patternType == 0)
            rule = "FREQ=DAILY;INTERVAL=" + Number(recurrence.period / 1440);
         else if (recurrence.patternType == 1)
            rule = "FREQ=WEEKLY;INTERVAL=" + Number(recurrence.period) + ";BYDAY=" + days + ";WKST=" + DayNames[recurrence.firstDayOfWeek % 7];
         else
         {
            // Monthly patterns, and yearly ones - a monthly pattern of twelve
            // months, in the month the series starts.
            const bool yearly = recurrence.frequency == 0x200D && recurrence.period % 12 == 0;
            rule = yearly ? "FREQ=YEARLY;INTERVAL=" + Number(recurrence.period / 12) : "FREQ=MONTHLY;INTERVAL=" + Number(recurrence.period);
            if (yearly)
            {
               int year, month, day, hour, minute, second;
               Civil(SecondsOfMinutes(recurrence.startDate), year, month, day, hour, minute, second);
               rule += ";BYMONTH=" + Number(month);
            }

            if (recurrence.patternType == 2)
            {
               // Outlook's "day 30 of every month" falls on the last day of a
               // shorter month: the last of the days up to it that the month has.
               const uint32_t day = recurrence.dayOfMonth;
               if (day <= 28)
                  rule += ";BYMONTHDAY=" + Number(day);
               else if (day >= 31)
                  rule += ";BYMONTHDAY=-1";
               else
               {
                  std::string list;
                  for (uint32_t d = 28; d <= day; d++)
                     list += (list.empty() ? "" : ",") + Number(d);
                  rule += ";BYMONTHDAY=" + list + ";BYSETPOS=-1";
               }
            }
            else if (recurrence.patternType == 3)
               rule += ";BYDAY=" + days + ";BYSETPOS=" + Number(recurrence.nth >= 5 ? -1 : (int64_t) recurrence.nth);
            else
               rule += ";BYMONTHDAY=-1";
         }

         if (recurrence.endType == 0x2022)
            rule += ";COUNT=" + Number(recurrence.occurrenceCount);
         else if (recurrence.endType == 0x2021)
         {
            // The last occurrence's start, in UTC as RFC 5545 asks of an
            // UNTIL beside a DTSTART with a zone; a DATE for an all-day series.
            if (allDay)
               rule += ";UNTIL=" + DateText(SecondsOfMinutes(recurrence.endDate));
            else
               rule += ";UNTIL=" + DateTimeText(PstItems::LocalToUtc(zone, SecondsOfMinutes(recurrence.endDate) + (int64_t) recurrence.startTimeOffset * 60), true);
         }

         return rule;
      }

      // The properties the series and its changed occurrences share.
      void CommonProperties(const PstAppointment &appointment, ICalComponent &event, const std::string &uid, const std::string &stamp,
                            const std::string &summary, const std::string &location, int busyStatus)
      {
         event.properties.push_back(TextProperty("UID", uid));
         event.properties.push_back(IcsProperty("DTSTAMP", stamp));
         if (!summary.empty())
            event.properties.push_back(TextProperty("SUMMARY", summary));
         if (!location.empty())
            event.properties.push_back(TextProperty("LOCATION", location));
         if (!Trim(appointment.body).empty())
            event.properties.push_back(TextProperty("DESCRIPTION", appointment.body));

         const std::string categories = EscapedList(appointment.categories);
         if (!categories.empty())
            event.properties.push_back(IcsProperty("CATEGORIES", categories));

         if (appointment.sensitivity == 2 || appointment.sensitivity == 3)
            event.properties.push_back(IcsProperty("CLASS", appointment.sensitivity == 2 ? "PRIVATE" : "CONFIDENTIAL"));

         if (busyStatus >= 0)
         {
            static const char *const words[] = { "FREE", "TENTATIVE", "BUSY", "OOF", "WORKINGELSEWHERE" };
            event.properties.push_back(IcsProperty("TRANSP", busyStatus == 0 ? "TRANSPARENT" : "OPAQUE"));
            if (busyStatus <= 4)
               event.properties.push_back(IcsProperty("X-MICROSOFT-CDO-BUSYSTATUS", words[busyStatus]));
         }

         if (appointment.canceled)
            event.properties.push_back(IcsProperty("STATUS", "CANCELLED"));

         if (!appointment.organizerAddress.empty())
         {
            ICalProperty organizer = IcsProperty("ORGANIZER", "mailto:" + appointment.organizerAddress);
            const std::string name = SafeName(appointment.organizerName);
            if (!name.empty())
               Parameter(organizer, "CN", name);
            event.properties.push_back(organizer);
         }

         for (const PstAttendee &attendee : appointment.attendees)
         {
            if ((attendee.flags & 0x0002) != 0 || attendee.address.empty())
               continue;

            ICalProperty property = IcsProperty("ATTENDEE", "mailto:" + attendee.address);
            const std::string name = SafeName(attendee.name);
            if (!name.empty())
               Parameter(property, "CN", name);
            if (attendee.type == 3)
            {
               Parameter(property, "CUTYPE", "RESOURCE");
               Parameter(property, "ROLE", "NON-PARTICIPANT");
            }
            else
               Parameter(property, "ROLE", attendee.type == 2 ? "OPT-PARTICIPANT" : "REQ-PARTICIPANT");

            const char *partstat = "NEEDS-ACTION";
            if (attendee.trackStatus == 2)
               partstat = "TENTATIVE";
            else if (attendee.trackStatus == 3)
               partstat = "ACCEPTED";
            else if (attendee.trackStatus == 4)
               partstat = "DECLINED";
            Parameter(property, "PARTSTAT", partstat);
            Parameter(property, "RSVP", "FALSE");
            event.properties.push_back(property);
         }

         if (appointment.reminderSet)
         {
            ICalComponent alarm;
            alarm.name = "VALARM";
            alarm.properties.push_back(IcsProperty("ACTION", "DISPLAY"));
            alarm.properties.push_back(TextProperty("DESCRIPTION", summary.empty() ? std::string("Reminder") : summary));
            const int minutes = appointment.reminderMinutes < 0 || appointment.reminderMinutes > 60 * 24 * 60 ? 15 : appointment.reminderMinutes;
            alarm.properties.push_back(IcsProperty("TRIGGER", "-PT" + Number(minutes) + "M"));
            event.children.push_back(alarm);
         }
      }

      std::string StampOf(uint64_t modified, uint64_t created, uint64_t start)
      {
         const uint64_t times[] = { modified, created, start };
         for (uint64_t fileTime : times)
         {
            int64_t seconds = 0;
            if (Seconds(fileTime, seconds))
               return DateTimeText(seconds, true);
         }
         return "20000101T000000Z";
      }

      ICalComponent Calendar()
      {
         ICalComponent calendar;
         calendar.name = "VCALENDAR";
         calendar.properties.push_back(IcsProperty("VERSION", "2.0"));
         calendar.properties.push_back(IcsProperty("PRODID", "-//hMailServer//PST import//EN"));
         return calendar;
      }

      std::string Text(const ICalComponent &calendar)
      {
         const AnsiString text = ICalendar::Serialize(calendar);
         return std::string(text.c_str(), text.size());
      }
   }

   // ---------------------------------------------------------------- the blob

   bool
   PstItems::ParseRecurrence(const std::string &blob, int codePage, PstRecurrence &recurrence, std::string &problem, PstBudget *budget)
   {
      recurrence = PstRecurrence();

      if (blob.size() > MaximumRecurrenceBytes)
      {
         problem = "its recurrence pattern is larger than any Outlook writes";
         return false;
      }

      if (budget)
      {
         budget->Item();
         budget->Bytes(blob.size());
      }

      try
      {
         Cursor cursor(blob);

         // [MS-OXOCAL] 2.2.1.44.1 RecurrencePattern.
         const uint16_t readerVersion = cursor.U16();
         cursor.U16();   // WriterVersion
         if (readerVersion != 0x3004)
         {
            problem = "its recurrence pattern's version (" + Hex16(readerVersion) + ") is not the one [MS-OXOCAL] describes";
            return false;
         }

         recurrence.frequency = cursor.U16();
         recurrence.patternType = cursor.U16();
         recurrence.calendarType = cursor.U16();
         cursor.U32();   // FirstDateTime: the start date and the pattern say the same
         recurrence.period = cursor.U32();
         cursor.U32();   // SlidingFlag: tasks only

         if (recurrence.frequency < 0x200A || recurrence.frequency > 0x200D)
         {
            problem = "its recurrence frequency (" + Hex16((uint32_t) recurrence.frequency) + ") is not one [MS-OXOCAL] names";
            return false;
         }

         switch (recurrence.patternType)
         {
         case 0x0000:
            break;
         case 0x0001:
            recurrence.dayMask = cursor.U32();
            break;
         case 0x0002:
         case 0x0004:
            recurrence.dayOfMonth = cursor.U32();
            break;
         case 0x0003:
            recurrence.dayMask = cursor.U32();
            recurrence.nth = cursor.U32();
            break;
         case 0x000A:
         case 0x000B:
         case 0x000C:
            problem = "it recurs by the Hijri calendar, which an iCalendar rule cannot say";
            return false;
         default:
            problem = "its recurrence pattern type (" + Hex16((uint32_t) recurrence.patternType) + ") is not one [MS-OXOCAL] names";
            return false;
         }

         // The Gregorian calendars, and those counted in Gregorian days under
         // other eras (Japan, Taiwan, Korea, Thai); a lunar or Hijri one is not
         // a rule iCalendar can say.
         static const int gregorian[] = { 0, 1, 2, 3, 4, 5, 7, 9, 10, 11, 12 };
         bool known = false;
         for (int calendar : gregorian)
            known = known || recurrence.calendarType == calendar;
         if (!known)
         {
            problem = "it recurs by a calendar (" + Number(recurrence.calendarType) + ") an iCalendar rule cannot say";
            return false;
         }

         recurrence.endType = cursor.U32();
         recurrence.occurrenceCount = cursor.U32();
         recurrence.firstDayOfWeek = cursor.U32();

         const uint32_t deletedCount = cursor.U32();
         if (deletedCount > cursor.Left() / 4)
         {
            problem = "its recurrence pattern lists more removed occurrences than it holds";
            return false;
         }
         for (uint32_t i = 0; i < deletedCount; i++)
            recurrence.deleted.push_back(cursor.U32());

         const uint32_t modifiedCount = cursor.U32();
         if (modifiedCount > cursor.Left() / 4)
         {
            problem = "its recurrence pattern lists more changed occurrences than it holds";
            return false;
         }
         for (uint32_t i = 0; i < modifiedCount; i++)
            recurrence.modified.push_back(cursor.U32());

         recurrence.startDate = cursor.U32();
         recurrence.endDate = cursor.U32();

         // [MS-OXOCAL] 2.2.1.44.5 AppointmentRecurrencePattern.
         const uint32_t readerVersion2 = cursor.U32();
         const uint32_t writerVersion2 = cursor.U32();
         if (readerVersion2 != 0x3006)
         {
            problem = "its appointment recurrence pattern's version (" + Hex16(readerVersion2) + ") is not the one [MS-OXOCAL] describes";
            return false;
         }

         recurrence.startTimeOffset = cursor.U32();
         recurrence.endTimeOffset = cursor.U32();

         const uint16_t exceptionCount = cursor.U16();
         if (exceptionCount > cursor.Left() / 14 || exceptionCount > deletedCount)
         {
            problem = "its recurrence pattern lists more changed occurrences than it holds";
            return false;
         }

         // [MS-OXOCAL] 2.2.1.44.5: ExceptionCount MUST be ModifiedInstanceCount.
         // One fewer would leave a moved occurrence where it was and where it
         // went, or nowhere.
         if ((uint32_t) exceptionCount != modifiedCount)
         {
            problem = "its recurrence pattern changes " + Number((int64_t) modifiedCount) + " occurrences and describes " +
                      Number((int64_t) exceptionCount) + ", which [MS-OXOCAL] requires to be the same";
            return false;
         }

         for (uint16_t i = 0; i < exceptionCount; i++)
         {
            if (budget)
               budget->Item();

            // ExceptionInfo: the times, OverrideFlags, and a field for each
            // flag, in [MS-OXOCAL]'s order.
            PstRecurrenceException exception;
            exception.start = cursor.U32();
            exception.end = cursor.U32();
            exception.originalStart = cursor.U32();
            exception.overrideFlags = cursor.U16();

            if (exception.overrideFlags & 0x0001)
            {
               const uint16_t length = cursor.U16();
               const uint16_t length2 = cursor.U16();
               // SubjectLength is SubjectLength2 and one: the second can never be
               // the longer.
               if (length2 > length)
               {
                  problem = "a changed occurrence's subject says two lengths that do not agree";
                  return false;
               }
               exception.hasSubject = true;
               exception.subject = EightBit(cursor.Take(length2), codePage);
            }
            if (exception.overrideFlags & 0x0002)
               cursor.U32();   // MeetingType
            if (exception.overrideFlags & 0x0004)
               cursor.U32();   // ReminderDelta
            if (exception.overrideFlags & 0x0008)
               cursor.U32();   // ReminderSet
            if (exception.overrideFlags & 0x0010)
            {
               const uint16_t length = cursor.U16();
               const uint16_t length2 = cursor.U16();
               if (length2 > length)
               {
                  problem = "a changed occurrence's location says two lengths that do not agree";
                  return false;
               }
               exception.hasLocation = true;
               exception.location = EightBit(cursor.Take(length2), codePage);
            }
            if (exception.overrideFlags & 0x0020)
            {
               exception.hasBusyStatus = true;
               exception.busyStatus = (int) cursor.U32();
            }
            if (exception.overrideFlags & 0x0040)
               cursor.U32();   // Attachment
            if (exception.overrideFlags & 0x0080)
               cursor.U32();   // SubType
            if (exception.overrideFlags & 0x0100)
               cursor.U32();   // AppointmentColor

            recurrence.exceptions.push_back(exception);
         }

         cursor.Take(cursor.U32());   // ReservedBlock1

         // ExtendedException ([MS-OXOCAL] 2.2.1.44.3): the Unicode subject and
         // location, taken only where its times are the ExceptionInfo's own -
         // [MS-OXOCAL] says they MUST be - so a shorter blob of an older
         // writer is never misread; the eight-bit ones stand otherwise.
         try
         {
            for (PstRecurrenceException &exception : recurrence.exceptions)
            {
               if (writerVersion2 >= 0x3009)
               {
                  const uint32_t size = cursor.U32();
                  if (size < 4)
                     break;
                  cursor.Take(size);   // ChangeHighlightValue and its reserved bytes
               }
               cursor.Take(cursor.U32());   // ReservedBlockEE1

               if ((exception.overrideFlags & 0x0011) == 0)
                  continue;

               const uint32_t start = cursor.U32();
               const uint32_t end = cursor.U32();
               const uint32_t original = cursor.U32();
               std::string subject, location;
               bool hasSubject = false, hasLocation = false;
               if (exception.overrideFlags & 0x0001)
               {
                  subject = PstFormat::Utf16ToUtf8(cursor.Take((uint64_t) cursor.U16() * 2));
                  hasSubject = true;
               }
               if (exception.overrideFlags & 0x0010)
               {
                  location = PstFormat::Utf16ToUtf8(cursor.Take((uint64_t) cursor.U16() * 2));
                  hasLocation = true;
               }
               cursor.Take(cursor.U32());   // ReservedBlockEE2

               if (start != exception.start || end != exception.end || original != exception.originalStart)
                  break;
               if (hasSubject)
                  exception.subject = subject;
               if (hasLocation)
                  exception.location = location;
            }
         }
         catch (PstError &)
         {
            // No extended part, or not one that holds: the eight-bit texts.
         }
      }
      catch (PstBudgetExhausted &)
      {
         // The budget's, not the blob's: the caller's to count.
         throw;
      }
      catch (PstError &error)
      {
         problem = std::string("its recurrence pattern is cut short: ") + error.what();
         return false;
      }

      // What iCalendar can say, and what [MS-OXOCAL] allows.
      if (recurrence.patternType == 0 && (recurrence.period == 0 || recurrence.period % 1440 != 0 || recurrence.period / 1440 > 999))
      {
         problem = "its daily recurrence has a period (" + Number(recurrence.period) + " minutes) that is not a whole number of days";
         return false;
      }
      if (recurrence.patternType != 0 && (recurrence.period == 0 || recurrence.period > 99 * 12))
      {
         problem = "its recurrence has a period (" + Number(recurrence.period) + ") [MS-OXOCAL] does not allow";
         return false;
      }
      if ((recurrence.patternType == 1 || recurrence.patternType == 3) && ((recurrence.dayMask & 0x7F) == 0 || recurrence.dayMask > 0x7F))
      {
         problem = "its recurrence names no day of the week";
         return false;
      }
      if (recurrence.patternType == 3 && (recurrence.nth < 1 || recurrence.nth > 5))
      {
         problem = "its recurrence names a week of the month (" + Number(recurrence.nth) + ") that is not the first to the last";
         return false;
      }
      if (recurrence.patternType == 2 && (recurrence.dayOfMonth < 1 || recurrence.dayOfMonth > 31))
      {
         problem = "its recurrence names a day of the month (" + Number(recurrence.dayOfMonth) + ") that no month has";
         return false;
      }
      if (recurrence.endType != 0x2021 && recurrence.endType != 0x2022 && recurrence.endType != 0x2023 && recurrence.endType != 0xFFFFFFFF)
      {
         problem = "its recurrence ends in a way (" + Hex16(recurrence.endType) + ") [MS-OXOCAL] does not name";
         return false;
      }
      if (recurrence.endType == 0x2022 && (recurrence.occurrenceCount == 0 || recurrence.occurrenceCount > 999999))
      {
         problem = "its recurrence ends after a count (" + Number(recurrence.occurrenceCount) + ") that is not one";
         return false;
      }
      if (recurrence.endType == 0x2021 && recurrence.endDate < recurrence.startDate)
      {
         problem = "its recurrence ends before it starts";
         return false;
      }
      if (recurrence.firstDayOfWeek > 6 || recurrence.startTimeOffset >= 1440 || recurrence.endTimeOffset < recurrence.startTimeOffset ||
          recurrence.endTimeOffset - recurrence.startTimeOffset > 1440 * 366)
      {
         problem = "its recurrence's times of day or first day of the week are not ones [MS-OXOCAL] allows";
         return false;
      }

      // Dates at midnight, as [MS-OXOCAL] writes them.
      recurrence.startDate -= recurrence.startDate % 1440;
      for (uint32_t &date : recurrence.deleted)
         date -= date % 1440;
      return true;
   }

   // ---------------------------------------------------------------- time zones

   bool
   PstItems::ParseTimeZoneStruct(const std::string &blob, PstTimeZone &zone, PstBudget *budget)
   {
      zone = PstTimeZone();
      if (budget)
      {
         budget->Item();
         budget->Bytes(blob.size());
      }

      if (blob.size() < 48)
         return false;

      try
      {
         zone.bias = (int32_t) PstFormat::U32(blob, 0, "a time zone");
         zone.standardBias = (int32_t) PstFormat::U32(blob, 4, "a time zone");
         zone.daylightBias = (int32_t) PstFormat::U32(blob, 8, "a time zone");
         zone.standardDate = SystemTimeAt(blob, 14);
         zone.daylightDate = SystemTimeAt(blob, 32);
      }
      catch (PstError &)
      {
         return false;
      }

      zone.valid = ValidZone(zone);
      return zone.valid;
   }

   bool
   PstItems::ParseTimeZoneDefinition(const std::string &blob, int year, PstTimeZone &zone, PstBudget *budget)
   {
      zone = PstTimeZone();
      if (budget)
      {
         budget->Item();
         budget->Bytes(blob.size());
      }

      try
      {
         // TZDEFINITION: bMajorVersion 2, bMinorVersion, cbHeader, then the
         // header - wReserved, cchKeyName, KeyName, cRules - and the rules.
         if (PstFormat::U8(blob, 0, "a time zone") != 0x02)
            return false;

         const size_t headerSize = PstFormat::U16(blob, 2, "a time zone");
         const size_t keyLength = PstFormat::U16(blob, 6, "a time zone");
         if (keyLength > 260 || !PstFormat::Fits(8, keyLength * 2 + 2, blob.size()) || 4 + headerSize < 8 + keyLength * 2 + 2)
            return false;

         const std::string key = PstFormat::Utf16ToUtf8(blob.substr(8, keyLength * 2));
         const size_t rules = PstFormat::U16(blob, 8 + keyLength * 2, "a time zone");
         const size_t rulesAt = 4 + headerSize;
         if (rules == 0 || rules > 1024 || !PstFormat::Fits(rulesAt, (uint64_t) rules * 66, blob.size()))
            return false;

         // TZRULE, 66 bytes: versions, wReserved, wTZRuleFlags, wYear, X, then
         // the three biases and the two transitions. The rule for the year:
         // the latest that starts by it, else the one marked effective
         // (TZRULE_FLAG_EFFECTIVE_TZREG), else the first.
         size_t chosen = rules;
         int chosenYear = -1;
         size_t effective = rules;
         for (size_t i = 0; i < rules; i++)
         {
            if (budget)
               budget->Item();

            const size_t at = rulesAt + i * 66;
            const int flags = PstFormat::U16(blob, at + 4, "a time zone");
            const int ruleYear = PstFormat::U16(blob, at + 6, "a time zone");
            if ((flags & 0x0002) != 0 && effective == rules)
               effective = i;
            if (ruleYear <= year && ruleYear > chosenYear)
            {
               chosen = i;
               chosenYear = ruleYear;
            }
         }
         if (chosen == rules)
            chosen = effective == rules ? 0 : effective;

         const size_t at = rulesAt + chosen * 66;
         zone.bias = (int32_t) PstFormat::U32(blob, at + 22, "a time zone");
         zone.standardBias = (int32_t) PstFormat::U32(blob, at + 26, "a time zone");
         zone.daylightBias = (int32_t) PstFormat::U32(blob, at + 30, "a time zone");
         zone.standardDate = SystemTimeAt(blob, at + 34);
         zone.daylightDate = SystemTimeAt(blob, at + 50);
         zone.name = SafeName(key);
      }
      catch (PstBudgetExhausted &)
      {
         throw;
      }
      catch (PstError &)
      {
         zone = PstTimeZone();
         return false;
      }

      zone.valid = ValidZone(zone);
      return zone.valid;
   }

   int
   PstItems::NthWeekday(int year, int month, int weekday, int nth)
   {
      if (month < 1 || month > 12)
         return 1;
      const int64_t first = DaysFromCivil(year, month, 1);
      const int firstWeekday = (int) (((first + 4) % 7 + 7) % 7);
      int day = 1 + ((weekday - firstWeekday) % 7 + 7) % 7;
      day += ((nth < 1 ? 1 : (nth > 5 ? 5 : nth)) - 1) * 7;
      while (day > DaysInMonth(year, month))
         day -= 7;
      return day;
   }

   int64_t
   PstItems::LocalToUtc(const PstTimeZone &zone, int64_t localSeconds)
   {
      if (!zone.valid)
         return localSeconds;
      const int offset = DaylightAtLocal(zone, localSeconds) ? zone.DaylightOffset() : zone.StandardOffset();
      return localSeconds - (int64_t) offset * 60;
   }

   int64_t
   PstItems::UtcToLocal(const PstTimeZone &zone, int64_t utcSeconds)
   {
      if (!zone.valid)
         return utcSeconds;
      if (!zone.HasDaylight())
         return utcSeconds + (int64_t) zone.StandardOffset() * 60;

      int year, month, day, hour, minute, second;
      Civil(utcSeconds + (int64_t) zone.StandardOffset() * 60, year, month, day, hour, minute, second);
      const int64_t daylightStart = TransitionIn(zone.daylightDate, year) - (int64_t) zone.StandardOffset() * 60;
      const int64_t standardStart = TransitionIn(zone.standardDate, year) - (int64_t) zone.DaylightOffset() * 60;
      const bool daylight = daylightStart < standardStart ? (utcSeconds >= daylightStart && utcSeconds < standardStart)
                                                          : (utcSeconds >= daylightStart || utcSeconds < standardStart);
      return utcSeconds + (int64_t) (daylight ? zone.DaylightOffset() : zone.StandardOffset()) * 60;
   }

   bool
   PstItems::IsNoDate(uint64_t fileTime)
   {
      int64_t seconds = 0;
      if (!PstFormat::FileTimeToUnix(fileTime, seconds))
         return false;
      return seconds >= DaysFromCivil(4500, 1, 1) * 86400;
   }

   std::string
   PstItems::UidOfGlobalObjectId(const std::string &goid)
   {
      // Byte Array ID (16), YH, YL, M, D (4), Creation Time (8), X (8), Size
      // (4), Data (Size).
      static const unsigned char arrayId[16] = { 0x04, 0x00, 0x00, 0x00, 0x82, 0x00, 0xE0, 0x00, 0x74, 0xC5, 0xB7, 0x10, 0x1A, 0x82, 0xE0, 0x08 };
      if (goid.size() < 40 || goid.size() > 4096 || memcmp(goid.data(), arrayId, 16) != 0)
         return std::string();

      const uint32_t size = PstFormat::U32(goid, 36, "a global object id");
      if (!PstFormat::Fits(40, size, goid.size()))
         return std::string();

      const std::string data = goid.substr(40, size);
      static const char vcal[] = "vCal-Uid\x01\x00\x00\x00";
      if (data.size() > 12 && data.compare(0, 12, std::string(vcal, 12)) == 0)
      {
         std::string uid;
         for (size_t i = 12; i < data.size() && data[i] != '\0' && uid.size() < 255; i++)
         {
            const unsigned char c = (unsigned char) data[i];
            if (c < 0x21 || c > 0x7E)
            {
               uid.clear();
               break;
            }
            uid += (char) c;
         }
         if (!uid.empty())
            return uid;
      }

      std::string clean = goid;
      for (size_t i = 16; i < 20; i++)
         clean[i] = '\0';
      std::string hex = PstFormat::Hex(clean);
      for (size_t i = 0; i < hex.size(); i++)
      {
         if (hex[i] >= 'a' && hex[i] <= 'f')
            hex[i] = (char) (hex[i] - 'a' + 'A');
      }
      return hex;
   }

   // ---------------------------------------------------------------- appointments

   bool
   PstItems::AppointmentText(const PstAppointment &appointment, const std::string &fallbackUid, std::string &text,
                             std::string &word, std::string &problem, PstBudget *budget)
   {
      text.clear();
      const std::string uid = appointment.uid.empty() ? fallbackUid : appointment.uid;
      const std::string stamp = StampOf(appointment.modified, appointment.created, appointment.start);

      ICalComponent calendar = Calendar();
      ICalComponent master;
      master.name = "VEVENT";
      CommonProperties(appointment, master, uid, stamp, appointment.subject, appointment.location, appointment.busyStatus);

      int64_t start = 0;
      const bool hasStart = Seconds(appointment.start, start);

      if (appointment.recurring)
      {
         PstRecurrence recurrence;
         if (appointment.recurrenceBlob.empty())
         {
            word = "recurrence_unreadable";
            problem = "it is marked recurring and carries no recurrence pattern";
            return false;
         }
         if (!ParseRecurrence(appointment.recurrenceBlob, appointment.codePage, recurrence, problem, budget))
         {
            word = "recurrence_unreadable";
            return false;
         }

         // What the object will come to, counted before each part of it is
         // built: the series as it is now (its body, categories, organiser
         // and attendees - what every changed occurrence repeats), then each
         // EXDATE and each override as it is added.
         const size_t shared = Text(master).size();
         size_t expected = shared + 2048;
         auto tooLarge = [&]() -> bool
         {
            if (expected <= MaximumItemTextBytes)
               return false;
            word = "too_large";
            problem = "it would be larger than a calendar object may be (" + Number((int64_t) (MaximumItemTextBytes / 1024)) +
                      " KB), each of its changed occurrences repeating the series' description and attendees";
            return true;
         };
         if (tooLarge())
            return false;

         // The series' zone; with none, a zone of the fixed offset between
         // the pattern's first start and the appointment's own, to the
         // quarter hour.
         PstTimeZone zone = appointment.zone;
         const int64_t firstLocal = SecondsOfMinutes(recurrence.startDate) + (int64_t) recurrence.startTimeOffset * 60;
         if (!zone.valid)
         {
            int64_t offset = hasStart ? (firstLocal - start) / 60 : 0;
            offset = (offset / 15) * 15;
            if (offset < -14 * 60 || offset > 14 * 60)
               offset = 0;
            zone = PstTimeZone();
            zone.valid = true;
            zone.bias = (int) -offset;
            zone.name = "UTC" + OffsetText((int) offset);
         }
         const std::string safe = SafeName(zone.name);
         const std::string tzid = safe.empty() ? std::string("Outlook time zone") : safe;

         const bool allDay = appointment.allDay;
         calendar.children.push_back(TimeZoneComponent(zone, tzid));

         const int64_t day = SecondsOfMinutes(recurrence.startDate);
         if (allDay)
         {
            int64_t days = ((int64_t) recurrence.endTimeOffset - (int64_t) recurrence.startTimeOffset + 1439) / 1440;
            if (days < 1)
               days = 1;
            master.properties.push_back(LocalProperty("DTSTART", day, true, tzid));
            master.properties.push_back(LocalProperty("DTEND", day + days * 86400, true, tzid));
         }
         else
         {
            master.properties.push_back(LocalProperty("DTSTART", day + (int64_t) recurrence.startTimeOffset * 60, false, tzid));
            master.properties.push_back(LocalProperty("DTEND", day + (int64_t) recurrence.endTimeOffset * 60, false, tzid));
         }

         master.properties.push_back(IcsProperty("RRULE", RuleOf(recurrence, zone, allDay)));

         // The occurrences removed: EXDATE for each one that was deleted, and
         // an override - RECURRENCE-ID, its own times, subject and place - for
         // each that was changed.
         std::set<uint32_t> moved;
         for (const PstRecurrenceException &exception : recurrence.exceptions)
            moved.insert(exception.originalStart - exception.originalStart % 1440);

         std::set<uint32_t> removed;
         for (uint32_t date : recurrence.deleted)
         {
            if (moved.count(date) != 0 || !removed.insert(date).second)
               continue;
            expected += 64 + tzid.size();
            if (tooLarge())
               return false;
            const int64_t at = SecondsOfMinutes(date) + (allDay ? 0 : (int64_t) recurrence.startTimeOffset * 60);
            master.properties.push_back(LocalProperty("EXDATE", at, allDay, tzid));
         }

         calendar.children.push_back(master);

         std::set<uint32_t> overridden;
         for (const PstRecurrenceException &exception : recurrence.exceptions)
         {
            if (!overridden.insert(exception.originalStart).second)
               continue;

            if (budget)
               budget->Item();
            expected += shared + 2 * (exception.subject.size() + exception.location.size()) + 256;
            if (tooLarge())
               return false;

            ICalComponent occurrence;
            occurrence.name = "VEVENT";
            CommonProperties(appointment, occurrence, uid, stamp, exception.hasSubject ? exception.subject : appointment.subject,
                             exception.hasLocation ? exception.location : appointment.location,
                             exception.hasBusyStatus ? exception.busyStatus : appointment.busyStatus);

            const int64_t original = SecondsOfMinutes(allDay ? exception.originalStart - exception.originalStart % 1440 : exception.originalStart);
            occurrence.properties.push_back(LocalProperty("RECURRENCE-ID", original, allDay, tzid));

            const uint32_t end = exception.end < exception.start ? exception.start : exception.end;
            if (allDay)
            {
               const int64_t first = SecondsOfMinutes(exception.start - exception.start % 1440);
               int64_t last = SecondsOfMinutes(end - end % 1440);
               if (last <= first)
                  last = first + 86400;
               occurrence.properties.push_back(LocalProperty("DTSTART", first, true, tzid));
               occurrence.properties.push_back(LocalProperty("DTEND", last, true, tzid));
            }
            else
            {
               occurrence.properties.push_back(LocalProperty("DTSTART", SecondsOfMinutes(exception.start), false, tzid));
               occurrence.properties.push_back(LocalProperty("DTEND", SecondsOfMinutes(end), false, tzid));
            }
            calendar.children.push_back(occurrence);
         }

         text = Text(calendar);
         return true;
      }

      if (!hasStart)
      {
         word = "no_start";
         problem = "it has no start time";
         return false;
      }

      int64_t end = start;
      if (!Seconds(appointment.end, end) || end < start)
         end = start;

      if (appointment.allDay)
      {
         // Midnight to midnight where it was made: its zone, else the day the
         // start is nearest to.
         const PstTimeZone &zone = appointment.startZone.valid ? appointment.startZone : appointment.zone;
         int64_t first = zone.valid ? UtcToLocal(zone, start) : start + 43200;
         int64_t last = zone.valid ? UtcToLocal(zone, end) : end + 43200;
         first -= ((first % 86400) + 86400) % 86400;
         last -= ((last % 86400) + 86400) % 86400;
         if (last <= first)
            last = first + 86400;
         master.properties.push_back(LocalProperty("DTSTART", first, true, std::string()));
         master.properties.push_back(LocalProperty("DTEND", last, true, std::string()));
      }
      else
      {
         master.properties.push_back(IcsProperty("DTSTART", DateTimeText(start, true)));
         master.properties.push_back(IcsProperty("DTEND", DateTimeText(end, true)));
      }

      calendar.children.push_back(master);
      text = Text(calendar);
      return true;
   }

   // ---------------------------------------------------------------- tasks

   std::string
   PstItems::TaskText(const PstTask &task, const std::string &uid)
   {
      ICalComponent calendar = Calendar();
      ICalComponent todo;
      todo.name = "VTODO";
      todo.properties.push_back(TextProperty("UID", uid));
      todo.properties.push_back(IcsProperty("DTSTAMP", StampOf(task.modified, task.created, 0)));
      if (!task.subject.empty())
         todo.properties.push_back(TextProperty("SUMMARY", task.subject));
      if (!Trim(task.body).empty())
         todo.properties.push_back(TextProperty("DESCRIPTION", task.body));

      // PidLidTaskStartDate and PidLidTaskDueDate are days ([MS-OXOTASK]
      // 2.2.2.2.4, 2.2.2.2.5): midnight of the day, kept as UTC.
      int64_t start = 0;
      int64_t due = 0;
      const bool hasStart = Seconds(task.start, start);
      const bool hasDue = Seconds(task.due, due);
      if (hasStart && (!hasDue || DateText(start) < DateText(due)))
      {
         ICalProperty property = IcsProperty("DTSTART", DateText(start));
         Parameter(property, "VALUE", "DATE");
         todo.properties.push_back(property);
      }
      if (hasDue)
      {
         ICalProperty property = IcsProperty("DUE", DateText(due));
         Parameter(property, "VALUE", "DATE");
         todo.properties.push_back(property);
      }

      const bool complete = task.complete || task.status == 2;
      todo.properties.push_back(IcsProperty("STATUS", complete ? "COMPLETED" : (task.status == 1 ? "IN-PROCESS" : "NEEDS-ACTION")));
      if (complete || task.percent >= 0)
         todo.properties.push_back(IcsProperty("PERCENT-COMPLETE", Number(complete ? 100 : (task.percent > 100 ? 100 : task.percent))));

      int64_t completed = 0;
      if (complete && Seconds(task.completedAt, completed))
         todo.properties.push_back(IcsProperty("COMPLETED", DateTimeText(completed, true)));

      if (task.importance == 2)
         todo.properties.push_back(IcsProperty("PRIORITY", "1"));
      else if (task.importance == 0)
         todo.properties.push_back(IcsProperty("PRIORITY", "9"));

      const std::string categories = EscapedList(task.categories);
      if (!categories.empty())
         todo.properties.push_back(IcsProperty("CATEGORIES", categories));
      if (task.sensitivity == 2 || task.sensitivity == 3)
         todo.properties.push_back(IcsProperty("CLASS", task.sensitivity == 2 ? "PRIVATE" : "CONFIDENTIAL"));

      int64_t reminder = 0;
      if (task.reminderSet && Seconds(task.reminderTime, reminder))
      {
         ICalComponent alarm;
         alarm.name = "VALARM";
         alarm.properties.push_back(IcsProperty("ACTION", "DISPLAY"));
         alarm.properties.push_back(TextProperty("DESCRIPTION", task.subject.empty() ? std::string("Reminder") : task.subject));
         ICalProperty trigger = IcsProperty("TRIGGER", DateTimeText(reminder, true));
         Parameter(trigger, "VALUE", "DATE-TIME");
         alarm.properties.push_back(trigger);
         todo.children.push_back(alarm);
      }

      calendar.children.push_back(todo);
      return Text(calendar);
   }

   // ---------------------------------------------------------------- contacts

   std::string
   PstItems::ContactName(const PstContact &contact)
   {
      if (!Trim(contact.displayName).empty())
         return Trim(contact.displayName);

      const std::string assembled = Trim(Trim(contact.givenName) + " " + Trim(contact.surname));
      if (!assembled.empty())
         return assembled;
      if (!Trim(contact.fileUnder).empty())
         return Trim(contact.fileUnder);
      if (!Trim(contact.company).empty())
         return Trim(contact.company);
      return contact.emails.empty() ? std::string() : contact.emails[0];
   }

   std::string
   PstItems::ContactText(const PstContact &contact, const std::string &uid, bool withPhoto)
   {
      auto escape = [](const std::string &utf8) -> std::string
      {
         const AnsiString escaped = VCard::Escape(AnsiString(Trim(utf8)));
         return std::string(escaped.c_str(), escaped.size());
      };
      auto line = [](const std::string &text) -> std::string
      {
         const AnsiString folded = VCard::Fold(AnsiString(text));
         return std::string(folded.c_str(), folded.size()) + "\r\n";
      };

      std::string card = "BEGIN:VCARD\r\nVERSION:4.0\r\nPRODID:-//hMailServer//PST import//EN\r\n";
      card += line("UID:" + escape(uid));
      card += line("FN:" + escape(ContactName(contact)));
      card += line("N:" + escape(contact.surname) + ";" + escape(contact.givenName) + ";" + escape(contact.middleName) + ";" +
                   escape(contact.prefix) + ";" + escape(contact.generation));
      if (!Trim(contact.nickname).empty())
         card += line("NICKNAME:" + escape(contact.nickname));
      if (!Trim(contact.company).empty() || !Trim(contact.department).empty())
         card += line("ORG:" + escape(contact.company) + (Trim(contact.department).empty() ? std::string() : ";" + escape(contact.department)));
      if (!Trim(contact.title).empty())
         card += line("TITLE:" + escape(contact.title));

      for (size_t i = 0; i < contact.emails.size(); i++)
         card += line(std::string("EMAIL") + (i == 0 ? ";PREF=1" : "") + ":" + escape(contact.emails[i]));

      for (const PstContact::Phone &phone : contact.phones)
         card += line("TEL" + (phone.types.empty() ? std::string() : ";TYPE=" + phone.types) + ":" + escape(phone.number));

      for (const PstContact::Address &address : contact.addresses)
      {
         card += line("ADR" + (address.type.empty() ? std::string() : ";TYPE=" + address.type) + ":" + escape(address.poBox) + ";;" +
                      escape(address.street) + ";" + escape(address.city) + ";" + escape(address.region) + ";" +
                      escape(address.postalCode) + ";" + escape(address.country));
      }

      if (!contact.birthday.empty())
         card += line("BDAY:" + contact.birthday);

      for (const std::string &url : contact.urls)
         card += line("URL:" + escape(url));

      std::string categories;
      for (const std::string &category : contact.categories)
      {
         if (Trim(category).empty())
            continue;
         categories += (categories.empty() ? "" : ",") + escape(category);
      }
      if (!categories.empty())
         card += line("CATEGORIES:" + categories);

      if (!Trim(contact.note).empty())
         card += line("NOTE:" + escape(contact.note));

      if (withPhoto && !contact.photo.empty() && !contact.photoType.empty())
      {
         std::string base64 = PstMime::Base64(contact.photo);
         std::string unfolded;
         unfolded.reserve(base64.size());
         for (size_t i = 0; i < base64.size(); i++)
         {
            if (base64[i] != '\r' && base64[i] != '\n')
               unfolded += base64[i];
         }
         card += line("PHOTO:data:" + contact.photoType + ";base64," + unfolded);
      }

      int64_t modified = 0;
      if (Seconds(contact.modified, modified))
         card += line("REV:" + DateTimeText(modified, true));

      card += "END:VCARD\r\n";
      return card;
   }

   std::string
   PstItems::CutToUnits(const std::string &utf8, size_t maximum)
   {
      size_t units = 0;
      size_t i = 0;
      while (i < utf8.size())
      {
         // The sequence's length from its lead byte; a byte that leads none
         // is one character of its own.
         const unsigned char lead = (unsigned char) utf8[i];
         size_t length = 1;
         if (lead >= 0xF0 && lead <= 0xF4)
            length = 4;
         else if (lead >= 0xE0)
            length = 3;
         else if (lead >= 0xC2 && lead <= 0xDF)
            length = 2;
         if (length > 1)
         {
            for (size_t k = 1; k < length; k++)
            {
               if (i + k >= utf8.size() || ((unsigned char) utf8[i + k] & 0xC0) != 0x80)
               {
                  length = 1;
                  break;
               }
            }
         }

         // A character past U+FFFF is two UTF-16 code units.
         const size_t needs = length == 4 ? 2 : 1;
         if (units + needs > maximum)
            break;
         units += needs;
         i += length;
      }
      return utf8.substr(0, i);
   }

   bool
   PstItems::RtfBodyText(const std::string &compressed, std::string &text, std::string &problem, PstBudget *budget)
   {
      text.clear();

      // The header ([MS-OXRTFCP] 2.1.3.1.1) checked first: a value that is not
      // one costs nothing. Then what it can produce is charged before anything
      // is decompressed: at most RAWSIZE, MaximumRtfBytes, and nine times its
      // own bytes - LZFu turns two bytes into at most seventeen, MELA one into
      // one.
      if (compressed.size() < 16)
      {
         problem = "its RTF is shorter than its header";
         return false;
      }
      const uint32_t compType = PstFormat::U32(compressed, 8, "an RTF header");
      if (compType != 0x75465A4C && compType != 0x414C454D)
      {
         problem = "its RTF is neither compressed nor stored as [MS-OXRTFCP] describes";
         return false;
      }
      if (budget)
      {
         uint64_t claimed = PstFormat::U32(compressed, 4, "an RTF header");
         if (claimed > MaximumRtfBytes)
            claimed = MaximumRtfBytes;
         if (claimed > 9 * (uint64_t) compressed.size())
            claimed = 9 * (uint64_t) compressed.size();
         budget->Item();
         budget->Bytes(claimed);
      }

      std::string rtf;
      if (!PstRtf::Decompress(compressed, MaximumRtfBytes, rtf, problem))
         return false;

      const PstRtf::Kind kind = PstRtf::KindOf(rtf);
      if (kind == PstRtf::KindHtml)
      {
         problem = "it is HTML carried in RTF, which is not read as plain text";
         return false;
      }

      // Plain text carried in RTF, or a body written in RTF: its visible text,
      // as the mail phase takes it for a message's text part.
      text = PstRtf::Deencapsulate(rtf, false);
      return true;
   }

   std::string
   PstItems::ImageType(const std::string &bytes)
   {
      if (bytes.size() >= 3 && (unsigned char) bytes[0] == 0xFF && (unsigned char) bytes[1] == 0xD8 && (unsigned char) bytes[2] == 0xFF)
         return "image/jpeg";
      if (bytes.size() >= 8 && bytes.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0)
         return "image/png";
      if (bytes.size() >= 6 && (bytes.compare(0, 6, "GIF87a") == 0 || bytes.compare(0, 6, "GIF89a") == 0))
         return "image/gif";
      return std::string();
   }

   // ---------------------------------------------------------------- journal

   std::string
   PstItems::JournalText(const PstJournal &journal)
   {
      std::string text = "Journal entry: " + (Trim(journal.type).empty() ? std::string("(no type)") : Trim(journal.type)) + "\r\n";

      int64_t start = 0;
      int64_t end = 0;
      if (Seconds(journal.start, start))
         text += "Start: " + ReadableText(start) + "\r\n";
      if (Seconds(journal.end, end))
         text += "End: " + ReadableText(end) + "\r\n";
      if (journal.duration >= 0)
         text += "Duration: " + Number(journal.duration) + " minute" + (journal.duration == 1 ? "" : "s") + "\r\n";

      auto list = [](const std::vector<std::string> &values) -> std::string
      {
         std::string out;
         for (const std::string &value : values)
         {
            if (!Trim(value).empty())
               out += (out.empty() ? "" : ", ") + Trim(value);
         }
         return out;
      };
      if (!list(journal.contacts).empty())
         text += "Contacts: " + list(journal.contacts) + "\r\n";
      if (!list(journal.companies).empty())
         text += "Companies: " + list(journal.companies) + "\r\n";

      return text + "\r\n";
   }
}
