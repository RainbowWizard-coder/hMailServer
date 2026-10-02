# Rooms and equipment

A meeting room, or a projector, has a mailbox the way it has in Exchange: it is
invited to a meeting like any guest, and it answers by itself - it accepts an
invitation for a time it is free, inside its booking window, from somebody who
may book it, and declines one that is not, saying why. Nobody reads its mail to
decide. This page is what the server does, what an administrator sets, and what
it deliberately does not do.

## A room is an account

A room or a piece of equipment is an **account** whose kind says so: `person`
(every account there was, and the default), `room` or `equipment`. It is a
column of `hm_accounts` (`accountkind`, schema 6101), not a table of mailboxes of
its own, so everything that works on an account works on a room: its address and
its aliases (an alias domain's spelling of it too), its quota, its calendar, the
calendar's sharing and delegation, its place in the organisation's directory, the
backup, a mailbox move.

Where it is set:

* **REST** - `kind` on `GET` and `PUT /api/v1/accounts/{address}`, the word or
  its digit (`person` 0, `room` 1, `equipment` 2).
* **The Control Deck** - the account editor's **General** group has a select of
  the three, and a room or a piece of equipment gets a **Booking policy** card.
* **COM** - `Account.Kind`, the digit, saved by `Save()`. The booking policy is
  on REST and the Deck only: it did not follow cleanly as COM properties, and a
  policy object of its own would be a new COM interface that every installation
  must register again, so it was not built.
* **The desktop Control Panel** - the account dialog's **Identity** tab has
  *Kind of account* (a person, a room, a piece of equipment), over
  `Account.Kind`. It has no booking policy: that is set in the Control Deck.

### It cannot be signed into

A room is administered, not signed into, as Exchange's resource mailbox is a
disabled user. From the moment its kind is saved **every** sign-in as it is
refused, whatever the password: IMAP, POP3, SMTP `AUTH`, ManageSieve, CalDAV and
CardDAV, the webmail and REST sign-in (in `PasswordValidator`, before any script
hook or hash, and for SCRAM in IMAP, POP3 and SMTP where the account it asks for
is looked up), and the sign-ins that prove identity without a password - an
OAuth2 bearer token, a Kerberos ticket, a client certificate, a passkey, single
sign-on - at `AccountLogon::RefusedByHold`, where they all ask. Nor can anybody
sign in as it by way of their own password (SASL PLAIN's authorisation name),
and a trusted upstream's `XCLIENT LOGIN=` naming it is refused (550 5.7.8)
before anything of the session changes. The refusal is the ordinary
wrong-password answer. A webmail or REST session that was open when the account
became a room is refused at its next request and ended; an IMAP or POP3
connection already signed in is left to end by itself, as it is when an account
is deactivated. Setting the kind back to `person` lets it sign in again with the
password it has; a room keeps a password only because every account has one,
and nobody needs to know it.

### Who answers for it: its delegates

What a room does not answer by itself goes to its **delegates**, who look after
its calendar with the delegation a person's assistant already has
([Calendar.md](Calendar.md), "A delegate"): a delegate sees the room's calendar in
their own Calendars box and in their CalDAV home, and can write to it. A booking
in it they answer with Accept, Maybe or Decline "on behalf of" the room - the
organiser receives the `METHOD:REPLY` `From:` the room with `Sender:` the
delegate and `SENT-BY` on the room's `ATTENDEE`, sent with the delegate's own
envelope, so a bounce goes back to them. A booking a delegate **declines**
leaves the room's calendar - from the webmail or over REST, and from a CalDAV
client that writes the room's `PARTSTAT=DECLINED` back - as one the room
declines itself does, since everything the room's calendar holds that is not
cancelled is busy; and the organiser the room believed is told either way
(below), the CalDAV decline answered by the server from the booking as the
room held it, since a CalDAV client schedules nothing here. (A decline of one occurrence from a CalDAV client is stored as
written, and that occurrence stays busy: decline it from the webmail, or delete
it.)

**Whom an answer goes to.** The room records, when it accepts a booking, the
organiser it believed (`hm_bookingdecisions`, by the booking's UID, compared
exactly on every database), and an
answer about that booking - a delegate's Accept, Maybe or Decline, over REST or
from a CalDAV client - goes to that organiser and to nobody else, and only
while the booking still names them as its `ORGANIZER`. Anybody who may write
to the room's calendar can write any `ORGANIZER` into a booking there, and mail
`From:` the room is never addressed by what a calendar object says. A booking
the room did not accept - one a delegate wrote in by hand - or one whose
`ORGANIZER` was changed since is answered to nobody: the answer is stored (a
decline still takes the booking out), the REST reply says `"replied":false`
with an empty `to`, and the delegate tells the organiser themselves. The record
goes with the booking - declined, cancelled whole, deleted, written back under
another UID, moved to another account's calendar, or gone with a calendar
deleted - and stays while any calendar of the room still holds the UID, so a
booking moved between two of its calendars keeps it, a CalDAV client's move (a
`PUT` into one, then a `DELETE` from the other) included. A pass that forgets
these records - one booking's when it goes, or every record whose calendar is
gone when a calendar is deleted - and is stopped by an unexpected error writes an
HM7434 line to the error log: the record is forgotten when a calendar of the
account is next deleted, or with the account.

**Only a delegate answers for it.** A plain write share of the room's calendar
lets its holder change and delete what is there, and a decline written from a
CalDAV client by one takes the booking out as a `DELETE` would - but tells
nobody: the room's answer is its delegates' to give, as the REST reply (403 to
anybody else) says. A request the room left for them is not in its calendar
(below); they are told of it by mail. The delegates are named on the
booking policy (`delegates`, accounts' addresses and group names): each gets a
delegate share of the room's calendar, and one no longer named loses it. A plain
read or write share of the room's calendar is left alone. A credential confined
to named domains names accounts of those domains only, and no groups - a
group's members may be anywhere - and is refused (403) before an address outside
its domains is looked up.

## The booking policy

One row of `hm_bookingpolicies` per room (schema 6101), made on the first save;
a room with none answers by the defaults. `GET` and `PUT
/api/v1/accounts/{address}/booking-policy`, and the Deck's card:

| Field | Default | What it decides |
|---|---|---|
| `auto_accept` | `true` | Answer invitations by this policy. `false` leaves every one for the delegates (below). |
| `window_days` | 180 | How many days ahead a booking may start, 1 to 1080, counted from the moment the request is decided - not from the start of the day. |
| `max_duration_minutes` | 1440 | The longest booking; 0 is no limit. |
| `working_hours_only` | `false` | Only inside the working hours, on the working days. |
| `work_days` | mon to fri | The working days. |
| `work_start`, `work_end` | 08:00, 17:00 | The working hours, `HH:MM`, the end at most 24:00. |
| `time_zone` | UTC | The zone the working hours are in: any zone the calendar knows (`Europe/London`, `America/New_York`). |
| `allow_conflicts` | `false` | Accept a booking over another one. |
| `allow_recurring` | `true` | Accept a recurring meeting. |
| `bookers` | empty | Who may book: empty is everyone in this server's own domains; a list is its entries alone - an address, `@domain` for anyone there, a distribution list's address for its members, a group's name for its members. An alias domain's spelling matches its domain, on either side. An organiser from outside must be named here by address or `@domain`, and is believed only as below. |
| `capacity` | 0 | How many people a room seats, 0 for not said: shown in the directory, and decides nothing. |
| `delegates` | none | Who answers for it (above). Not a column: the calendar's delegate shares. |

A `PUT` changes the fields it names and refuses an unknown one by name; the
addresses and domains among the bookers are kept lower-cased, a group by its name
as the server has it (named in any case), duplicates dropped. A group is checked
when it is added to the list; one deleted since it was named stays until it is
taken out, matching nobody, and does not refuse every later change. A person's
policy is read (the defaults) and never written: the `PUT` answers 409 until the
account is a room or a piece of equipment. The policy is saved first and the
delegates after it, every delegate resolved before either: a policy that cannot
be saved has changed nothing, and delegates that could not all be written after
it are said to be so in the 500. The route needs `manage_accounts` to write and
`view_accounts` to read, and a credential confined to named domains is scoped by
the address in the path. The row is on the audit trail and in the backup.

## Who is asking

Before anything else the room decides whether the invitation's `ORGANIZER` is who
sent it - from what the server itself recorded when the message arrived, and
never from anything a sender writes. A `From:` header, or a `MAIL FROM`, is
whatever the sender puts there, and the server does not by default hold an
unauthenticated sender to its own address. So schema 6101 records, on the queued
message and not in a header, where it came from (`hm_messages.messagesubmitterid`
and `messageauthdomains`):

* **An organiser at this server's own domains** is the organiser only when the
  message was submitted by a signed-in session - SMTP `AUTH` (or an upstream's
  `XCLIENT LOGIN`), or the webmail's own sends - as that account, or as an
  account allowed to send as it by the send-as rules that authenticated SMTP
  already obeys (`SMTPConnection::SenderPermittedFor`: an alias of it, the post
  right on its INBOX, a verified or masked address). A message submitted through
  an API key is no account's session and is not believed, even when the key may
  send as the organiser's address (`Message::FlagSubmittedThroughSendApi`: the
  send API records that account for the domain rules, and 6.3.4 and 6.3.5 took
  the record as the organiser's session).
* **An organiser from outside** is the organiser only when the policy's
  `bookers` name that address or its `@domain`, **and** the message passed DMARC
  for that domain - which DMARC grants to an SPF or DKIM pass aligned with the
  From - with the organiser as its From: one `From:` field naming one mailbox,
  the organiser's, and written as one, so that no other mail program could read
  another address in it ([Calendar.md](Calendar.md), "Who may change a stored
  event"). DMARC vouches for a domain, and the From is the address that domain
  let be written, so another sender at the organiser's domain is not the
  organiser. SPF on its own is not enough, and is not recorded: it vouches for the
  connecting address for an envelope domain whatever the From says, and a shared
  platform named in a partner's SPF record carries other tenants' mail. The
  result is the inbound check's own (`AuthenticationResults::PassedDomains`),
  recorded whether or not the `Authentication-Results` header is switched on,
  and only when it fits its column (a From domain of 250 characters or more is
  never recorded, and never fails the message's save); with DMARC off, or for
  mail from an address range spam protection skips, no outside organiser is ever
  believed. The `CANCEL` of a booking the room already holds from such an
  organiser needs the DMARC pass but not the list, so that taking a partner off
  the list does not leave their bookings where they cannot cancel them.
* **Anything else is not the organiser.** Its request is not filed into the
  room's calendar and not answered, its update changes no booking and its
  `CANCEL` frees nothing. The message stays in the room's mailbox, and the
  delegates are told - at most once an hour for each organiser at each room, and
  at most ten times an hour for a room altogether, so that a sender who writes a
  new organiser into every message cannot flood them. These are counted apart
  from the named organiser's own, so a forged request cannot use up a real
  organiser's hour. The notice repeats nothing of the request's own text: it says
  which organiser it named (when that is an address), for when, and why it was
  not believed. A room tells its delegates at most 60 times an hour in all,
  whoever the organisers, which also bounds a believed sender who rotates
  organiser addresses (aliases, send-as identities, a partner's many addresses)
  to get round the budget each address has. The ceilings are what keeps a flood
  from the delegates, and during one they can hold a genuine notice back for the
  rest of the hour: the request is in the room's mailbox all the same. A notice
  one ceiling refuses spends nothing of the other, nor of the organiser's hour.

## The automatic answer

When an invitation - an iMIP `METHOD:REQUEST` ([Calendar.md](Calendar.md),
"Invitations by mail") that names the room as a guest - is delivered to a room,
the delivery writes it to the room's queue (`hm_bookingqueue`, with where it came
from) and is done: the decision is made **just after the delivery**, on a work
queue of the rooms' own (two threads), one delivery at a time for each room and
the rooms taking turns. No delivery thread and no SMTP acknowledgement waits for
it, and the message is in the room's mailbox whatever is decided. A delivery
leaves the queue only once it has been decided, so a stop, a Reinitialize or a
crash puts a decision off rather than losing it: every minute the server wakes
each room that still has one waiting (and one a stop interrupted half way is
decided again, whole - its answer may then go twice). Each try is counted before
it is made, so that a delivery whose decision takes the service down counts too:
after three tries it is taken off the queue with an HM7434 line, and the
delegates are told, once an hour, that a request could not be decided and is in
the room's mailbox - it neither holds the room's queue for ever nor runs again
after every restart. A try whose queued row could not even be read is given
back, so passing read errors give up nothing; and a row the database will not
let go of stays at the head of the room's queue for the next minute's sweep,
rather than being tried again at once. The checks, in order:

1. **Who is asking** (above).
2. **May they book it:** the `bookers` list, or the organisation. An organiser
   the room believes but the list does not name is **declined** ("You are not one
   of the people allowed to book Board Room."), never filed, and the delegates
   are told as above.
3. **Sent automatically?** A message with an empty reverse path, or an
   `Auto-Submitted` other than `no`, is never answered automatically (RFC 3834):
   it is left for the delegates.
4. **Answers automatically?** With `auto_accept` off, it is left for the
   delegates.
5. **The answer memory:** at most 20 answers an hour from one room to one
   organiser (`hm_bookinganswers`, like the out-of-office reply's memory of whom
   it answered), notices to the delegates of that organiser's requests counted
   with them. Past that, a request is left for the delegates.
6. **A recurring meeting** when the policy takes none.
7. **The window:** every occurrence must start within `window_days` of now; a
   series with no end never does. A series is checked occurrence by occurrence
   up to its end, which must therefore be inside the window, so the check is
   bounded by it; an occurrence already over is not judged.
8. **The longest booking**, for each occurrence.
9. **Working hours**, for each occurrence, in the policy's zone: on a working
   day, starting at or after the start and ending by the end; an all-day
   booking never is.
10. **Conflicts**, unless allowed - the room's own check, not free-busy's
    rules: every booking its calendars hold that is **not cancelled** is busy,
    whatever `TRANSP` its organiser sent ("show as free" is a guest's setting,
    not a room's) and whatever `PARTSTAT` any attendee has on it. The booking
    being decided again is left out of it. A booking the calendar holds that
    cannot be read, or has more occurrences in the time than can be expanded,
    leaves the request for the delegates rather than guess. The room's
    free-busy, as a colleague asks for it, is read by the same rule.

A booking that passes is filed into the room's calendar with the room's
`PARTSTAT` `ACCEPTED`. A declined one is not filed; a declined **update** of a
booking the room held takes that booking out of the calendar, so its old slot is
free again. The organiser is sent a `METHOD:REPLY` built by the same builder the
webmail's answers use: the invitation's `UID` and `SEQUENCE`, and one `ATTENDEE`,
the room by its own name (`CUTYPE=ROOM`, or `RESOURCE` for equipment), with
`PARTSTAT=ACCEPTED` or `DECLINED`. It is mail **from the room's address** -
`From:` the room, the envelope sender the room - queued through this server's own
queue like any mail the room sends (restricted delivery, routes and DKIM apply),
to the organiser the room believed, and marked `Auto-Submitted: auto-replied` so
the organiser's own out-of-office does not answer it. The server records it as
the room's own submission, as it records a delegate's answer as the delegate's,
so an organiser's calendar on this server files the answer into the event
([Calendar.md](Calendar.md), "Who may change a stored event").

**A decline quotes nothing of the request.** An acceptance may repeat what it
accepts ("Accepted: Planning @ ..."); a decline's subject is the room's name and
the time ("Declined: Board Room, Sep 29, 2026 10:30 - 11:30 UTC"), its text is
the reason alone, and its `REPLY` carries the `UID`, the `SEQUENCE`, the
`RECURRENCE-ID` when there is one, the organiser's bare address and the room's
answer - no `SUMMARY`, no times, no location, no guest list. The reason is in the
text part and in a `COMMENT`:

* "You are not one of the people allowed to book Board Room."
* "Board Room does not take recurring meetings; book each occurrence on its own."
* "Board Room can be booked at most 30 days ahead, and this meeting is on Nov 02, 2026 10:00 - 11:00 UTC."
* "Board Room can be booked at most 180 days ahead, and this series has no end."
* "Board Room can be booked for at most 60 minutes at a time, and this meeting lasts 120."
* "Board Room can be booked in working hours only - 08:00 to 18:00 UTC, on its working days - and this meeting, ..., is not."
* "Board Room is already booked at that time: Sep 29, 2026 10:00 - 11:00 UTC."
* "This meeting is in the past."

**A conflict says when, never whose.** As Exchange's default does, the answer
names the times the room is busy and not the other booking's organiser, title or
guests: the room's calendar is not everybody's to read.

**A `CANCEL` frees the slot** - from the booking's organiser only, believed as
above, since a UID is mailed to every guest. The whole booking is marked
cancelled; a `CANCEL` of one occurrence (a `RECURRENCE-ID`) takes that occurrence
out of the series with an `EXDATE`, and the rest stays booked. **An updated
`REQUEST`** - a higher `SEQUENCE` - is decided again from the start, the booking
it replaces not counting against it; one with a lower `SEQUENCE` than the booking
held, or under a UID another organiser holds, is not decided at all.

**A change to one occurrence** - a `REQUEST` that carries only a component with a
`RECURRENCE-ID`, which is how Google and Apple send a moved occurrence of a
series - is decided as **that occurrence alone**, against the room's other
bookings and the series' other occurrences. Accepted, it is merged into the
series the room holds, replacing any earlier change of the same occurrence;
declined, that occurrence alone leaves the series with an `EXDATE`. The series
itself is never replaced or deleted because of one occurrence. A room invited to
one occurrence only, with no series held, books that occurrence on its own. A
message that changes several occurrences at once is left for the delegates.

**Two requests for one room are decided one after the other.** The room's queue
has one worker at a time, and the decision and the filing of its answer are one
step under a lock of the room's own, which a delegate's write to the room's
calendar over REST or CalDAV takes too - a CalDAV write reads its preconditions
under it as well - so neither lands in the middle of a decision. The lock is this
process's: **two servers sharing one database are not serialised**, and could
each accept a request for the same slot; running rooms on more than one node at
once waits for the Roadmap2 row "Active-active nodes".

## Left for the delegates

A request whose organiser the room believes is left for the delegates when:

* the policy answers nothing automatically (`auto_accept` off);
* it was sent automatically (an empty reverse path, or `Auto-Submitted`);
* the organiser has had the hour's answers;
* it changes several occurrences of a series in one message;
* no decision could be reached: the calendar could not be read, the
  invitation's times cannot be placed, a booking the room holds cannot be
  expanded in full, or the booking could not be written. A fault among these also
  writes **one line** to the error log - HM7430 the policy, HM7431 the calendar or
  the decision, HM7432 the booking not written, HM7434 anything unforeseen or a
  `CANCEL` that could not be filed; HM7433 is an answer decided and filed that
  could not be queued. None of them fires on a server that has no room. A
  policy that cannot be read (HM7430) believes nobody, since an organiser from
  outside needs its list: the request is treated as one not honoured, spends no
  organiser's hour, and the notice repeats nothing of it.

Such a request is **never put into the room's calendar**: the room has not
decided it, and a booking filed as its sender wrote it - a series with no end,
say, or the room's own `PARTSTAT=ACCEPTED` - would hold the slot on the sender's
word. It stays in the room's mailbox, a booking the calendar already holds stays
as it was, and each delegate is told by mail, from the room: "Booking request
for Board Room: ...", who asked, for when, what, and why it was not answered
automatically - within the organiser's hour of answers, and then once an hour.
A delegate who approves books the time in the room's calendar themselves (their
delegate share can write to it) and tells the organiser; nothing answers the
organiser on the room's behalf for a request it left. A room with no delegates
keeps the request and tells nobody - name at least one.

**Every request stays in the room's mailbox**, answered or not, as every message
delivered to it does. A room's mailbox has a quota like any account's, and once
it is full nothing more is delivered to it, so nothing more is answered: give a
room a quota and a retention that suit it - the account's `MessageRetentionDays`
([SettingLevels.md](SettingLevels.md)) empties it of what is older, since nobody
signs in to.

## Where people find a room

* **The directory** ([Directory.md](Directory.md)): a room is listed with
  `kind` `room` (or `equipment`) and its `capacity`, on
  `GET /api/v1/me/directory` and in the webmail's Directory page; its card in the
  read-only CardDAV directory says `KIND:location` (`KIND:device` for
  equipment).
* **CalDAV** (RFC 6638): every principal says its `calendar-user-type` -
  `INDIVIDUAL` for a person, `ROOM`, `RESOURCE` for equipment. A room's or a
  piece of equipment's principal, `/dav/principals/<address>/`, can be read by
  any account of its domain (its name, its address and its type; not its
  calendar), and the `principal-property-search` REPORT on `/dav/principals/`
  (RFC 3744 section 9.4) - what a client's room finder sends, typically with
  `calendar-user-type` `ROOM` - answers the domain's rooms and equipment whose
  name, address or type contain what was asked. People are not searched there
  (the directory is where a colleague is found), a person's principal stays
  theirs alone, and with the directory switched off nothing is answered.
  Whether a client shows a room finder at all is the client's: Apple Calendar
  searches for locations this way; Thunderbird has no room finder.

## Kept, audited, erased

The policy is carried by the backup (`hm_bookingpolicies` is an account store;
the kind is an attribute of the account's node), and goes with the account when
it is deleted. The delegate shares are the calendar's and, like every calendar
share, are not carried: name the delegates again after a restore. The answer
memory (`hm_bookinganswers`) is an hour long and is not carried either, as the
out-of-office memory is not. The record of whom each booking was accepted from
(`hm_bookingdecisions`) is carried with the calendar it answers for, so a
delegate's decline of a restored booking still tells its organiser. Every
change of the policy is on the audit trail. An address erased
([PersonalData.md](PersonalData.md)) is taken out of every policy's bookers, as
written or through an alias domain; a list the erasure leaves empty turns that
room's automatic answers off rather than opening it to the whole organisation;
and every room's memory of the address as an organiser - under its own key and
under the one its unverified requests were counted by - and its record of the
bookings accepted from it are forgotten.

## What Exchange does that this does not

Kept to what a small organisation uses. Left out, deliberately:

* **Requests in and out of policy for different people** (`BookInPolicy`,
  `RequestInPolicy`, `RequestOutOfPolicy`): here one list says who may book, and
  everything else is declined or left for the delegates as a whole.
* **Approval by a delegate of an in-policy request before it is accepted**
  (`TentativePendingApproval`): with `auto_accept` off every request goes to the
  delegates; there is no "accept it tentatively and ask".
* **Declining one occurrence of a series and accepting the rest**
  (`MaximumConflictInstances`, `ConflictPercentageAllowed`): a series with any
  conflicting occurrence is declined whole. (A change to one occurrence, sent
  on its own, is decided on its own - above.)
* **Truncating a series at the booking window** (`EnforceSchedulingHorizon`
  false): a series that runs past the window is declined.
* **A minimum duration**, and **rewriting the booking** - removing the subject,
  the body, the private flag, adding the organiser's name to the subject
  (`DeleteSubject`, `DeleteComments`, `AddOrganizerToSubject`,
  `RemovePrivateProperty`): the booking is filed as the organiser sent it, and
  the calendar is read only by its delegates.
* **An additional text in the answer** (`AdditionalResponse`), and custom
  resource properties (a projector, a whiteboard) or room lists, buildings and
  floors: capacity is the one property kept.
* **Free-busy of a room on another server**, scheduling over CalDAV (RFC 6638's
  schedule-inbox and schedule-outbox), and a room finder in the webmail's own
  event editor: a room is booked by inviting its address, from any client.
* **Believing an organiser by anything but the server's own record**: a
  forwarded invitation, one relayed by a list on another server, or one sent
  through an API key is not the organiser's, and is left to the delegates'
  notice. (A list of this server delivers the organiser's own queued message to
  its members, so an invitation sent to a local list with the room on it is
  believed as if sent to the room.)
* **Answering a request the room left, on its behalf, from the notice**: the
  delegates book it themselves; there is no "accept on behalf" of a request
  that is not in the room's calendar.
* **COM properties for the policy** (above), and so the policy in the desktop
  Control Panel, which sets only the kind.

## What proves it

`RegressionTests.API.RoomsAndEquipment`, on ports 10210 to 10212, twenty-four
tests, every local organiser who is to be believed sending signed in over SMTP:
a free slot accepted, with the REPLY's `METHOD`, `PARTSTAT`, `UID`, `SEQUENCE`,
`CUTYPE` and sender checked and the room busy to a colleague; an event made on
the organiser's page with the room as a guest, and the room's answer filed into
the organiser's event as `ACCEPTED`; a conflict with a
booking sent as `TRANSP:TRANSPARENT` declined with the time, without the other
booking's organiser or title and without a word of the declined request; a
booking past the window, one outside the working hours and a series with no end
declined, and one inside the window accepted; two hours declined where sixty
minutes is the most, and sixty minutes accepted; an organiser the list does not
name declined and not filed, and one it does accepted for the same slot; a
request written in Alice's name without her signing in, and one Bob signed in to
send in her name, neither filed nor answered, the delegate told once, and
Alice's own request for the same slot then accepted; a request in Alice's name
sent through the send API with a key that may send as any address of the domain
neither filed nor answered, the delegate told, and Alice's own then accepted; an outside organiser named
among the bookers from a domain that publishes nothing, neither filed nor
answered, the delegate told without a word of the request, and nothing leaving
the queue; an outside organiser named among the bookers whose domain the suite's
DNS zone makes pass DMARC (SPF for the loopback address, aligned with the From),
accepted and answered through the queue `MAIL FROM` the room, another sender at
that domain - for which DMARC passes too - writing the organiser in, of a request
and of a `CANCEL` of the organiser's booking, neither believed and the booking
standing, the same pass for a domain the bookers do not name not believed, and a named domain with an SPF
record and no DMARC policy - the SPF pass alone - not believed either, with
"DMARC did not pass" and nothing leaving the queue; two forged `CANCEL`s - Bob
signed in with Alice's `From`, and nobody signed in with Alice's `MAIL FROM` -
leaving the booking, and the organiser's `CANCEL` freeing the slot for another
request; an update moved onto a taken slot declined with its old slot freed, and
moved back accepted with the new `SEQUENCE`; a change to one occurrence of a
weekly series accepted and merged, and another moved onto a taken slot declined
with that occurrence alone leaving the series; two occurrences booked without
their series, one at `SEQUENCE` 3 and one at 1, the second moved at 2 and
accepted, a `CANCEL` of the whole booking at 0 leaving it (Bob's request for
the hour declined after it), a `CANCEL` of the second occurrence freeing it
alone, and one of the whole booking at 3 freeing the rest; a room holding one
occurrence at `SEQUENCE` 3 and not its series freed by the organiser's `CANCEL`
of the series at 1 that names that occurrence at 3; an invitation whose
summary is an emoji answered with it whole; with automatic answers off, no
REPLY, nothing in the room's calendar, the request in its mailbox and the
delegate told by mail; a booking the room accepted declined by the delegate over
REST - `From:` the room with `Sender:` the delegate - and another declined from a
CalDAV client, each leaving the calendar and each telling the organiser; from
a CalDAV client, a colleague with a plain write share declining a booking, and
declining another after writing Bob in as its `ORGANIZER`, each taking it out
and telling nobody, a delegate declining a booking whose `ORGANIZER` the
colleague rewrote telling nobody, and a delegate whose own `PUT` names Bob
telling Alice, whom the room believed - Bob mailed nothing, Alice nothing but
her acceptances and that one decline; a request sent `Auto-Submitted` not
answered, not booked, and the delegate told; two requests for one slot sent at
once ending with one booking; the room refused on REST, POP3 and IMAP and
signing in again as a person, with COM's `Kind` read, refused out of range and
written; the directory's `room` and `capacity`, the principal's
`calendar-user-type` and the room finder's search; and the policy route's
refusals and round trip, a group kept by its own name and a group deleted since
refusing nothing. The server's own tests (`ResourceBookingTester`, run by
`Infrastructure.MainOperations.TestInternals`) prove that a DMARC pass for a
From domain of 253 characters is never recorded - it would not fit its column -
that 249 is the longest that is, whole, and that nothing but DMARC for exactly
the organiser's domain vouches for them.
`RegressionTests.Infrastructure.BackupAccountStores.WhomARoomAcceptedABookingFromComesBack`
restores a booking the room accepted and has a delegate decline it, which tells
its organiser;
`RegressionTests.Infrastructure.BackupAccountStores.TheBookingPolicyComesBack`
carries the kind and the policy through a backup and a restore. The Deck's
harness checks the kind's select, the card, and that a save sends only what
changed.

**Not proved by a test**, and why: that the room's queue and lock keep two
simultaneous requests apart (the test of two requests at once checks the outcome,
and could not tell a working lock from a missing one - the window is
milliseconds, and no switch to widen it is built into the server); a decision
put off by a stop or a Reinitialize and made by the next sweep, and a delivery
given up after three tries (the suite has no way to stop the server between a
delivery and its decision, or to make a decision fail on purpose, and no switch
to make one is built into the server); `XCLIENT LOGIN=`
naming a room (it needs a trusted upstream's address range); a credential
confined to named domains refused a delegate or a group outside them; and two
servers on one database, which are not serialised at all.
