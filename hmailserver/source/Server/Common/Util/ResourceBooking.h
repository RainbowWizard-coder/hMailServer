// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Rooms and equipment: Exchange's resource mailboxes, the Calendar
// Attendant's part of them (docs/RoomsAndEquipment.md).
//
// A resource is an account whose kind (Account::GetKind, schema 6101) is a
// room or a piece of equipment. It is an account in every other way - it has
// an address, aliases, a quota, a calendar, delegates and a place in the
// directory - and it cannot be signed into: every password logon refuses it
// in PasswordValidator, SCRAM where IMAP, POP3 and SMTP look the account up,
// XCLIENT's LOGIN, and every logon that proves identity without a password in
// AccountLogon::RefusedByHold. It is administered, and the people who answer
// for it are the delegates of its calendar, the same delegation a person's
// assistant has (CalendarStore's delegate share).
//
// Its booking policy is one row of hm_bookingpolicies, keyed by the account;
// an account with no row has the defaults. What is delivered to it - an iMIP
// REQUEST naming it as a guest, or a CANCEL - is handed here by the delivery
// path's iMIP hook (Imip::FileOnDelivery), written to hm_bookingqueue with the
// origin the queue message carried, and decided AFTER the delivery on a work
// queue of its own (WorkQueueName, DrainThreads threads): one drain at a time
// per resource, one delivery a task, so two requests for one resource are
// decided one after the other and never together, the resources take turns,
// and no delivery thread or SMTP finalisation waits on a decision. A delivery
// leaves the table only once decided, so a stop, a Reinitialize or a crash
// puts it off rather than losing it: WakeQueued, every minute, wakes every
// resource that still has one waiting. The message is in the resource's
// mailbox whatever is decided.
//
// Who is asking comes first, and it is decided from what the server itself
// recorded when the message arrived, never from anything a sender wrote
// (Message::GetSubmitterAccountID and GetAuthDomains, schema 6101):
//
//   - an organiser at this server's own domains is the organiser only when an
//     authenticated session - SMTP AUTH, the webmail's own sends - submitted
//     the message as that account, or as an account allowed to send as it
//     (SMTPConnection::SenderPermittedFor, the send-as rules); a message an
//     application sent through the send API is no account's session, whatever
//     address its key sent as (Message::FlagSubmittedThroughSendApi);
//   - an organiser from outside is the organiser only when the policy's
//     bookers name that address or its @domain AND the message passed DMARC
//     for that domain with the organiser as its From - one From naming one
//     mailbox, the organiser's (SPF alone vouches for nobody's From, and
//     DMARC for a domain vouches for none of its other senders); the CANCEL
//     of a booking the room holds from them needs that pass and not the list;
//   - anything else is not the organiser: its request is not filed, not
//     answered and changes no booking, and its CANCEL frees nothing. The
//     delegates are told, at most once an hour per organiser per resource
//     (and at most NoticesPerHour times an hour per resource).
//
// Then the policy, for a request whose organiser is who wrote it:
//
//   - who may book: everyone in the server's own domains when the policy
//     names nobody, else the addresses, @domains, distribution lists and
//     groups it names (alias domains resolved on both sides);
//   - recurring meetings allowed or not, and a series is checked occurrence
//     by occurrence up to its end, which must be inside the booking window;
//   - the booking window: how many days ahead a booking may start, counted
//     from the moment the request is decided;
//   - the longest booking, in minutes;
//   - working hours only, on the working days, in the policy's time zone;
//   - conflicts: none allowed unless the policy says so, judged by a check of
//     the resource's own (BookedPeriods): every booking its calendars hold
//     that is not cancelled is busy, whatever its TRANSP and whatever any
//     attendee's PARTSTAT says; one that cannot be read or expanded in full
//     leaves the request for the delegates rather than guess.
//
// An accepted booking is filed as ACCEPTED, and the organiser the resource
// believed is recorded beside it (hm_bookingdecisions, keyed by the UID): an
// answer a delegate gives later goes to that organiser and to nobody else,
// whatever the calendar object says by then (AnswerableOrganiser). A
// declined update of a booking the calendar held takes that booking out, so
// its old slot is free. A REQUEST that carries only a RECURRENCE-ID - one
// occurrence of a series changed - is decided as that occurrence alone and
// merged into the series
// the calendar holds, or taken out of it with an EXDATE when declined; a
// series is never replaced or deleted because of one occurrence. The
// organiser is sent an iMIP REPLY (PARTSTAT ACCEPTED or DECLINED,
// Imip::ReplyObject) from the resource's address through the server's own
// queue, with the reason in the text part and in a COMMENT; a declined
// conflict says when the resource is busy and never whose booking it is, and
// a decline quotes nothing of the request's own text. No answer goes to a
// message with an empty reverse path or an Auto-Submitted other than "no"
// (RFC 3834), and at most AnswersPerHour an hour to one organiser from one
// resource (hm_bookinganswers, like the out-of-office reply's memory). A
// CANCEL from the booking's organiser frees the slot; a REQUEST with a
// higher SEQUENCE is decided again.
//
// A request from the organiser is left for the delegates - when the policy
// answers nothing automatically, when it was sent automatically, when the
// organiser has had their hour's answers, and when no decision could be
// reached; a failure also writes one line to the error log (7430-7434). A
// request left is never put into the resource's calendar, which holds only
// what the resource decided: it is in the mailbox, and the delegates are told
// by mail within the organiser's hour of answers, then once an hour.
//
// The lock is this process's: two servers on one database decide for the
// same resource without seeing each other (the Roadmap2 row "Active-active
// nodes"). A delegate's write to the resource's calendar over REST or CalDAV
// takes the same lock (ResourceCalendarGuard), so it never lands in the
// middle of a decision.

#pragma once

#include <memory>
#include <mutex>
#include <vector>

namespace HM
{
   class Account;
   class Message;
   struct ImipMessage;
   struct ICalComponent;

   // One resource's booking policy, as hm_bookingpolicies holds it.
   struct BookingPolicy
   {
      BookingPolicy() : accountId(0), autoAccept(true), windowDays(180), maxMinutes(1440), workingHoursOnly(false),
         workDays(31), workStart(480), workEnd(1020), timeZone(_T("UTC")), allowConflicts(false), allowRecurring(true),
         capacity(0), stored(false) { }

      __int64 accountId;
      bool autoAccept;        // answer invitations by the policy; off leaves them for the delegates
      int windowDays;         // how many days ahead a booking may start, 1 to MaximumWindowDays
      int maxMinutes;         // the longest booking; 0 is no limit
      bool workingHoursOnly;
      int workDays;           // a bit per day: Monday 1, Tuesday 2 ... Sunday 64
      int workStart;          // minutes after midnight, in timeZone
      int workEnd;
      String timeZone;        // an IANA or Windows name ICalendar knows
      bool allowConflicts;
      bool allowRecurring;
      String bookers;         // who may book, comma-separated; empty is everyone in the organisation
      int capacity;           // how many people a room seats; 0 is not said
      bool stored;            // whether the row exists
   };

   // Where a message delivered to a resource came from, as the server recorded
   // it when the message arrived - never a header's word - and whether it asks
   // not to be answered automatically.
   struct BookingOrigin
   {
      BookingOrigin() : submitterAccountId(0), autoSubmitted(false) { }

      String envelopeSender;        // the return path; empty for the null sender
      __int64 submitterAccountId;   // Message::GetSubmitterAccountID; 0 when no authenticated session sent it
      AnsiString authDomains;       // Message::GetAuthDomains: "dmarc=example.com", or ""
      bool autoSubmitted;           // an Auto-Submitted header other than "no" (RFC 3834)
   };

   class ResourceBooking
   {
   public:
      // The bounds every surface holds the policy to.
      static const int MaximumWindowDays = 1080;
      static const int MaximumDurationMinutes = 527040;
      static const int MaximumBookersLength = 2000;
      static const int MaximumCapacity = 100000;
      static const int MaximumTimeZoneLength = 64;

      // The answer memory (hm_bookinganswers): at most this many answers - and
      // notices of requests left for the delegates - an hour from one resource
      // to one organiser; past it, and for a request not honoured, the
      // delegates are told at most once an hour per organiser, and at most
      // NoticesPerHour times an hour per resource altogether.
      static const int AnswersPerHour = 20;
      static const int NoticesPerHour = 10;

      // Every notice one resource sends its delegates in an hour, whoever the
      // organiser: what bounds organisers rotated to get round their own
      // budgets. A flood can hold a genuine notice back for the rest of the
      // hour; the request is in the mailbox all the same.
      static const int AllNoticesPerHour = 60;

      // How many times a queued delivery is tried - each try counted before it
      // is made, so that one that takes the service down counts - before it is
      // taken off the queue, with an error-log line and a notice to the
      // delegates, rather than tried again after every restart.
      static const int MaximumAttempts = 3;

      // The work queue the decisions run on, created at start beside the
      // maintenance queue (Application::InitInstance) and removed at exit.
      static const int DrainThreads = 2;
      static String WorkQueueName();

      // Every resource with a delivery waiting in hm_bookingqueue woken: what a
      // stop, a Reinitialize or a crash left undecided is decided now. Run
      // every minute (ScheduledMailTask).
      static void WakeQueued();

      // Whether the object's master says the resource declined it: the ATTENDEE
      // that is the resource - its address or an alias domain's spelling of
      // it - with PARTSTAT=DECLINED. A delegate who declines a booking over
      // CalDAV frees it with this (CalDavServer), as the REST reply does.
      static bool DeclinedBy(const ICalComponent &calendar, const Account &resource);

      // The address an answer about the booking under uid may go to, From the
      // resource: the organiser the resource believed and recorded when it
      // accepted the booking (hm_bookingdecisions), and only while the booking
      // still names them - organizer is the ORGANIZER the stored object has,
      // as written. "" when the resource never accepted a booking under that
      // UID (one a delegate or a share wrote in by hand), when the ORGANIZER
      // has been changed since, or when the record could not be read: nothing
      // is sent then, since anybody who may write to the resource's calendar
      // may write any ORGANIZER into it, and nothing taken from a calendar
      // object decides where mail From the resource goes.
      static AnsiString AnswerableOrganiser(const Account &resource, const String &uid, const AnsiString &organizer);

      // The same, for a booking as its calendar object holds it: its UID and
      // its master's ORGANIZER.
      static AnsiString AnswerableOrganiser(const Account &resource, const ICalComponent &calendar);

      // The record of the booking under uid forgotten, once the booking has
      // left the resource's calendar - whatever the account's kind is now,
      // since one that was a room keeps its rows while it is a person - and
      // kept while another calendar of the account still holds the UID (a
      // CalDAV move is a PUT into one calendar and a DELETE from the other).
      // Never throws.
      static void ForgetDecision(__int64 accountId, const String &uid);

      // Every record of the account whose booking no calendar of the account
      // holds any more forgotten: what a whole calendar deleted leaves. Never
      // throws.
      static void ForgetGoneBookings(__int64 accountId);

      // The organiser of a booking the resource held told that a delegate
      // declined it on the resource's behalf: the REPLY the REST route sends,
      // From the resource, Sender the delegate, SENT-BY on its ATTENDEE, with
      // the delegate's envelope, queued through the server's own queue.
      // calendar is the booking as the resource held it before the decline,
      // and organiser what AnswerableOrganiser said of it - read by the caller
      // under the resource's lock, before the booking and its record went -
      // so a booking the resource did not accept, or whose ORGANIZER was
      // changed since, tells nobody. The caller has made sure the delegate is
      // one (a share with delegate set): a plain write share does not answer
      // for a resource. Called with the resource's lock released. What a
      // CalDAV decline calls, since a CalDAV client schedules nothing here.
      // False when nobody was told.
      static bool TellOrganiserOfDecline(std::shared_ptr<const Account> resource, std::shared_ptr<const Account> delegate, const ICalComponent &calendar,
                                         const AnsiString &organiser);

      // "person", "room" or "equipment", and back. ReadKind takes the word or
      // the digit, case-insensitively.
      static AnsiString KindWord(int kind);
      static bool ReadKind(const String &word, int &kind);

      // The policy of one account: its row, or the defaults when it has none
      // (stored false). False when the table could not be read.
      static bool GetPolicy(__int64 accountId, BookingPolicy &policy);

      // Writes the row, making one when there is none.
      static bool SavePolicy(const BookingPolicy &policy);

      // Whether a policy is one this server keeps, with the reason when it is
      // not; the bookers list is put in its stored form - entries trimmed,
      // addresses and domains lower-cased, a group by its name as the server
      // has it, duplicates dropped. A group is checked only when it is new to
      // the list (not in previousBookers, the list as stored), so a group
      // deleted since it was named does not refuse every later change; it
      // simply matches nobody until it is taken out. A caller that may not
      // name groups (a credential confined to named domains) is refused a new
      // one before it is looked up, and outside says so.
      static bool CheckPolicy(BookingPolicy &policy, const String &previousBookers, bool mayNameGroups, String &problem, bool &outside);

      // A bookers or delegates list as its entries, and back.
      static std::vector<String> SplitList(const String &list);
      static String JoinList(const std::vector<String> &entries);

      // The addresses of the resource's delegates: the accounts its default
      // calendar is shared with as a delegate, and the members of the groups
      // it is shared with as one. Empty when there is no calendar yet.
      static bool DelegatesOf(__int64 accountId, std::vector<String> &addresses);

      // The delegates as they were named: an account's address for a share
      // with an account, a group's name for a share with a group - what
      // ResolveDelegates takes back.
      static bool DelegateEntries(__int64 accountId, std::vector<String> &entries);

      // The delegates named, resolved and checked with nothing written:
      // addresses of this server's accounts (alias domains resolved) and names
      // of its groups. allowedDomains is the caller's confinement (empty for
      // none): an account outside it is refused before it is looked up, so
      // the refusal says nothing about whether it exists, and a group - whose
      // members may be anywhere - is refused to a confined caller altogether.
      // False with the reason; outside says the refusal was the confinement's.
      static bool ResolveDelegates(std::shared_ptr<const Account> resource, const std::vector<String> &entries, const std::vector<String> &allowedDomains,
                                   std::vector<__int64> &accountIds, std::vector<__int64> &groupIds, String &problem, bool &outside);

      // The delegates made to be exactly these: each gets a delegate share of
      // the resource's default calendar (made if it has none), a delegate
      // share not named goes, and a plain share (read or write, not a
      // delegate) is left as it is. False with the reason when a share could
      // not be written.
      static bool ApplyDelegates(std::shared_ptr<const Account> resource, const std::vector<__int64> &accountIds, const std::vector<__int64> &groupIds, String &problem);

      // The delivery path (Imip::FileOnDelivery): a REQUEST or a CANCEL read
      // from the message just delivered to the resource, the delivered file
      // (whose Auto-Submitted is read) and the queue message in memory it was
      // delivered from (whose origin is read - a mailbox row never carries
      // it). Written to hm_bookingqueue and decided after the delivery; never
      // throws and never waits for the decision.
      static void OnDelivery(std::shared_ptr<const Account> resource, const ImipMessage &message, const String &fileName,
                             std::shared_ptr<const Message> original);

      // An address erased (AddressTraceEraser): taken out of every policy's
      // bookers list, and every resource's memory of it - what it answered,
      // what it sent that waits, and the bookings it recorded it accepted from
      // it - forgotten. The number
      // of rows changed, or -1 when they could not be read or written.
      static int EraseAddress(const String &address);
   };

   // Held while a REST or CalDAV write changes a calendar: when the calendar's
   // owner is a room or a piece of equipment, the resource's own lock - the one
   // its bookings are decided under - so that a delegate's accept, decline or
   // edit never lands in the middle of a decision. Nothing for anybody else.
   class ResourceCalendarGuard
   {
   public:
      explicit ResourceCalendarGuard(__int64 ownerAccountId);
      ~ResourceCalendarGuard();

   private:
      ResourceCalendarGuard(const ResourceCalendarGuard &);
      ResourceCalendarGuard &operator=(const ResourceCalendarGuard &);

      std::shared_ptr<std::recursive_mutex> mutex_;
   };

   // Run from ClassTester::DoTests, which the regression suite reaches through
   // Utilities.RunTestSuite: what the message's record of a DMARC pass may
   // hold - never more than its column, whatever the domain - and what vouches
   // for an organiser from outside.
   class ResourceBookingTester
   {
   public:
      void Test();
   };
}
