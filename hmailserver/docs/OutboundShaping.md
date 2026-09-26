Outbound shaping: back-off, connection ceilings and the tenant share
=====================================================================

How this server slows itself down for one destination that asked, and how it
keeps one sender from using every delivery thread - without touching mail to
anybody else, and without spending a message's retry budget on a wait this
server chose. The code is `SMTP/DestinationBackoff.cpp` (the back-off),
`Common/Util/RemoteDomainThrottle.cpp` (the connection counts) and the checks
in `ExternalDelivery::DeliverToSingleDomain_`.

Three things are here, each answering one question a busy server meets:

* **A destination said slow down.** A remote answered a 421, or a 4xx with
  words or a code that mean "not now, too much" - and every further message to
  it was being offered anyway, deferred, and retried on the ordinary schedule,
  each attempt spending the message's retries and each one more reason for
  the remote to keep refusing. Now the destination gets a **back-off window**:
  nothing is sent to it until the window ends, one delivery then probes it,
  and the same answer doubles the window.
* **How many sessions at once to one destination**, for a destination no
  remote domain policy names. The policy's `max_connections` has done this
  per named domain since it was built; `MaxOutboundConnectionsPerDestination`
  is the same ceiling for everybody else.
* **How many deliveries at once for one tenant.** One hosted domain - or all
  the mail relayed for outsiders together - cannot hold every delivery thread
  while the others wait.

And one rule that binds them: **a deferral this server chose does not count
as an attempt.** Only a failure at a remote spends the retry budget.

The order of the checks
-----------------------

For each batch of recipients on one target, `DeliverToSingleDomain_` judges, in
this order, before any connection is opened:

1. The remote domain policy's **largest message** (`5.3.4`, permanent),
   measured on the queued message. First because it never connects and its
   answer will not change: a message that will never go should not wait for a
   window first.
2. The destination's **back-off window** (this document). Held until the
   window ends; when the window has passed, one delivery is let through as the
   probe and any other arriving meanwhile waits a minute.
3. The policy's **messages a minute** (`max_messages_per_minute`).
4. The **connection ceiling**: the policy's `max_connections` where the policy
   sets one, else the server-wide `MaxOutboundConnectionsPerDestination`. A
   policy value wins when present.
5. The **tenant share**, `MaxOutboundConnectionsPerTenant`, taken beside the
   destination slot and released with it.
6. The server-wide **`MaxOutboundPerDestinationPerMinute`**, the fixed
   per-destination rate that predates all of this.
7. DNS: the MX lookup (or the route's host) and MTA-STS; then the **IP
   warm-up**'s daily ceiling for the address the batch will leave from and the
   provider its first host belongs to ([IpWarmup.md](IpWarmup.md)), which holds
   the recipients over the ceiling, usually until the next UTC day; then - where
   DANE applies - whether the MX set is DNSSEC-validated.
8. **Gateway encryption**, for a batch whose domain's policy encrypts
   ([RemoteDomains.md](RemoteDomains.md), "Gateway encryption"): the partner's
   copy is made here, after everything above that could hold the batch back
   without connecting, so a batch that is only going to be deferred is never
   encrypted, signed, logged or traced as encrypted (until 24 September 2026
   this came before step 1). The largest-message ceiling is measured again on
   the copy, which is what is sent (`5.3.4`, permanent).
9. The connection attempts, each host's DANE records judged in turn, where a
   later host that would be reached from another warming address is skipped.

Every one of 2 to 6 is a deferral this server chose, with `4.4.5` ("mail system
congestion") and a sentence naming the rule, and none of them is counted (see
below); the warm-up's hold in 7 is one too, and follows the same rule, with one
addition that is the warm-up's alone: a pass in which it held a recipient for
longer than the retry interval keeps the hold's length when it is counted past
the span, so that its counted tries land on new days (see *The hold is
bounded*, below, and IpWarmup.md). Nothing changes for the deferrals 2 to 6.
The **target** every check is keyed by is what the batch is delivered to: the recipient domain of an MX delivery, or the host of a route or a relay,
or its address when the route names an IP literal; lower-cased. A provider's
MX hosts share its limits, so the domain is the right grain; a smart host is
one destination whatever it relays for. (Until 23 September
2026 `MaxOutboundPerDestinationPerMinute` keyed an IP-literal route by an
empty name, so every such route shared one bucket; it takes the target now.)

The signal
----------

After the connection attempts, every recipient whose delivery failed
non-fatally *and* whose failure carries a reply a remote really gave is read.
The reply is a request to slow down when:

| Reply | Read as |
|---|---|
| Code **421**, unless one of the exclusions below applies | A signal, whatever the text: the service is closing the channel. At the banner or anywhere after it. |
| A 4xx whose enhanced status is **4.7.0**, **4.7.28**, **4.7.500**, **4.7.650**, **4.7.652** or **4.3.2** | A signal. These are the codes providers decorate their deferrals with. |
| A 4xx whose text, lower-cased, contains one of the phrases below | A signal. |

The phrases: `rate limit`, `rate-limit`, `ratelimit`, `too many messages`,
`too many connections`, `too much`, `unusual rate`, `unexpected volume`,
`temporarily deferred`, `temporarily rate`, `throttl`, `try again later`,
`server busy`, `back off`, `slow down`. They are English: a remote that says
the same thing in another language is read by its codes alone. The table is
fixed, not editable: a table an administrator can widen is a table that
eventually matches one mailbox's own words, and the point of the signal is
that it means the *destination*.

**Deliberately not a signal**, and every one of these wins over the text and
over the 421 rule:

* any **5xx** - a refusal is not a request to wait;
* **4.7.1** - greylisting and policy refusals answer it, and it is about this
  message, not the destination; a back-off on it would hold the whole
  destination after every first contact. `451 4.7.1 please try again later`
  is greylisting, not a slow-down;
* a reply whose text says **greylist** (or graylist), whatever its code - a
  greylister that sends no enhanced code says "try again later" too;
* **4.2.x** - one mailbox (busy, full, over quota), never the destination;
* **4.5.3** - too many recipients for one message, which this server's
  batching of up to a hundred recipients can meet on its own; and the same
  reply without an enhanced code is not caught by `too many`, which is why
  the phrase is `too many messages` and `too many connections`;
* **4.4.x** - the network subject: `421 4.4.2 timeout exceeded` is the
  remote's idle timeout on a slow DATA, and it is this message that is slow,
  not the destination that is busy;
* a connection that was **refused or timed out** - nothing was said;
* a **DNS failure**, and this server's **own deferrals** - the reply field is
  empty for both, because RFC 3464's Diagnostic-Code is only ever what the
  other MTA said.

One miss, said rather than hidden: the delivery client keeps a recipient's
first non-fatal reply through every later MX host's non-fatal outcome, so a
genuine 421 from the second host after a plain 4xx from the first is not seen
for that recipient. Changing which reply the client keeps is not this row's;
the first host's reply is the one recorded and the one read.

The window and the probe
------------------------

The first slow-down reply from a destination opens a window of
`OutboundBackoffMinutes` (2 by default; 0 turns the whole back-off off).
While it is open nothing is sent to that destination: a batch for it is held
with `4.4.5`, the reason names the destination, when it asked and what it
said, and the application log carries one line:

    Delivery to <target> deferred: the destination asked this server to slow
    down at <time> (<reply>); next attempt after <time>.

Deliveries to every other destination are untouched. Mind what "destination"
is for a relay: with `SMTPRelayer` set, or a route through a smart host, the
relay is one destination, and one slow-down reply from it holds every message
that goes through it for the window - which is what the relay asked for, and
worth knowing before the window is made long.

When the window has passed, the **first** delivery to the destination goes
through as the **probe**; any other arriving while the probe is out waits one
minute, not counted as deferred. Then:

* the probe **delivered**, or failed with anything that is not a slow-down
  reply - a 5xx, a plain 4xx, a refused connection: the window is cleared and
  the message's own outcome stands;
* the probe was **answered with a slow-down reply** - a 421 at the banner
  counts, a refused connection does not: the window **doubles**, up to
  `OutboundBackoffMaxMinutes` (60 by default), from that moment, and the entry
  records the new reply. Once per probe: the same reply found again after the
  probe's next MX host changes nothing.

A probe nobody ended - a delivery thread that died with it - goes stale after
fifteen minutes and the next delivery takes it over, so a destination cannot
stay closed for good.

What an entry records: the destination, since, until, the window's minutes,
the reply (at most 200 characters, control characters stripped) and the host
that gave it, how many deliveries the window has held back, and whether a
probe is out.

The connection ceilings, and the tenant share
---------------------------------------------

Both use the same counting semaphore the remote domain policy's
`max_connections` uses (`RemoteDomainThrottle`): non-blocking, because a
delivery thread that waited for a slot would be a thread not delivering
anybody else's mail; over the ceiling the batch is deferred one to five
minutes (one plus the message's id modulo five, so that a backlog held behind
one ceiling does not come back as one pass per message every minute). The
slot is held to the end of the last connection attempt for the batch.

* **`MaxOutboundConnectionsPerDestination`** (0, off, by default; 0 to 1000):
  the most sessions open at once to one target that no remote domain policy
  sets `max_connections` for. Where a policy sets one, the policy's value
  wins, whatever this is - an administrator who wrote it keeps it.
* **`MaxOutboundConnectionsPerTenant`** (0, off, by default; 0 to 1000): the
  most deliveries one tenant may have in flight at once. The tenant is the
  **hosted domain of the envelope sender** - the message's from address, with
  an alias domain resolved to its domain through the domain aliases, as the
  delivery code resolves it elsewhere. A sender this server does not host -
  mail relayed for an IP range or a route, forwarded external mail, the null
  sender - is the one tenant **`(external)`**, so relayed mail cannot starve
  the hosted domains either.

Say it plainly: the tenant share is a share of **connections**, bounded by
`MaxDeliveryThreads` - a share larger than the thread count is no share at
all - and it is not a volume control. The per-account daily quota
(`AccountSendingLimits`, the messages and recipients an account may send in a
period) is the volume control. Two consequences worth knowing: this server's
own delivery reports have the null sender, so they share `(external)` with
relayed mail - a burst of relayed mail under the tenant share delays the
server's bounces, and the other way round; and a tenant slot has no stale
recovery of the probe's kind - a session stuck waiting on a remote holds the
slot until the SMTP client's own timeout ends the session, which is bounded,
but it is a whole tenant's slot rather than one destination's.

A deferral this server chose is not counted
-------------------------------------------

Every shaping deferral - the back-off, the two connection ceilings, the tenant
share, the policy's messages a minute, and `MaxOutboundPerDestinationPerMinute`
- marks its recipients non-fatal with `4.4.5` and the reason exactly as before,
so the queue and a later delivery report still say why. What changed is the
rescheduling. When **every** non-fatal failure of a delivery pass was such a
deferral, the message's next try is set to the longest deferral of the pass
(the rest of the window; one to five minutes for the others) with **the tries
counter untouched**, and the log says so:

    Delivery deferred by this server's own shaping, not by a remote server.
    Scheduling it for later delivery in N minute(s); the attempt is not
    counted against the retry limit.

The quick-retry schedule and the retry randomness do not apply: the minutes
are the window's. When **any** recipient of the pass failed at a remote - the
probe answered with a slow-down reply is one such - the counted path runs
exactly as it always has, because a remote failure is what the retry budget is
for. The ETRN hold of a route (`HOLD`) is unchanged.

Before this, a deferral this server inflicted on itself spent the message's
retry budget as a remote failure does, and with a small retry count a message
could be bounced having never been offered to anybody.

**The hold is bounded.** A destination that answers "slow down" for good - a
decommissioned domain whose last MX says `421 4.3.2 Service shutting down`
for ever, an appliance that never recovers - would otherwise turn a bounce in
hours into a hold for as long as the backlog lasts, one probe per window and
the window at its ceiling, and this server sends no delayed delivery report
that would tell anybody. So a message that has been queued for longer than
the span its counted schedule would have allowed it - (retries + 1) times the
minutes between tries, from the time it was accepted - takes the counted path
from then on, whatever deferred it, and the log says so:

    Deferred by this server's own shaping, but it has been queued for N
    minute(s), longer than the M minute(s) its retry schedule spans. This
    deferral is counted, so that the message is not held for ever.

At one try that is the bounce, with the shaping reason in the report.

A counted try comes back at the retry interval, as any counted try does,
whatever held it - a back-off window longer than the interval included. The
one exception is the IP warm-up's ([IpWarmup.md](IpWarmup.md)): a pass in which
it held a recipient for longer than the retry interval comes back at the
hold's own length, and the log says so, because at the interval its tries
would come back inside a day whose ceiling cannot reopen and be spent there.
A pass the deferrals above alone held is counted at the interval exactly as
before.

**And a held pass is a first pass only once.** DKIM/ARC signing, the mirror
copy and a rule's policy notice used to be gated on the tries counter alone,
which an uncounted pass leaves at nought; the reschedule now sets
`FlagFirstPassDone` on the message in the same statement, and the three ask
both, so a message held through ten passes is mirrored once and sealed once.

**Nor does a held pass forward, copy, reply or call a script function again.**
The server's and the domains' mail flow rules run on every pass, and until 26
September 2026 a rule that forwards, copies or replies queued a new message on
each one - one per held pass, which for a message held behind a rate or a
back-off was dozens - and a rule's script function, which may send mail too, ran
on each one. The uncounted reschedule now also sets `FlagRepeatsAttempt`, and
those four actions are not taken on a pass that carries it; a counted retry
clears it, so they run once per counted attempt, as they always have
([MailFlowRules.md](MailFlowRules.md)).

Where to see it
---------------

**`GET /api/v1/queue/shaping`** (the queue's permission, `view_queue`;
server-wide, refused to a key confined to named domains):

    {"backoffs":[{"destination":"example.net","since":"2026-09-22 12:00:00",
      "until":"2026-09-22 12:02:00","window_minutes":2,
      "reply":"421 4.7.0 Messages temporarily deferred due to unexpected volume",
      "host":"mx.example.net","deferred":3,"probing":false}],
     "connections":{"destinations":{"example.net":1},"tenants":{"example.com":2}}}

`connections` is what is open right now under a ceiling: a remote domain
policy's name, or a target under `MaxOutboundConnectionsPerDestination`, under
`destinations`; the tenants under `tenants`. An empty map is nothing open. The
reply is at most 200 characters of the reply as the server holds it (UTF-16
code units), sent as UTF-8 like the rest of the document.

**`DELETE /api/v1/queue/shaping/{destination}`** (`manage_queue`) clears one
destination's window, so that an administrator who has fixed the cause need
not wait for it: 200 `{"cleared":true}`, 404 when no window is open for that
name, a line in the application log, and an event on the audit trail (object
type `queue`, action `back-off cleared`). The next slow-down reply opens a
new window from the base.

**The Deck's Delivery queue** shows the same under the queue as *Slowed
destinations*: each with its window, the reply and the host, a *Resume*
button that calls the route (after asking), and the connection counts as they
were when the page was drawn - a snapshot, refreshed with the page.

The **four settings** are on the Control Panel's server settings page,
Delivery tab, beside `MaxOutboundPerDestinationPerMinute`; over COM as
`Settings.OutboundBackoffMinutes`, `OutboundBackoffMaxMinutes`,
`MaxOutboundConnectionsPerDestination` and `MaxOutboundConnectionsPerTenant`;
over REST on `/api/v1/settings` as `outbound_backoff_minutes`,
`outbound_backoff_max_minutes`, `max_outbound_connections_per_destination` and
`max_outbound_connections_per_tenant`. Each refuses a value outside its range
with a sentence (0 to 10080 minutes; the ceiling 1 to 10080; 0 to 1000
connections), and the two window settings refuse each other's crossing the
same way on every surface: the ceiling is never below the base and the base
never above the ceiling, so to move both past each other, move the ceiling
first when raising and the base first when lowering. What GET and the COM
getter answer is always what the row holds. The
`hm_settings` rows are seeded at start with the defaults; the two connection
rows are named `MaxOutboundConnsPerDestination` and `MaxOutboundConnsPerTenant`
in the table, because `hm_settings.settingname` holds thirty characters.

What is per process, and forgotten by a restart
-----------------------------------------------

The windows and the connection counts live in memory in the server process.
There is one delivery queue in one process, so there is nothing to share; a
restart forgets every window, which costs at most one more slow-down reply per
destination, and forgets every count, which is right because it also closed
every session they counted. Nothing here is written to the database, and
nothing is shared between the nodes of a warm-standby pair.

Turning the back-off off (`OutboundBackoffMinutes` = 0) stops new windows
opening and stops the open ones being consulted; they stay in the listing
until cleared or the service restarts.

Tests
-----

`RegressionTests/SMTP/OutboundShaping.cs` (REST port 9856), against the
suite's SMTP simulator through routes, each with its negative control: a
slow-down reply at MAIL FROM opens a window and the next message to the same
destination is held without a connection while one to another destination
goes at once, with the log line, the listing and the queue's tries; clearing
the window over the route and retrying delivers; a 421 at the banner opens a
window while `450 4.2.1`, `451 4.7.1` (the counted path, one try counted), a
550 and a refused connection do not; the probe after a one-minute window
doubles the window on the same answer and clears it on a delivery; a deferral
by `MaxOutboundPerDestinationPerMinute` at one try is not bounced, waits one
to five minutes with zero tries and arrives once the rate's window has
drained; a held message is mirrored once, not once per pass; three messages
held behind a destination that answers 421 to every session are all bounced
within the schedule's span; the policy's messages a minute and connection
ceilings, which had no test before; the server-wide connection ceiling where
no policy applies, and a policy's value winning over it; the tenant share
holding one hosted domain and not another, and relayed mail sharing
`(external)`; the four settings over REST and COM with their refusals, the
base above the ceiling included. `DestinationBackoffTester`, inside the
service, holds the signal table to twelve replies that are the signal and
sixteen that are not, and the window's arithmetic - doubling, once per probe,
the ceiling, the probe, the stale probe - against a clock it moves. The Deck's
box is checked in `build/deck-script-test.js`.

Not done
--------

* No per-tenant **volume** quota beyond the per-account one: this shares
  connections, not messages a day.
* The back-off state is **per process**, not shared between nodes.
* The signal table is **fixed**, not editable, and its phrases are English.
* A window is keyed by the **target**, so two routes to one host on different
  ports are one destination, and a provider whose MX hosts differ in what they
  accept is still one destination.
* A hold is bounded by the **retry schedule's span**, not by a delayed
  delivery report: this server sends none, so a sender learns of a long hold
  only when it ends in a bounce.
* A deferral by a rate or a connection ceiling costs **one delivery pass per
  message every one to five minutes** - the message loaded, the rules run,
  the targets resolved, a row written and a log line - for as long as the
  backlog lasts. A backlog of thousands behind a rate of one is the shape a
  tenant's abuse produces, and the per-account sending limit is the control
  for that, not this.
* A slow-down reply from a **later MX host** after an earlier host's plain
  4xx to the same recipient is not seen (above).
