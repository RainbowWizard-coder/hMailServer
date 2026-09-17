// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// iMIP (RFC 6047): invitations, replies, cancellations and counter-proposals
// carried by mail as text/calendar parts with a METHOD - the one standards
// path between this server's calendar and Exchange, Microsoft 365, Google and
// every desktop client. This unit reads the part out of a message file, files
// a REPLY into the organiser's stored event when the reply is delivered, and
// builds the mails the page sends: REQUEST to each attendee, REPLY to the
// organiser, CANCEL, COUNTER. The REST half - what the page asks - is
// RestApiCalendar.cpp.

#pragma once

#include <vector>
#include "ICalendar.h"

namespace HM
{
   class Account;

   struct ImipAttendee
   {
      AnsiString address;    // lower-cased, without mailto:
      AnsiString name;       // the CN parameter, or ""
      AnsiString partstat;   // NEEDS-ACTION, ACCEPTED, TENTATIVE, DECLINED, ...
      AnsiString role;       // REQ-PARTICIPANT, OPT-PARTICIPANT, CHAIR, ...
   };

   // What one text/calendar part says, read once for the page and the filer.
   struct ImipMessage
   {
      ImipMessage() : start(0), end(0), allDay(false), recurring(false), sequence(0) { }

      AnsiString method;       // REQUEST, REPLY, CANCEL, COUNTER, PUBLISH, ADD, REFRESH, DECLINECOUNTER, or "" for a bare object
      AnsiString component;    // VEVENT or VTODO
      AnsiString uid;
      AnsiString summary;
      AnsiString location;
      AnsiString description;
      __int64 start;           // UTC seconds of the master's start
      __int64 end;
      bool allDay;
      AnsiString tzid;         // the DTSTART's zone, or ""
      bool recurring;
      AnsiString rrule;
      int sequence;
      AnsiString organizer;    // lower-cased address, or ""
      AnsiString organizerName;
      std::vector<ImipAttendee> attendees;

      AnsiString text;         // the VCALENDAR as found
      ICalComponent tree;
   };

   class Imip
   {
   public:
      // The first text/calendar part of the message file, or an attachment
      // named *.ics, transfer-decoded: false when the message has none. The
      // file's head is looked at first, so a message without one costs a read
      // of a few kilobytes and no MIME parse.
      static bool FindInFile(const String &fileName, AnsiString &calendarText);

      // The part read: the METHOD, the master component and who is in it.
      static bool Read(const AnsiString &calendarText, ImipMessage &out, AnsiString &problem);

      // A REPLY delivered to the organiser is filed into the stored event: the
      // attendee's PARTSTAT set to what they answered, in every calendar of the
      // account that holds the UID. A CANCEL marks the stored event cancelled.
      // Anything else is left for the page. Never throws; says what it did in
      // the log.
      static void FileOnDelivery(std::shared_ptr<const Account> account, const String &fileName);

      // "mailto:Some.One@Example.com" to "some.one@example.com".
      static AnsiString AddressOf(const AnsiString &calAddress);

      // The object with its METHOD replaced (or added), serialised.
      static AnsiString WithMethod(const ICalComponent &calendar, const AnsiString &method);

      // Every VEVENT/VTODO of the object: the attendee's PARTSTAT set, or the
      // attendee added with it when the object does not list them yet.
      static void SetPartstat(ICalComponent &calendar, const AnsiString &address, const AnsiString &name, const AnsiString &partstat);

      // The REPLY object for one attendee (RFC 5546 section 3.2.3): the
      // masters' identity, the organiser, and that one attendee with their
      // answer - nothing another attendee said.
      static AnsiString ReplyObject(const ImipMessage &invitation, const AnsiString &address, const AnsiString &name, const AnsiString &partstat);

      // A mail from the account carrying the object as text/calendar with the
      // method, and the same words as text/plain for a client without a
      // calendar: what QueueRawMessage_ writes as sent.
      static AnsiString BuildMail(const AnsiString &fromHeader, const AnsiString &to, const AnsiString &subject,
                                  const AnsiString &bodyText, const AnsiString &method, const AnsiString &calendarText, const AnsiString &fromAddress);

      // The words a person reads about an event: when, where, who.
      static AnsiString Describe(const ImipMessage &message);

      // "Sep 19, 2026 09:00 - 10:00 UTC" for the mail's subject and text.
      static AnsiString WhenText(__int64 start, __int64 end, bool allDay);

      // A UID's resource name in a calendar: the UID with anything a path
      // cannot carry replaced, and ".ics".
      static String UriFor(const AnsiString &uid);
   };
}
