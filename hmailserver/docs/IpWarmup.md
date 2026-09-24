IP warm-up: a schedule for a new outbound address
=================================================

A large mailbox provider judges a sending address by its history. An address
with none that starts sending thousands of messages a day looks, to Google or
Microsoft, exactly like a spammer's new address: its mail is deferred, then
filtered, then refused. What every sending service does about it is a
**warm-up** - so many messages a day to each large provider on the first day,
a little more every day after, over some weeks, until the address has a
history and the limits can go.

This server does that in the delivery queue. A **schedule** holds one outbound
address to a daily ceiling of recipients per provider, rising day by day along
a fixed plan; the queue sends up to the ceiling and holds the rest, usually
until the next UTC day. The counts are on the Control Deck's queue page. The
code is `SMTP/IpWarmup.cpp` (the schedules, the plans and the counts),
`SMTP/OutboundProviders.cpp` (which provider a delivery goes to) and the
checks in `ExternalDelivery::DeliverToSingleDomain_` and
`RescheduleDelivery_`.

Nothing is held until an administrator makes a schedule. A server with none -
which is every server after the upgrade - delivers exactly as it did.

What a schedule is about
------------------------

A schedule is about **one outbound address**: the local IPv4 or IPv6 address a
delivery leaves from. That is the address this server binds for the
connection - a global rule's *bind to address* action when one applies, which
overrides the setting, otherwise `SMTPDeliveryBindToIP` (*Bind outbound
connections to IP* under Delivery of e-mail in the Control Panel,
`smtp_delivery_bind_to_ip` on `PUT /api/v1/settings`). Of either, only the
first address of the recipient host's family is bound: the setting may name
one address of each family, and a second address of one family is never
used. A delivery that binds no address - which is what happens when both are
empty, or name nothing of the host's family - leaves from whatever address
the operating system chooses, and no schedule is consulted for it.

So a schedule does nothing, even enabled, until two things outside it are
true:

1. the address is configured on this host's network interface;
2. `SMTPDeliveryBindToIP`, or a global rule's bind-to-address action, binds it.

This server can check the setting and nothing else: the route that lists the
schedules says whether `SMTPDeliveryBindToIP` binds the address the way a
delivery reads it - the first entry of the address's own family
(`in_bind_setting`) - and the Deck shows a warning when it does not. Whether
the address is on the host, or whether a rule binds it, cannot be seen from
there; a schedule for an address only a rule binds counts all the same.

A batch is judged against the address it will bind for the **first** mail
host it is going to try. When the delivery falls back to a later host that
would be reached from another address - the other family's, when the setting
names one of each - and that address is in warm-up too, the host is skipped
and the log says so in one line: this batch reserved nothing of that
address's day, and a delivery from it would be uncounted and past its ceiling.
A later host reached from the same address, or from an address with no
schedule, is tried as it always was.

A schedule has a **start** - the UTC date of its day 1 - and a **plan**. Today
is day 1 on the start date, day 2 the next UTC day, and so on; a start later
than today is refused. The days are UTC days, whatever the server's own time
zone: a provider's day does not follow it either.

The providers
-------------

Each large provider keeps its own reputation for an address, so each gets its
own ceiling. Which provider a delivery goes to is decided from the **mail
host** it connects to - an MX host of an ordinary delivery, the host of a
route or of the relay - and **never from the recipient's domain**: a company
whose mail is hosted by Google Workspace is reached through Google's MX hosts,
and it is Google who judges the address. The host is the first the batch will
try, the one its source address was chosen for, so that the address and the
provider are about the same connection. A backup MX host of another provider
further down the list decides nothing; a batch that falls back to one is
counted against the first host's provider.

A host belongs to a provider when its name is one of the provider's names, or
ends with a dot and one of them (case ignored, a trailing dot dropped) - so
`alt1.aspmx.l.google.com` is Google's, and `notgoogle.com` is nobody's.

| Provider | Key | Mail host names |
|---|---|---|
| Google | `google` | `google.com`, `googlemail.com`, `aspmx.l.google.com` |
| Microsoft | `microsoft` | `outlook.com`, `hotmail.com`, `protection.outlook.com`, `olc.protection.outlook.com` |
| Yahoo | `yahoo` | `yahoodns.net`, `yahoo.com`, `aol.com` |
| Apple | `apple` | `icloud.com`, `apple.com` |
| Proton | `proton` | `protonmail.ch`, `proton.me` |
| Every other provider | `other` | everything else |

The table is fixed: it cannot be edited, from the Deck or anywhere else. The
five named providers are the ones whose reputation systems a warm-up plan is
written for. Every other destination is counted together as `other`, with a
ceiling five times the named providers' - a thousand small providers do not
share one reputation, and holding all of them to Google's first-day fifty
would stop a new address being used at all.

A delivery through a relay or a route is classified by the relay's or the
route's host name: mail relayed through a smart host that is not one of the
names above is `other`, whoever the smart host delivers it to.

The plans
---------

A plan is a fixed table of the daily ceiling for **each named provider**, by
the day of the warm-up. After its last day there is no ceiling, and the
schedule does nothing more: nothing is held and nothing is counted. The plans
cannot be edited.

**standard** - thirty days:

| Day | Ceiling per named provider | `other` |
|---|---|---|
| 1 | 50 | 250 |
| 2 | 100 | 500 |
| 3 | 200 | 1,000 |
| 4 | 500 | 2,500 |
| 5 | 1,000 | 5,000 |
| 6 | 2,000 | 10,000 |
| 7 | 5,000 | 25,000 |
| 8 to 10 | 10,000 | 50,000 |
| 11 to 14 | 20,000 | 100,000 |
| 15 to 21 | 50,000 | 250,000 |
| 22 to 30 | 100,000 | 500,000 |
| 31 on | none | none |

**gentle** - half of each ceiling, and twice the days: gentle day N is
standard day (N + 1) / 2, halved. Sixty days:

| Day | Ceiling per named provider | `other` |
|---|---|---|
| 1 and 2 | 25 | 125 |
| 3 and 4 | 50 | 250 |
| 5 and 6 | 100 | 500 |
| 7 and 8 | 250 | 1,250 |
| 9 and 10 | 500 | 2,500 |
| 11 and 12 | 1,000 | 5,000 |
| 13 and 14 | 2,500 | 12,500 |
| 15 to 20 | 5,000 | 25,000 |
| 21 to 28 | 10,000 | 50,000 |
| 29 to 42 | 25,000 | 125,000 |
| 43 to 60 | 50,000 | 250,000 |
| 61 on | none | none |

**fast** - twice each ceiling, and half the days: fast day N is standard day
2N - 1, doubled. Fifteen days:

| Day | Ceiling per named provider | `other` |
|---|---|---|
| 1 | 100 | 500 |
| 2 | 400 | 2,000 |
| 3 | 2,000 | 10,000 |
| 4 | 10,000 | 50,000 |
| 5 | 20,000 | 100,000 |
| 6 and 7 | 40,000 | 200,000 |
| 8 to 11 | 100,000 | 500,000 |
| 12 to 15 | 200,000 | 1,000,000 |
| 16 on | none | none |

Changing a schedule's plan or start changes the ceilings from that moment; it
does not change what was counted.

What is counted
---------------

**Recipients, not messages.** A message to forty people at Gmail is forty to
Google, because what a provider's reputation counts is the recipients it was
offered.

For each schedule the server counts, per UTC day and per provider:

* **sent** - recipients delivered from the address: the remote accepted the
  message for them. Recipients a delivery still in progress holds are not in
  it until that delivery ends.
* **deferred** - recipients held over the day's ceiling. A recipient held on
  three passes in one day is three.

Beside them each day's row keeps the **ceiling** and the **warm-up day** that
were in force when it was last written, so that the history says what applied
then, not what the schedule says now.

The count is taken in two steps. Before a delivery connects, its recipients
**reserve** room out of the day's allowance: the ceiling, less what was sent,
less what other deliveries still in progress hold - as many as fit. The
reservation is held in memory only. When the attempts are over it is
**settled**: every recipient that was delivered is added to sent, and the whole
reservation is let go. So a recipient that was not delivered is not counted,
whatever stopped it - a connection that was refused or timed out, a remote
that deferred or refused it at any stage, a TLS requirement that could not be
met - and a delivery that failed leaves the count where it was.

That is per recipient, and it is a choice: a warm-up is meant to limit what a
provider sees, and a provider that refuses the warming address - `5xx` at
`RCPT TO`, `421` deferrals - is offered as many recipients a day as the queue
has, since only what it accepts is bounded. Its own slow-down replies are
answered by the back-off ([OutboundShaping.md](OutboundShaping.md)), not by
the warm-up.

The counts are written to the database at every change (`hm_warmupcounts`),
so a restart of the service resumes the day where it was. Only mail to other
servers is counted; local delivery is not.

The hold
--------

When a delivery's recipients do not all fit, the ones that fit go, and the
rest - the last of the batch - are **held**:

* nothing is sent for them, and no connection is made for them;
* they are marked `4.4.5` (mail system congestion), and counted as deferred;
* when the **day's ceiling** is what was reached - what was sent fills it, or
  the batch alone is more than the day has left - the message's next try is
  set to one to five minutes after the next midnight UTC (spread by the
  message's id, so a held backlog does not come back in one minute);
* when the room is held by **other deliveries still in progress** from the
  address, which may give it back, the next try is those one to five minutes
  alone, and the sentence says the rest of today's allowance is held by
  deliveries in progress.

Whether the try is counted against the message's retries, and so when it
really is, is decided after the whole message has been through the queue, by
the rule every deferral this server chooses for itself follows
([OutboundShaping.md](OutboundShaping.md)). The log line and the text a
delivery report would carry are written then, from the path the message took:

* **Not counted** - every failure of the pass was this server's own, and the
  message is younger than its retry schedule's span (the number of retries and
  one, times the minutes between them). The next attempt is the hold's own:
  after midnight UTC, or a few minutes.
* **Counted, at the hold's own length** - the message has been queued longer
  than that span, and the warm-up's hold is longer than the retry interval, as
  a hold to the next UTC day is. The try is counted and still lands after
  midnight UTC. Tried at the retry interval instead, it would come back inside
  a day whose ceiling cannot reopen, be held again and spend a retry each time,
  and the message would be returned within hours with the next day's room
  unused. This is the warm-up's alone: a pass that only the shaping row's
  deferrals held is counted at the interval as it always was.
* **Counted, at the retry interval** - another recipient of the same message
  failed at a remote in the same pass, which is what the retry budget is for.
  The held recipients come back with it at the retry interval, inside the day,
  and are held again.
* **Returned** - the retries are spent. The report's text says no further
  attempt is made.

For example, in the application log:

    SMTPDeliverer - Message 1234: 1 recipient(s) held by the IP warm-up of
    192.0.2.10: day 1 of its standard schedule allows 50 recipient(s) a day at
    Google, and today's have been sent. Next attempt after midnight UTC, in
    723 minute(s); the attempt is not counted against the retry limit.

**How long a backlog is carried.** A held recipient is offered again once on
each following UTC day until it goes or its retries run out: the first return
is not counted when the message was queued within its retry span before
midnight, and every return after the span is counted. So with the default
schedule - four retries, sixty minutes apart, a five-hour span - a recipient
queued in the morning comes back on each of the next five UTC days and is
returned if the fifth still has no room for it; one queued in the last five
hours of a day gets one more. What the address can send over those days is the
sum of their ceilings, less what other mail takes: send no more from a warming
address than that, or raise the retry count for the duration of the warm-up.

Where the check sits: after every check [OutboundShaping.md](OutboundShaping.md)
lists - a batch the warm-up holds has already passed, and been counted by, the
per-minute rates above it - and after DNS and MTA-STS, because the provider and
the address are decided from the first host; before DANE and the connection
attempts.

When the database does not answer, the error is reported and the queue does
not stop: schedules that cannot be read (7210) and a day's count that cannot
be read or started (7211, 7212) leave the delivery going as though the address
had no schedule. A count that cannot be saved (7213) is kept in memory, and the
next change to it writes the whole count again; if the service restarts or is
reinitialised first, the unsaved part is lost and the day can go past its
ceiling by that much.

The routes
----------

On the REST API, under the queue's permissions - `view_queue` to read,
`manage_queue` to change - and refused to a credential confined to named
domains, with the queue's own sentence, because a schedule is about an address
every domain's mail leaves from; a read-only key reads and does not write. A
schedule changed over REST is recorded on the audit trail as a *warm-up
schedule*; the counts are the queue's own writes and are not.

| Route | What it does |
|---|---|
| `GET /api/v1/warmup` | `today` (the UTC date) and `schedules`: each with `id`, `address`, `plan`, `started`, `enabled`, `day` (which day of the warm-up today is), `last_day`, `complete` (the plan is over), `in_bind_setting`, and `providers` - for each of the six, today's `sent`, `deferred` and `ceiling` (`null` when there is none). |
| `POST /api/v1/warmup` | Adds a schedule. `address` (required), `plan` (`standard`, `gentle` or `fast`, or 0, 1 or 2; standard by default), `started` (`YYYY-MM-DD`, today by default, never later than today) and `enabled` (true by default). 201 with the entry. |
| `PUT /api/v1/warmup/{id}` | Changes `plan`, `started` or `enabled`; a field left out keeps its value. The address cannot be changed, since the counts are about it: delete the schedule and add one for the other address. |
| `DELETE /api/v1/warmup/{id}` | Removes a schedule and its counts. Mail it held waits for its next try and then goes as any other. |
| `GET /api/v1/warmup/{id}/history` | The schedule's counts, newest first: `date`, `day`, `provider`, `sent`, `deferred`, `ceiling`, with the day and the ceiling that were in force that day, stored with the count. A date on which nothing was delivered to or held for a provider has no entry. |

Any syntactically valid IPv4 or IPv6 address is accepted (not `0.0.0.0` or
`::`, which mean "no bind"), and it is written back the way a delivery names
it - `0:0:0:0:0:0:0:1` is stored as `::1` - so that two spellings of one
address are one schedule. What is refused, with nothing changed: an address
that is not one (400), a plan that is none of the three (400), a date not
written `YYYY-MM-DD` or later than today (400), a field the route does not
know (400), a PUT naming another address (400), and a second schedule for an
address (409) - also when two requests for one address arrive together, where
the database's unique index answers for the second.

The schedules and the backup
----------------------------

The schedules are part of the server's settings: a backup made with **Back up
settings** carries them - address, start as a date, plan and switch - beside
the remote domain policies ([Backup.md](Backup.md)), and a restore of the
settings puts them back. A schedule the restoring server already has for the
same address is changed to the archive's and keeps its counts; one the archive
does not name is removed with its counts; one the archive adds starts with
none. The counts themselves are traffic and are not in the archive, so a
schedule restored onto another server - or moved with the database to another
backend - starts its day at zero there.

The Deck
--------

The Control Deck's **Delivery queue** page has a **Warm-up** box under the
slowed destinations. Each schedule shows its address, its plan, the day it is
on (or that it is complete) and, for each provider, today's sent against the
ceiling and what was deferred; a warning when `SMTPDeliveryBindToIP` does not
bind the address. **Edit** opens a small editor - the address, shown and not
changeable, the plan, the day 1 and the switch - with **Delete** beside Save;
an emptied day 1 is today. **Add a schedule** opens the same editor empty. The
editor and the buttons are there for a role with `manage_queue`; a role with
`view_queue` sees the counts. When the listing cannot be read, the box says so
and the rest of the queue page is drawn as ever.

The desktop Control Panel has nothing for it: the warm-up is a page of the
Deck. There is no COM interface either.

What it does not do
-------------------

* **The providers and the plans are fixed.** Neither can be edited; a sixth
  provider, or a plan of your own, is not possible.
* **Per address, not per pool.** A schedule is about one address. There is no
  pool of addresses the queue spreads a day's mail across, and no automatic
  move of the mail that did not fit to another address: it waits.
* **No reputation feedback.** The ceilings rise by the calendar, not by how
  the providers are answering. A provider that is deferring the address
  anyway is slowed down for by the back-off ([OutboundShaping.md](OutboundShaping.md)),
  not by the warm-up.
* **Recipients over the ceiling are the last of their batch**, not chosen by
  any priority.
* The counts are on the Deck and the routes; they are not exported as metrics.

What is tested, and what is not
-------------------------------

`RegressionTests/SMTP/IpWarmup.cs` proves, each against a control: the ceiling
on day 1 and the recipient held being the batch's last, the hold to after
midnight UTC with no retry spent, the counted hold past a retry span still
landing after midnight, no schedule, a disabled schedule and no bind address
holding nothing, a schedule switched on taking effect at the next delivery, a
completed plan holding and counting nothing, a count read again from the table
after a reinitialise, a refused connection moving nothing against a partial
refusal counting what was accepted, the history's stored day and ceiling and
its order, the plans' day arithmetic, the routes' refusals, and the routes'
permissions. `WarmupPlanTester` in the internal suite holds the plans, the
dates, the addresses and the providers table.

Not proved by any test: room held by deliveries in progress (it needs two
deliveries racing); a later host of the other family skipped for being reached
from another warming address (it needs IPv6 to a test server); what a database
failure does (7210 to 7213); the audit trail's entry; the configuration
backup's round trip of the schedules; and an MX lookup's own host names
reaching the classification - every delivery in the fixture goes through a
route, and the MX path is covered by the providers table's unit test alone.
