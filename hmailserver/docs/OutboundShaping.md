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

1. The remote domain policy's **largest message** (`5.3.4`, permanent). First
   because it never connects and its answer will not change: a message that
   will never go should not wait for a window first.
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
7. DNS, MTA-STS, DANE, and the connection attempts.

Every one of 2 to 6 is a deferral this server chose, with `4.4.5` ("mail system
congestion") and a sentence naming the rule, and none of them is counted (see
below). The **target** every check is keyed by is what the batch is delivered
to: the recipient domain of an MX delivery, or the host of a route or a relay,
lower-cased. A provider's MX hosts share its limits, so the domain is the right
grain; a smart host is one destination whatever it relays for.

The signal
----------

After the connection attempts, every recipient whose delivery failed
non-fatally *and* whose failure carries a reply a remote really gave is read.
The reply is a request to slow down when:

| Reply | Read as |
|---|---|
| Code **421** | A signal, whatever the text: the service is closing the channel. At the banner or anywhere after it. |
| A 4xx whose enhanced status is **4.7.0**, **4.7.28**, **4.7.500**, **4.7.650**, **4.3.2** or **4.4.5** | A signal. These are the codes providers decorate their deferrals with; 4.4.5 is also the code this server's own shaping uses, so a peer that shapes as this one does is read the same way. |
| A 4xx whose text, lower-cased, contains one of the phrases below | A signal. |

The phrases: `rate limit`, `rate-limit`, `ratelimit`, `too many`, `too much`,
`unusual rate`, `unexpected volume`, `temporarily deferred`, `temporarily
rate`, `throttl`, `try again later`, `server busy`, `back off`, `slow down`.
The table is fixed, not editable: a table an administrator can widen is a table
that eventually matches one mailbox's own words, and the point of the signal is
that it means the *destination*.

**Deliberately not a signal:**

* any **5xx** - a refusal is not a request to wait;
* **4.7.1** - greylisting and policy refusals answer it, and it is about this
  message, not the destination; a back-off on it would hold the whole
  destination after every first contact. It wins over the text: `451 4.7.1
  please try again later` is greylisting, not a slow-down;
* **4.2.x** - one mailbox (busy, full, over quota), never the destination;
  wins over the text too;
* a connection that was **refused or timed out** - nothing was said;
* a **DNS failure**, and this server's **own deferrals** - the reply field is
  empty for both, because RFC 3464's Diagnostic-Code is only ever what the
  other MTA said.

The window and the probe
------------------------

The first slow-down reply from a destination opens a window of
`OutboundBackoffMinutes` (2 by default; 0 turns the whole back-off off).
While it is open nothing is sent to that destination: a batch for it is held
with `4.4.5`, the reason names the destination, when it asked and what it
said, and the application log carries one line:

    Delivery to <target> deferred: the destination asked this server to slow
    down at <time> (<reply>); next attempt after <time>.

Deliveries to every other destination are untouched.

When the window has passed, the **first** delivery to the destination goes
through as the **probe**; any other arriving while the probe is out waits one
minute, not counted as deferred. Then:

* the probe **delivered**, or failed with anything that is not a slow-down
  reply - a 5xx, a plain 4xx, a refused connection: the window is cleared and
  the message's own outcome stands;
* the probe was **answered with a slow-down reply** - a 421 at the banner
  counts, a refused connection does not: the window **doubles**, up to
  `OutboundBackoffMaxMinutes` (60 by default), from that moment, and the entry
  records the new reply.

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
anybody else's mail; over the ceiling the batch is deferred one minute. The
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
period) is the volume control.

A deferral this server chose is not counted
-------------------------------------------

Every shaping deferral - the back-off, the two connection ceilings, the tenant
share, the policy's messages a minute, and `MaxOutboundPerDestinationPerMinute`
- marks its recipients non-fatal with `4.4.5` and the reason exactly as before,
so the queue and a later delivery report still say why. What changed is the
rescheduling. When **every** non-fatal failure of a delivery pass was such a
deferral, the message's next try is set to the longest deferral of the pass
(the rest of the window; a minute for the others) with **the tries counter
untouched**, and the log says so:

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
`destinations`; the tenants under `tenants`. An empty map is nothing open.

**`DELETE /api/v1/queue/shaping/{destination}`** (`manage_queue`) clears one
destination's window, so that an administrator who has fixed the cause need
not wait for it: 200 `{"cleared":true}`, 404 when no window is open for that
name, a line in the application log, and an event on the audit trail (object
type `queue`, action `back-off cleared`). The next slow-down reply opens a
new window from the base.

**The Deck's Delivery queue** shows the same under the queue as *Slowed
destinations*: each with its window, the reply and the host, a *Resume*
button that calls the route (after asking), and the connection counts.

The **four settings** are on the Control Panel's server settings page,
Delivery tab, beside `MaxOutboundPerDestinationPerMinute`; over COM as
`Settings.OutboundBackoffMinutes`, `OutboundBackoffMaxMinutes`,
`MaxOutboundConnectionsPerDestination` and `MaxOutboundConnectionsPerTenant`;
over REST on `/api/v1/settings` as `outbound_backoff_minutes`,
`outbound_backoff_max_minutes`, `max_outbound_connections_per_destination` and
`max_outbound_connections_per_tenant`. Each refuses a value outside its range
with a sentence (0 to 10080 minutes; the ceiling 1 to 10080 and never below the
base - to raise both, raise the ceiling first; 0 to 1000 connections). The
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
window while `450 4.2.1`, `451 4.7.1`, a 550 and a refused connection do not;
the probe after a one-minute window doubles the window on the same answer and
clears it on a delivery; a deferral by `MaxOutboundPerDestinationPerMinute` at
one try is not bounced and arrives within the minute; the policy's messages a
minute and connection ceilings, which had no test before; the server-wide
connection ceiling where no policy applies, and a policy's value winning over
it; the tenant share holding one hosted domain and not another, and relayed
mail sharing `(external)`; the four settings over REST and COM with their
refusals. `DestinationBackoffTester`, inside the service, holds the signal
table to eleven replies each way and the window's arithmetic - doubling, the
ceiling, the probe, the stale probe - against a clock it moves. The Deck's box
is checked in `build/deck-script-test.js`.

Not done
--------

* No per-tenant **volume** quota beyond the per-account one: this shares
  connections, not messages a day.
* The back-off state is **per process**, not shared between nodes.
* The signal table is **fixed**, not editable.
* A window is keyed by the **target**, so two routes to one host on different
  ports are one destination, and a provider whose MX hosts differ in what they
  accept is still one destination.
