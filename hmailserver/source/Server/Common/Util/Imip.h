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

#include <functional>
#include <vector>
#include "ICalendar.h"

namespace HM
{
   class Account;
   class Message;
   struct CalendarRecord;
   struct CalendarObjectRecord;

   struct ImipAttendee
   {
      AnsiString address;    // lower-cased, without mailto:
      AnsiString name;       // the CN parameter, or ""
      AnsiString partstat;   // NEEDS-ACTION, ACCEPTED, TENTATIVE, DECLINED, ...
      AnsiString role;       // REQ-PARTICIPANT, OPT-PARTICIPANT, CHAIR, ...
      AnsiString sentBy;     // the SENT-BY parameter's address, lower-cased, or ""
   };

   // Who sent a delivered message, as the server itself recorded it when the
   // message arrived, and never from anything written in its calendar part:
   // what Imip::IsSentBy reads before a delivered REPLY, CANCEL or updated
   // REQUEST changes a stored event.
   struct ImipOrigin
   {
      ImipOrigin() : submitterAccountId(0) { }

      // The account a signed-in session of this server submitted the message
      // as - SMTP AUTH, the webmail's own sends, or a room or a piece of
      // equipment answering by itself - or 0 (Message::GetSubmitterAccountID);
      // 0 for a message an application sent through the send API, which
      // records the account its key sent as and was written by no session of
      // it (Message::FlagSubmittedThroughSendApi).
      __int64 submitterAccountId;
      // What the inbound checks vouched for in mail from outside,
      // "dmarc=<the From domain>", lower-cased, or "" (Message::GetAuthDomains).
      AnsiString authDomains;
      // The address of the delivered copy's From header, lower-cased, when the
      // header is one field naming one mailbox - the From DMARC was checked
      // against - and "" otherwise (Imip::FromAddressOf).
      AnsiString fromAddress;
   };

   // Who stands behind an invitation Imip::KeepInvitation keeps, as its
   // caller knows it: what decides whether it may replace a copy of the UID
   // already stored, and whether the copy it writes is marked as one whose
   // organiser nobody verified (Imip::FiledUnverified).
   enum class ImipProvenance
   {
      // The account answering a message it holds (the page's answer): the
      // copy is replaced by the rules of SEQUENCE and organiser, and what it
      // writes from the message is marked when it is a new event, a later
      // version than the copy the server had, or written over a marked copy.
      AccountsAnswer,
      // Delivered, and the server verified that the organiser sent it
      // (Imip::IsSentBy), or a room's own booking of a request it believed:
      // the copy is replaced by those rules, and a marked copy - which nobody
      // verified - holds off no such invitation by its SEQUENCE.
      FromOrganiser,
      // Delivered from a sender the server could not verify as the organiser:
      // a new event, marked, and never a copy already stored replaced.
      Unverified
   };

   // What KeepInvitation does with an invitation, decided from values alone
   // (Imip::KeepRule).
   struct ImipKeepRule
   {
      ImipKeepRule() : refused(false), useInvitation(true), mark(false) { }

      bool refused;         // nothing is written: a stored copy, and an invitation nobody verified
      bool useInvitation;   // the invitation's copy is written; else the stored copy stays, with only the answer set
      bool mark;            // what is written from the invitation is marked as filed unverified
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
      // A REQUEST that names the account as an ATTENDEE - by its address, an
      // alias domain's spelling of it, or an account alias that points at it -
      // and does not name it as the organiser is kept in the account's default
      // calendar as it arrived - the account's PARTSTAT what the organiser
      // wrote, NEEDS-ACTION as a rule - replacing an older copy of the UID and
      // never a newer one, and never one another organiser holds; an account
      // with no calendar row yet gets nothing filed, since a delivery is not a
      // use, and a REQUEST that does not name the account is not the account's
      // to keep. A message flagged as spam files nothing at all, whatever it
      // carries: a calendar part in spam is calendar spam, and a filed one
      // would be drawn on the page and counted as busy. Anything else is left
      // for the page. Never throws; says what it did in the log.
      //
      // Whatever changes an event already stored is applied only when the
      // server verified who sent the message (IsSentBy, from original's record
      // and the delivered copy's From), since a UID is mailed to every guest
      // and the calendar part says whatever its writer put there: a CANCEL
      // only from the organiser the stored event names, a REQUEST replaces a
      // stored copy only from that organiser, and a REPLY sets only the
      // PARTSTAT of an attendee the event already lists who sent it - or whose
      // delegate (a delegate share on one of that attendee's calendars) sent
      // it, named by SENT-BY, which is then kept beside the answer; an answer
      // from an address of the same mailbox as the one listed (an alias the
      // invitation was sent to) is that attendee's, and one from a member of
      // a distribution list of this server's that the event lists is added
      // beside the list (ApplyToStored). A new invitation is filed from
      // anybody, marked when its organiser was not verified, and such a copy
      // never holds off the verified organiser's own (KeepRule). Anything
      // else stays an ordinary message in the mailbox, for the page, and
      // changes no stored event.
      //
      // An account that is a room or a piece of equipment (Account::IsResource)
      // takes the REQUEST and the CANCEL another way, and nothing else: they
      // are handed to ResourceBooking::OnDelivery, which decides them after
      // the delivery by the booking policy and answers the organiser. original
      // is the queue message the delivery was made from, whose record of where
      // it came from (Message::GetSubmitterAccountID) that decision reads.
      static void FileOnDelivery(std::shared_ptr<const Account> account, const String &fileName, bool spam, std::shared_ptr<const Message> original);

      // The attendee of the invitation that is the account - by its address,
      // an alias domain's spelling of it, or an account alias (hm_aliases,
      // one step) that points at it - as the invitation writes it. me is the
      // account's address, lower-cased. False when none is.
      static bool AttendeeFor(const ImipMessage &message, const AnsiString &me, ImipAttendee &attendee);

      // The invitation's event kept in the account's calendars: the copy that
      // holds the UID replaced when the invitation is the same or a later
      // sequence, an older invitation not undoing a newer, and a copy whose
      // ORGANIZER is not the invitation's never replaced at all - a UID is
      // mailed to every guest, so anybody who saw an invitation could
      // otherwise rewrite the event under it; a new object in the default
      // calendar when nothing holds the UID - made when createDefault, else
      // refused when there is none. At the same sequence the answer the
      // address already gave (a PARTSTAT that is not NEEDS-ACTION, with its
      // SENT-BY) is carried into the replacement, since a resend does not
      // undo an answer. cancelled marks every component STATUS:CANCELLED; a
      // partstat, when given, is set for the address (with the name) before
      // the object is written. The page's answer and the delivery filer are
      // this one path, so a REQUEST filed on delivery and then answered from
      // the message is one event, never two. What was written comes back in
      // calendar and record. What may be replaced, and what is marked, is
      // the provenance's (KeepRule): Unverified refuses (false, with the
      // problem) whenever a copy of the UID is already stored, and so only
      // ever adds a new event - what the delivery filer passes for an
      // invitation whose organiser the server could not verify sent it.
      static bool KeepInvitation(__int64 accountId, const ImipMessage &invitation, bool createDefault, bool cancelled,
                                 const AnsiString &address, const AnsiString &name, const AnsiString &partstat,
                                 CalendarRecord &calendar, CalendarObjectRecord &record, AnsiString &problem,
                                 ImipProvenance provenance = ImipProvenance::AccountsAnswer);

      // KeepInvitation's rule, a function of values: inCalendar whether a
      // copy of the UID is stored, storedSequence its master's SEQUENCE (-1
      // when the stored copy does not read), storedUnverified whether it is
      // marked as filed unverified.
      //   - Unverified: refused when a copy is stored; else written, marked.
      //   - A stored copy at a later SEQUENCE than the invitation stays (an
      //     older invitation does not undo a newer) - unless it is marked and
      //     the organiser was verified (FromOrganiser): a copy nobody verified,
      //     whatever SEQUENCE its writer gave it, does not hold off the
      //     organiser's own invitation.
      //   - FromOrganiser writes no mark; AccountsAnswer marks what it writes
      //     from the invitation when nothing was stored, the stored copy was
      //     marked or did not read, or the invitation is a later SEQUENCE -
      //     a version the server did not verify the organiser sent.
      static ImipKeepRule KeepRule(ImipProvenance provenance, bool inCalendar, int storedSequence, bool storedUnverified, int invitationSequence);

      // Whether a stored copy is marked as filed from an invitation whose
      // organiser the server did not verify: X-HMAILSERVER-IMIP-UNVERIFIED on
      // one of its VEVENTs or VTODOs. The mark is the server's own: whatever
      // an invitation carries of it is taken out before anything is written.
      static bool FiledUnverified(const ICalComponent &tree);

      // What a delivered REPLY or CANCEL changes in one stored copy of the
      // event it names - a function of values, so that the rule can be held
      // to its cases without a delivery or a database; FileOnDelivery passes
      // the verification (IsSentBy, and a delegate share checked in the
      // store). me is the account's address.
      //   - A REPLY, to an event the account organises (or that names no
      //     organiser): the PARTSTAT of each attendee it names that the event
      //     already lists (ListedAddressesFor: under that address, or under
      //     another address of the same mailbox, as sameMailbox says - an
      //     alias the invitation was sent to), when sentBy vouches for the
      //     listed address, or when the attendee's SENT-BY names a delegate
      //     delegateSent vouches for, the SENT-BY then kept beside the answer.
      //     Set only on the components that list the address: nobody is
      //     added, to the event or to one occurrence of it - but for one
      //     case: an attendee the event does not list, at any address of
      //     their mailbox, who sent the answer themselves, or through a
      //     delegate as above, and is a member of a distribution list the
      //     event lists (listMember: the server's own list, and its own record
      //     of who is in it), is added beside the list on the components that
      //     list it and not them, with MEMBER naming the list (RFC 5545
      //     section 3.2.11) - the organiser invited them through it.
      //   - A CANCEL: every component STATUS:CANCELLED, when the stored
      //     event's ORGANIZER is the CANCEL's and sentBy vouches for it.
      // refused counts the answers, or the cancellation, left as they were.
      // True when the copy changed.
      static bool ApplyToStored(ICalComponent &tree, const ImipMessage &message, const AnsiString &me,
                                const std::function<bool(const AnsiString &address)> &sentBy,
                                const std::function<bool(const AnsiString &attendee, const AnsiString &delegate)> &delegateSent,
                                const std::function<bool(const AnsiString &listed, const AnsiString &named)> &sameMailbox,
                                const std::function<bool(const AnsiString &list, const AnsiString &named)> &listMember,
                                size_t &refused);

      // Whether the address is a member of the distribution list of this
      // server that list is, by the list's own record of its members
      // (DistributionListMembership::IsMember: a confirmed row, or an account
      // its membership query answers) - as the address is spelt, or as the
      // account it is (through an alias or an alias domain) is. Not through a
      // list that is a member of it, and not through a row that is an alias:
      // each would be a lookup for every member, on the delivery thread, for
      // every answer. False when list is no active list of this server's.
      static bool IsListMember(const AnsiString &list, const AnsiString &address);

      // The addresses a VEVENT or VTODO of the object lists as ATTENDEE that
      // an answer from the address is for: the address itself when it is
      // listed; otherwise every listed address that sameMailbox says is the
      // same mailbox as it; none when neither. Lower-cased, each once.
      static std::vector<AnsiString> ListedAddressesFor(const ICalComponent &tree, const AnsiString &address,
                                                        const std::function<bool(const AnsiString &listed, const AnsiString &named)> &sameMailbox);

      // Whether two addresses are one mailbox of this server: each the
      // address of the same account, directly or through an alias domain and
      // a few hops of account aliases. False when either is no account's.
      static bool SameMailbox(const AnsiString &one, const AnsiString &other);

      // Where the delivered message came from: the record the queue message
      // it was delivered from carries (original; an empty origin when there
      // is none), and the delivered copy's From.
      static ImipOrigin OriginOf(std::shared_ptr<const Message> original, const String &fileName);

      // Whether the message was sent by the address, by what the server
      // verified and nothing a sender wrote:
      //   - submitted by a signed-in session of this server: when that
      //     account may send as the address, by the send-as rules
      //     authenticated SMTP obeys (SMTPConnection::SenderPermittedFor - its
      //     own address, an alias of it, the post right on another account's
      //     INBOX, a verified or masked address);
      //   - from outside: when the address is not at one of this server's
      //     domains - those are believed from a signed-in session alone - and
      //     VouchedFromOutside holds.
      // An empty address is nobody's.
      static bool IsSentBy(const ImipOrigin &origin, const AnsiString &address);

      // The half of IsSentBy for mail from outside, a function of values: the
      // From is the address, and DMARC passed for exactly the address's domain
      // (a token "dmarc=<domain>" of authDomains, separated by spaces or
      // commas). DMARC vouches for the From's domain, through an SPF or DKIM
      // pass aligned with it; the From itself is the address that domain let
      // be written. SPF on its own vouches for nobody's From.
      static bool VouchedFromOutside(const AnsiString &authDomains, const AnsiString &fromAddress, const AnsiString &address);

      // The address a message header's From names, lower-cased, when the
      // header has one From field and it names one mailbox that the address
      // list parser and the DMARC check read alike, written as one mailbox is
      // written - a bare address, or a display name and one <address> that
      // ends the field, its quoted strings and comments aside; "" otherwise -
      // no From, two, a list of several, one the two readers disagree on, or
      // one with more than that one mailbox's syntax in it (a second
      // <address>, text after the >), which another program's reader could
      // take for a different address.
      static AnsiString FromAddressOf(const AnsiString &header);

      // "mailto:Some.One@Example.com" to "some.one@example.com".
      static AnsiString AddressOf(const AnsiString &calAddress);

      // The object with its METHOD replaced (or added), serialised.
      static AnsiString WithMethod(const ICalComponent &calendar, const AnsiString &method);

      // Every VEVENT/VTODO of the object: the attendee's PARTSTAT set, or the
      // attendee added with it when the object does not list them yet - unless
      // addWhenAbsent is false, when a component that does not list them is
      // left as it is. sentBy is the address of a delegate who answered for
      // the attendee, written as SENT-BY="mailto:..." on the ATTENDEE (RFC
      // 5545 section 3.2.18); empty when the attendee answered, and any
      // SENT-BY there was goes.
      static void SetPartstat(ICalComponent &calendar, const AnsiString &address, const AnsiString &name, const AnsiString &partstat, const AnsiString &sentBy = AnsiString(),
                              bool addWhenAbsent = true);

      // The REPLY object for one attendee (RFC 5546 section 3.2.3): the
      // masters' identity, the organiser, and that one attendee with their
      // answer - nothing another attendee said. sentBy, when given, is the
      // delegate who answered, on the ATTENDEE as SENT-BY. cutype, when given,
      // is the ATTENDEE's CUTYPE (RFC 5545 section 3.2.3): ROOM or RESOURCE
      // for the answer a room or a piece of equipment gives.
      static AnsiString ReplyObject(const ImipMessage &invitation, const AnsiString &address, const AnsiString &name, const AnsiString &partstat, const AnsiString &sentBy = AnsiString(),
                                    const AnsiString &cutype = AnsiString());

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

   // Who may change a stored event by a delivered message: FromAddressOf's
   // reading of a header and VouchedFromOutside's DMARC token, which decide
   // whether mail from outside was sent by an address, ApplyToStored's rule
   // for a REPLY and a CANCEL given who sent it (an alias the event lists,
   // an occurrence a guest was left out of, a member of a list the event
   // lists), KeepRule's for an invitation by who stands behind it, and
   // OriginOf, which never takes a message sent through the send API for an
   // account's session - each with the cases that must not change or vouch
   // for anything. Functions of
   // values only - no database, configuration or network; ClassTester runs it
   // on Windows and imip-tests on Linux. Throws std::logic_error on the first
   // failure.
   class ImipTester
   {
   public:
      void Test();
   };
}
