// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// iMIP (RFC 6047): invitations, replies, cancellations and counter-proposals
// carried by mail as text/calendar parts with a METHOD - the one standards
// path between this server's calendar and Exchange, Microsoft 365, Google and
// every desktop client. This unit reads the part out of a message file, files
// a REPLY into the organiser's stored event when the reply is delivered, a
// CANCEL into the guest's, and a REQUEST into the guest's default calendar as
// it arrived, so that the guest's page shows the invitation as tentative and
// a delegate who may answer for them sees it in the shared calendar; and it
// builds the mails the page sends: REQUEST to each attendee, REPLY to the
// organiser - by the attendee, or by a delegate on the attendee's behalf, with
// SENT-BY on the ATTENDEE and Sender: on the mail - CANCEL, COUNTER. The REST
// half - what the page asks - is RestApiCalendar.cpp.

#pragma once

#include <vector>
#include "ICalendar.h"

namespace HM
{
   class Account;
   struct CalendarRecord;
   struct CalendarObjectRecord;

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
      // A REQUEST for an account that does not organise the event is kept in
      // the account's default calendar as it arrived - the account's PARTSTAT
      // what the organiser wrote, NEEDS-ACTION as a rule - replacing an older
      // copy of the UID and never a newer one; an account with no calendar row
      // yet gets nothing filed, since a delivery is not a use. Anything else is
      // left for the page. Never throws; says what it did in the log.
      static void FileOnDelivery(std::shared_ptr<const Account> account, const String &fileName);

      // The invitation's event kept in the account's calendars: the copy that
      // holds the UID replaced when the invitation is the same or a later
      // sequence, an older invitation not undoing a newer; a new object in the
      // default calendar when nothing holds the UID - made when createDefault,
      // else refused when there is none. cancelled marks every component
      // STATUS:CANCELLED; a partstat, when given, is set for the address
      // (with the name) before the object is written. The page's answer and
      // the delivery filer are this one path, so a REQUEST filed on delivery
      // and then answered from the message is one event, never two. What was
      // written comes back in calendar and record.
      static bool KeepInvitation(__int64 accountId, const ImipMessage &invitation, bool createDefault, bool cancelled,
                                 const AnsiString &address, const AnsiString &name, const AnsiString &partstat,
                                 CalendarRecord &calendar, CalendarObjectRecord &record, AnsiString &problem);

      // "mailto:Some.One@Example.com" to "some.one@example.com".
      static AnsiString AddressOf(const AnsiString &calAddress);

      // The object with its METHOD replaced (or added), serialised.
      static AnsiString WithMethod(const ICalComponent &calendar, const AnsiString &method);

      // Every VEVENT/VTODO of the object: the attendee's PARTSTAT set, or the
      // attendee added with it when the object does not list them yet. sentBy
      // is the address of a delegate who answered for the attendee, written as
      // SENT-BY="mailto:..." on the ATTENDEE (RFC 5545 section 3.2.18); empty
      // when the attendee answered, and any SENT-BY there was goes.
      static void SetPartstat(ICalComponent &calendar, const AnsiString &address, const AnsiString &name, const AnsiString &partstat, const AnsiString &sentBy = AnsiString());

      // The REPLY object for one attendee (RFC 5546 section 3.2.3): the
      // masters' identity, the organiser, and that one attendee with their
      // answer - nothing another attendee said. sentBy, when given, is the
      // delegate who answered, on the ATTENDEE as SENT-BY.
      static AnsiString ReplyObject(const ImipMessage &invitation, const AnsiString &address, const AnsiString &name, const AnsiString &partstat, const AnsiString &sentBy = AnsiString());

      // A mail from the account carrying the object as text/calendar with the
      // method, and the same words as text/plain for a client without a
      // calendar: what QueueRawMessage_ writes as sent. senderHeader, when
      // given, is written as Sender: beside the From - the delegate beside
      // the owner, which a mail client shows as "on behalf of".
      static AnsiString BuildMail(const AnsiString &fromHeader, const AnsiString &to, const AnsiString &subject,
                                  const AnsiString &bodyText, const AnsiString &method, const AnsiString &calendarText, const AnsiString &fromAddress,
                                  const AnsiString &senderHeader = AnsiString());

      // The words a person reads about an event: when, where, who.
      static AnsiString Describe(const ImipMessage &message);

      // "Sep 19, 2026 09:00 - 10:00 UTC" for the mail's subject and text.
      static AnsiString WhenText(__int64 start, __int64 end, bool allDay);

      // A UID's resource name in a calendar: the UID with anything a path
      // cannot carry replaced, and ".ics".
      static String UriFor(const AnsiString &uid);
   };
}
