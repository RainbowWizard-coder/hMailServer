// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// What a PST holds besides mail (docs/Migration.md, "Outlook data files (PST)"):
// contacts, calendar items and tasks, read by PstStore (PstStoreItems.cpp) into
// the structures below, and written here as the text this server's address
// book and calendars keep - a vCard 4.0 (RFC 6350) for a contact, an iCalendar
// VEVENT or VTODO (RFC 5545) for an appointment or a task - the same text a
// CardDAV or CalDAV client would have sent.
//
// Written from Microsoft's published specifications: [MS-OXOCNTC] (contacts),
// [MS-OXOCAL] (appointments: the recurrence blob of 2.2.1.44, the time zone
// structures of 2.2.1.39 and 2.2.1.41, the global object id of 2.2.1.27),
// [MS-OXOTASK] (tasks), [MS-OXCICAL] (how an appointment's properties map to
// iCalendar) and [MS-OXPROPS] (the property ids and named-property sets).
//
// Every structure read from the file is untrusted, as everywhere in the reader:
// the recurrence blob's counts, lengths and dates are checked against what the
// blob holds and against what iCalendar can say before anything is built from
// them, and a blob that does not hold is refused with the reason - the one
// appointment is left out, never guessed at. Given the file's work budget
// (PstFormat.h), the blob, the time zones and each changed occurrence built are
// charged to it, and the job's stop is asked at each; what the budget and the
// stop throw (PstBudgetExhausted, PstStopped) is never taken for a blob that
// does not hold, and goes to the caller as it came.

#pragma once

#include "PstStore.h"

namespace HM
{
   // SYSTEMTIME as [MS-OXOCAL] 2.2.1.39 uses it for a time zone's transitions:
   // with year 0, day is the week of the month (1 to 4, 5 the last) and
   // dayOfWeek the weekday (0 Sunday); with a year, the date itself.
   struct PstSystemTime
   {
      PstSystemTime() : year(0), month(0), dayOfWeek(0), day(0), hour(0), minute(0), second(0) { }

      int year, month, dayOfWeek, day, hour, minute, second;
   };

   // A time zone as Outlook keeps one with an appointment: TZSTRUCT
   // (PidLidTimeZoneStruct) or the rule of a TZDEFINITION
   // (PidLidAppointmentTimeZoneDefinition*). UTC = local + bias + the standard
   // or the daylight bias, in minutes.
   struct PstTimeZone
   {
      PstTimeZone() : valid(false), bias(0), standardBias(0), daylightBias(0) { }

      bool valid;
      std::string name;            // the TZID written: the Windows key name, else the description (UTF-8)
      int bias;
      int standardBias;
      int daylightBias;
      PstSystemTime standardDate;  // when standard time begins (in daylight time)
      PstSystemTime daylightDate;  // when daylight time begins (in standard time)

      bool HasDaylight() const { return standardDate.month != 0 && daylightDate.month != 0; }

      // Minutes east of UTC.
      int StandardOffset() const { return -(bias + standardBias); }
      int DaylightOffset() const { return -(bias + daylightBias); }
   };

   // One changed occurrence of a recurring appointment ([MS-OXOCAL] 2.2.1.44.2
   // ExceptionInfo, with the Unicode subject and location of 2.2.1.44.3). Times
   // are minutes since 1601-01-01 in the series' own zone.
   struct PstRecurrenceException
   {
      PstRecurrenceException() : start(0), end(0), originalStart(0), overrideFlags(0), hasSubject(false), hasLocation(false),
                                 hasBusyStatus(false), busyStatus(0) { }

      uint32_t start;
      uint32_t end;
      uint32_t originalStart;
      int overrideFlags;
      bool hasSubject;
      std::string subject;         // UTF-8
      bool hasLocation;
      std::string location;
      bool hasBusyStatus;
      int busyStatus;
   };

   // [MS-OXOCAL] 2.2.1.44.1 RecurrencePattern and 2.2.1.44.5
   // AppointmentRecurrencePattern, read and checked.
   struct PstRecurrence
   {
      PstRecurrence() : frequency(0), patternType(0), calendarType(0), period(0), dayMask(0), dayOfMonth(0), nth(0), endType(0),
                        occurrenceCount(0), firstDayOfWeek(0), startDate(0), endDate(0), startTimeOffset(0), endTimeOffset(0) { }

      int frequency;               // 0x200A daily, 0x200B weekly, 0x200C monthly, 0x200D yearly
      int patternType;             // 0 day, 1 week, 2 month, 3 month-nth, 4 month-end
      int calendarType;
      uint32_t period;             // minutes (day), weeks, or months
      uint32_t dayMask;            // week and month-nth: bit 0 Sunday .. bit 6 Saturday
      uint32_t dayOfMonth;         // month and month-end
      uint32_t nth;                // month-nth: 1 to 4, 5 the last
      uint32_t endType;            // 0x2021 by a date, 0x2022 after a count, 0x2023 never
      uint32_t occurrenceCount;
      uint32_t firstDayOfWeek;     // 0 Sunday
      std::vector<uint32_t> deleted;    // the original dates, midnight, of the occurrences removed or moved
      std::vector<uint32_t> modified;   // the dates, midnight, the moved ones start on
      uint32_t startDate;          // midnight of the first occurrence
      uint32_t endDate;            // midnight of the last
      uint32_t startTimeOffset;    // minutes from midnight to the start
      uint32_t endTimeOffset;      // and to the end
      std::vector<PstRecurrenceException> exceptions;
   };

   struct PstAttendee
   {
      PstAttendee() : type(1), flags(0), trackStatus(0) { }

      int type;                    // PidTagRecipientType: 1 required, 2 optional, 3 a resource
      int flags;                   // PidTagRecipientFlags: 0x2 the organiser
      int trackStatus;             // PidTagRecipientTrackStatus: 2 tentative, 3 accepted, 4 declined
      std::string name;            // UTF-8
      std::string address;         // SMTP, or empty
   };

   struct PstAppointment
   {
      PstAppointment() : nid(0), start(0), end(0), created(0), modified(0), allDay(false), recurring(false), meeting(false),
                         canceled(false), busyStatus(-1), reminderSet(false), reminderMinutes(15), sensitivity(0), codePage(1252) { }

      uint32_t nid;
      std::string messageClass;
      std::string subject;
      std::string location;
      std::string body;
      std::string uid;             // from PidLidGlobalObjectId, or empty
      uint64_t start;              // FILETIME, UTC; 0 when absent
      uint64_t end;
      uint64_t created;
      uint64_t modified;
      bool allDay;
      bool recurring;
      bool meeting;
      bool canceled;
      int busyStatus;              // PidLidBusyStatus: 0 free, 1 tentative, 2 busy, 3 away, 4 elsewhere; -1 absent
      bool reminderSet;
      int reminderMinutes;
      int sensitivity;             // PidTagSensitivity: 2 private, 3 confidential
      int codePage;
      std::string recurrenceBlob;  // PidLidAppointmentRecur
      PstTimeZone zone;            // the series' zone: the recurring TZDEFINITION, else TZSTRUCT
      PstTimeZone startZone;       // PidLidAppointmentTimeZoneDefinitionStartDisplay
      std::vector<std::string> categories;
      std::string organizerName;
      std::string organizerAddress;
      std::vector<PstAttendee> attendees;
      std::vector<PstWarning> warnings;
   };

   struct PstTask
   {
      PstTask() : nid(0), start(0), due(0), completedAt(0), created(0), modified(0), status(0), percent(-1), complete(false),
                  importance(1), sensitivity(0), reminderSet(false), reminderTime(0) { }

      uint32_t nid;
      std::string messageClass;
      std::string subject;
      std::string body;
      uint64_t start;              // PidLidTaskStartDate: a date, as midnight UTC of the day
      uint64_t due;                // PidLidTaskDueDate
      uint64_t completedAt;        // PidLidTaskDateCompleted
      uint64_t created;
      uint64_t modified;
      int status;                  // PidLidTaskStatus: 0 not started, 1 in progress, 2 complete, 3 waiting, 4 deferred
      int percent;                 // PidLidPercentComplete as 0-100; -1 absent
      bool complete;               // PidLidTaskComplete
      int importance;              // PidTagImportance: 0 low, 1 normal, 2 high
      int sensitivity;
      bool reminderSet;
      uint64_t reminderTime;       // PidLidReminderTime, UTC
      std::vector<std::string> categories;
      std::vector<PstWarning> warnings;
   };

   struct PstContact
   {
      PstContact() : nid(0), modified(0) { }

      struct Phone
      {
         std::string number;
         std::string types;        // vCard 4 TYPE list: "work,voice", "cell"
      };

      struct Address
      {
         std::string type;         // "home", "work", or empty
         std::string poBox, street, city, region, postalCode, country;
      };

      uint32_t nid;
      std::string messageClass;
      std::string displayName;
      std::string fileUnder;
      std::string givenName, surname, middleName, prefix, generation, nickname;
      std::string company, department, title;
      std::vector<std::string> emails;   // SMTP addresses, Email1 to Email3 in order, each once
      std::vector<Phone> phones;
      std::vector<Address> addresses;
      std::string birthday;        // YYYYMMDD, or empty
      std::string note;
      std::vector<std::string> urls;
      std::vector<std::string> categories;
      std::string photo;           // the picture's bytes, a JPEG, PNG or GIF
      std::string photoType;       // image/jpeg ...
      uint64_t modified;
      std::vector<PstWarning> warnings;
   };

   // A journal entry's fields ([MS-OXPROPS] PSETID_Log), for the text above it.
   struct PstJournal
   {
      PstJournal() : start(0), end(0), duration(-1) { }

      std::string type;            // PidLidLogType: "Phone call"
      uint64_t start;
      uint64_t end;
      int duration;                // minutes; -1 absent
      std::vector<std::string> contacts;
      std::vector<std::string> companies;
   };

   class PstItems
   {
   public:
      // [MS-OXOCAL] 2.2.1.44: the recurrence blob, read and checked. False with
      // the reason when it is not one iCalendar can say: a version, a pattern,
      // a calendar or a count that is not [MS-OXOCAL]'s, a Hijri pattern, a
      // length that runs past the blob. codePage decodes an exception's
      // eight-bit subject where it has no Unicode one. budget, where given, is
      // charged the blob's bytes and each changed occurrence.
      static bool ParseRecurrence(const std::string &blob, int codePage, PstRecurrence &recurrence, std::string &problem,
                                  PstBudget *budget = nullptr);

      // PidLidTimeZoneStruct ([MS-OXOCAL] 2.2.1.39), 48 bytes; and a
      // TZDEFINITION ([MS-OXOCAL] 2.2.1.41), whose rule for year (the latest one
      // not after it, else the one marked effective) is taken, and whose key
      // name is the zone's. False for bytes that are not one. budget, where
      // given, is charged the bytes and each rule.
      static bool ParseTimeZoneStruct(const std::string &blob, PstTimeZone &zone, PstBudget *budget = nullptr);
      static bool ParseTimeZoneDefinition(const std::string &blob, int year, PstTimeZone &zone, PstBudget *budget = nullptr);

      // [MS-OXCICAL] 2.1.3.1.1.20.26: the UID a PidLidGlobalObjectId stands for
      // - the one it carries after "vCal-Uid", else its bytes in hex, the
      // instance's date zeroed. Empty for bytes that are not one.
      static std::string UidOfGlobalObjectId(const std::string &goid);

      // A wall-clock time in the zone to UTC, and back; seconds since 1970.
      static int64_t LocalToUtc(const PstTimeZone &zone, int64_t localSeconds);
      static int64_t UtcToLocal(const PstTimeZone &zone, int64_t utcSeconds);

      // The day of the month of the nth weekday (0 Sunday) of a month, 5 the
      // last.
      static int NthWeekday(int year, int month, int weekday, int nth);

      // The VEVENT of an appointment, with its VTIMEZONE and its changed
      // occurrences, as iCalendar text. False with a word - "no_start", an
      // appointment with no time; "recurrence_unreadable", a recurring one
      // whose pattern cannot be read or said; "too_large", a series whose
      // removed and changed occurrences would make it larger than
      // MaximumItemTextBytes - and a sentence. Each changed occurrence repeats
      // the series' body, categories and attendees, so what they would come to
      // is counted before each one is built, never after: a blob of 65,535
      // changed occurrences cannot make as many copies of a 256 KB body.
      // budget, where given, is charged the blob and each occurrence built.
      static bool AppointmentText(const PstAppointment &appointment, const std::string &fallbackUid, std::string &text,
                                  std::string &word, std::string &problem, PstBudget *budget = nullptr);

      // The VTODO of a task.
      static std::string TaskText(const PstTask &task, const std::string &uid);

      // The vCard 4.0 of a contact; the name the address book keeps it under.
      static std::string ContactText(const PstContact &contact, const std::string &uid, bool withPhoto);
      static std::string ContactName(const PstContact &contact);

      // The text a journal entry is given above its own.
      static std::string JournalText(const PstJournal &journal);

      // A picture's media type from its first bytes: a JPEG, a PNG or a GIF,
      // else empty.
      static std::string ImageType(const std::string &bytes);

      // UTF-8 text cut to at most maximum UTF-16 code units - what a name
      // column of nvarchar(maximum) holds - at a character, never inside one.
      static std::string CutToUnits(const std::string &utf8, size_t maximum);

      // The text of a body kept only as RTF (PR_RTF_COMPRESSED), through the
      // RTF reader the mail phase uses: plain text carried in RTF, or the
      // visible text of a body written in RTF. False with the reason for RTF
      // that cannot be read and for HTML carried in RTF, which is not plain
      // text; at most MaximumRtfBytes of RTF are read, charged to budget, where
      // it is given, before they are decompressed.
      static bool RtfBodyText(const std::string &compressed, std::string &text, std::string &problem, PstBudget *budget = nullptr);

      // An Outlook date of none: Outlook writes 4501-01-01 for "no date".
      static bool IsNoDate(uint64_t fileTime);

      // A class and its sub-classes, without regard to case: "IPM.Contact" and
      // "IPM.Contact.x", not "IPM.Contacts". What the contacts and calendars
      // phases take an item or a folder by, in the service and in the reader's
      // process alike.
      static bool IsOfClass(const std::string &value, const char *base);

      // The most a recurrence blob, a photo, a card or event, and a body's
      // RTF once decompressed may be.
      static const size_t MaximumRecurrenceBytes = 1024 * 1024;
      static const size_t MaximumPhotoBytes = 512 * 1024;
      static const size_t MaximumItemTextBytes = 1024 * 1024;
      static const size_t MaximumRtfBytes = 4 * 1024 * 1024;
   };
}
