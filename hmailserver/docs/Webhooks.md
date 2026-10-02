# Outbound webhooks

**Telling another program what this server's mail just did.**

Five things happen to mail that somebody else's software wants to know about at
the moment they happen: a message was accepted, a recipient was delivered to, a
delivery failed for good, a message was held in the quarantine, an account was
locked out. Until this existed the only way to learn any of them was to read a
log file afterwards. A ticket system that opens a case when mail to a customer
bounces, a CRM that files the receipt, a monitor that pages somebody when an
account is locked — each of those is a program that wants a POST, not a log
parser.

Nothing here is on until you make a subscription. A server with none raises no
event and opens no socket; what it pays is one `select` of `hm_webhooks` at most
every thirty seconds, from whichever thread raised an event, to learn that there
is still nothing to do. Switch `WebhooksEnabled` off and it pays nothing at all.

---

## What a webhook carries, and what it does not

Read this part first. It is the part people are surprised by.

**It does not carry the message.** No body, no attachments, no header block, no
recipient list beyond the one recipient the event is about. A webhook is a
notification, not a copy, and an endpoint that wanted the mail should have been
sent the mail.

**The one piece of content it can carry is the subject**, and only where two
separate switches agree:

| Switch | Where | Default |
|---|---|---|
| `webhook_subjects` | the domain (`PUT /api/v1/domains/{domain}`, `Domain.WebhookSubjects` over COM, the Control Panel's domain dialog on its Webhooks tab) | **off** on every domain |
| `include_subject` | the subscription | **off** |

Even with both on, a subject travels on `message.received` and
`message.quarantined` only — the two events whose owning path already has the
subject in its hands. A delivery and a failure name the same message by its
`queue_id` and carry no subject at all; reading one there would mean parsing the
message file on every delivery.

A domain also has `webhooks_enabled` (on by default). With it off, nothing that
domain's mail or accounts do raises an event, whatever any subscription asks
for. It is the answer a domain administrator can give once, rather than a thing
to police subscription by subscription.

**What a body does carry** is the envelope and the outcome: which event, which
subscription, when, this server's name, and then only the members that event
actually has.

```json
{
  "event": "message.received",
  "subscription": "ticketing",
  "time": 1789000042,
  "time_utc": "2026-09-16 08:00:42Z",
  "server": "mail.example.com",
  "domain": "example.com",
  "account": "ann@example.com",
  "sender": "outsider@example.net",
  "recipient": "ann@example.com",
  "queue_id": 4711,
  "size": 8213
}
```

A member that is absent is a thing this event does not have, so a receiver never
has to decide whether an empty string means "none" or "not told".

| Member | Meaning |
|---|---|
| `event` | one of the five names below |
| `subscription` | the name of the subscription this copy was sent to |
| `time`, `time_utc` | when the event was raised: seconds since the epoch, and the same instant as text |
| `server` | `Settings.HostName` |
| `domain` | the local domain the event belongs to (see *Which domain an event belongs to*) |
| `account` | the local address the event is about, where it has one |
| `sender`, `recipient` | the envelope, for a message event |
| `queue_id` | the delivery queue's id for the message, which correlates the events about one message - for every recipient of it, local ones included: until 24 September 2026 a message delivered to several local accounts gave each `message.delivered` but one the id of that account's own copy |
| `size` | the message's size in bytes: for `message.delivered`, the queued message's, before a local recipient's copy gains its `Return-Path` and `Delivered-To` fields (since 24 September 2026; before, a local recipient's event carried its copy's size, those few bytes larger) |
| `subject` | only where the domain and the subscription both allow it, and only on `message.received` and `message.quarantined` |
| `status` | `250` for a delivery; the RFC 3463 enhanced status for a bounce |
| `reason` | what the remote said, the quarantine's reason, or why an account was locked |
| `number` | the spam score for a quarantine; the minutes a lockout lasts |

### The five events

| Event | Raised where | Raised when |
|---|---|---|
| `message.received` | the SMTP acceptance, beside the per-domain counters | the message has been saved and the server has taken responsibility for it — once per local recipient |
| `message.delivered` | the delivery journal both deliverers call | a recipient, local or remote, has been delivered to |
| `message.bounced` | the delivery-failure report | a non-delivery report has been written and queued for the sender — once per failed recipient |
| `message.quarantined` | the quarantine store | the held copy and its row exist — once per recipient the message was for |
| `account.locked` | the per-account lockout | repeated authentication failures have locked a name |

Note what `message.bounced` is **not**. A delivery attempt that failed and will
be retried is not a bounce — the message is still in the queue. A recipient
refused at `RCPT TO` is not a bounce either: the sending server was told at the
time, and this server sent nothing.

An application that sends through the **send API** ([SendApi.md](SendApi.md))
can have what became of its messages pushed instead of asking for it: a
subscription confined to its sender (`"addresses": "orders@example.com"`) hears
`message.delivered` and `message.bounced` for each recipient outside this server
it sent to, since such an event belongs to the sender's address - with the
message trace on or off. The send API's own `GET /api/v1/send/{queue_id}`
answers the same question when asked, once the message trace is on
(`MessageTraceEnabled`, off by default; without it a message that has left the
queue reads `unknown`), and also says `deferred` - an attempt that will be
retried - which no event here carries.

Every event is raised by the path that owns it, never by anything that polls a
table. Raising writes one queue row per matching subscription and returns;
nothing is POSTed from the thread that noticed. That is the same deferral the
alerts make, for the same reason: a webhook about a delivery must not be sent by
the thread doing the delivering.

### Which domain an event belongs to

The recipient's domain when this server hosts it, otherwise the sender's domain
when it hosts that, otherwise neither. Recipient first because most events are
about mail arriving; the fallback is what makes a delivery to, or a bounce from,
the outside world still belong to the domain of yours that sent it. A message
that is neither from nor to a domain of this server's — which only a relay sees
— belongs to no domain, and a subscription confined to a domain never matches
one.

---

## A subscription

```
POST /api/v1/webhooks
{
  "name": "ticketing",
  "url": "https://tickets.example/hooks/mail",
  "secret": "a-long-random-string",
  "events": "message.bounced,account.locked",
  "domains": "example.com",
  "max_per_minute": 120
}
```

| Field | What it is |
|---|---|
| `name` | the identifier in the path, in `hmctl` and in the Deck. Letters, digits and `-` `_` `.`, at most 64 characters, stored lower case |
| `url` | `https://` to any address **this server** can reach; plain `http://` only to an address on this machine |
| `secret` | **write-only**. The bodies are signed under it; it is never returned, and it is protected at rest (see below) |
| `events` | comma-separated, or a JSON array. Empty means **every** event |
| `domains` | comma-separated domain names. Empty means **every** domain |
| `addresses` | comma-separated addresses. Empty means **every** address |
| `enabled` | default true |
| `include_subject` | default false; see above |
| `max_per_minute` | 0 means the server's `WebhookMaxPerMinute` |

An empty list means "everything", which is what makes a subscription that names
no domain the administrator saying *every domain* rather than *no domain*.

Several subscriptions may want the same event, and each gets its own signed
copy.

**Plain `http` off this machine is refused at the point of configuration**, not
at delivery: a body that says who wrote to whom must not cross a network in the
clear, and a subscription that could never work is better refused when it is
made than every minute afterwards.

### Where an endpoint may be

The endpoint is reached **from this server**, so it may be an address only this
server can reach: a receiver on the same network, behind the same firewall, on
a private range. That is the common case and it is allowed on purpose. The
server does not apply to a subscription's URL the public-address rule it
applies to a URL an outsider chose (a List-Unsubscribe address, say), because
the person who made the subscription is not an outsider.

Say plainly what follows from that. Whoever can create or change a
subscription can make this server connect to a host and port of their choosing
and, through `POST /test`, `POST /run` and the queue's last error, learn whether
it answered, refused the connection or timed out. That is why creating,
changing and testing a subscription need `manage_server` (the permission for
acting *as* the server) rather than `manage_settings`, why a role confined to
named domains is refused, and why no API key reaches these routes at all.
Deleting one needs only `manage_settings`: it is the one change that cannot make
the server reach anything.

**Who can set a subscription's URL**, and so point this server at an address of
their choosing - checked against the code for 6.3.6, the same since outbound
webhooks came in 6.3.4 (schema 6051):

| Surface | Who |
|---|---|
| `POST /api/v1/webhooks`, `PUT /api/v1/webhooks/{name}` (and `POST /test`, `POST /run`, which make the server connect) | The administrator password, or an account whose role holds `manage_server` and is not confined to named domains: the built-in server administrator role (an account with the server administrator's level), or a custom role an administrator made with `manage_server` in it |
| COM `Utilities.SaveWebhookSubscription` | A COM session signed in as the server administrator (`AdministerServer`, which no custom role can hold: the administrator password, or an account with the server administrator's level or role) |
| Anything else | Nobody: no API key, whatever its scope or role (401); no role confined to named domains (403); no domain administrator, whose role holds no `manage_server`; no account through the webmail or `/api/v1/me` |

That is the same set of credentials that may let an inbound webhook route reach
this server's own network ([InboundWebhooks.md](InboundWebhooks.md): a
credential that is not an API key, holds `manage_server` and reaches every
domain), so nobody short of those credentials can make a subscription post to
an address only this server can reach, and a subscription is posted with the
plain HTTPS client: loopback and the private ranges are reached on purpose,
since an administrator's own receiver on the same network is the common case.
Giving a custom role `manage_server` gives its holders that reach; give it to
those you would let act as the server. A post goes through `HttpProxy` when
one is set, except to an address on this machine, as the server's other
requests to an address an administrator named do.

A subscription is not the only address an administrator names that this server
connects to. An alert rule's webhook is posted with the same plain HTTPS client,
and `manage_settings` sets it ([AlertsAndAuditTrail.md](AlertsAndAuditTrail.md));
the same permission sets the server's settings, among them the hosts of its
other clients. A custom role given `manage_settings` can therefore point this
server at its own network as well ([Roles.md](Roles.md)).

### The secret

It can be set and it can be replaced, and it is never handed back. The listing
says `secret_set`, which is what an administrator needs to know and what a
reader of the page must not be able to take away.

It is **protected at rest** the way a route's or a relaying domain's password
is - `Crypt::ProtectSecret`, which on Windows is a DPAPI envelope bound to the
machine and on Linux one bound to the key file - so a copy of the database (a
backup, a replica, a SQL login) does not hand out what every body to the
endpoint is signed with. The alert rules' `alertrulesecret` predates this and is
still stored in the clear; that is a reason to fix it, not a precedent.

An **empty** secret in a change keeps the one that is set rather than clearing
it. A subscription with no secret would send bodies nobody can verify and is
refused anyway; without this rule, a client that sent `"secret": ""` by accident
while switching a subscription off would break a working endpoint for good.

To rotate a secret: `PUT` the new one, and accept either signature at the
receiver for as long as it takes for the change to be made. There is no second
secret and no overlap window in the server.

---

## Signing

Exactly the alerts' recipe, because a receiver that already verifies an alert
must not need a second implementation to verify a message event.

Every request is:

```
POST /hooks/mail HTTP/1.0
Content-Type: application/json
X-hMailServer-Timestamp: 1789000042
X-hMailServer-Signature: sha256=<hex>
X-hMailServer-Event: message.bounced
X-hMailServer-Delivery: 4193
X-hMailServer-Attempt: 1
User-Agent: hMailServer
```

`X-hMailServer-Delivery` is the queue row's id and is the same on every attempt,
which is how a receiver recognises a retry of something it has already acted on.
`X-hMailServer-Attempt` says which attempt this is.

The signature is **HMAC-SHA256 over the timestamp, a full stop, and the raw
request body**, under the subscription's secret, as lower-case hex. Verify it
against the bytes you received, before parsing them:

```python
import hmac, hashlib, time

def verify(secret, headers, raw_body):
    timestamp = headers["X-hMailServer-Timestamp"]
    signature = headers["X-hMailServer-Signature"]

    # Refuse anything older than five minutes, or the signature can be replayed.
    if abs(time.time() - int(timestamp)) > 300:
        return False

    expected = hmac.new(secret.encode(), (timestamp + ".").encode() + raw_body,
                        hashlib.sha256).hexdigest()

    return hmac.compare_digest("sha256=" + expected, signature)
```

```javascript
const crypto = require("crypto");

function verify(secret, headers, rawBody) {
   const timestamp = headers["x-hmailserver-timestamp"];
   const signature = headers["x-hmailserver-signature"];

   if (Math.abs(Date.now() / 1000 - Number(timestamp)) > 300) return false;

   const expected = "sha256=" + crypto.createHmac("sha256", secret)
      .update(timestamp + ".").update(rawBody).digest("hex");

   return crypto.timingSafeEqual(Buffer.from(expected), Buffer.from(signature));
}
```

Compare in constant time, and check the timestamp: a signature with no freshness
check can be replayed for ever.

The body is sent byte for byte as it was stored when the event was raised. A
retry is the same bytes with a fresh timestamp and therefore a fresh signature —
so a receiver de-duplicating on `X-hMailServer-Delivery` never sees two
different documents under one identity.

---

## The queue, the retries and the dead letter

An event becomes a row in `hm_webhookqueue` holding the body as it will be sent.
It is a table and not a list in memory because a restart between the message
arriving and the receiver being told must not lose the second — and because it
is what lets a receiver that was down for ten minutes get the ten minutes it
missed.

`WebhookTask` runs once a minute and is the only thing that sends. A pass is
bounded three ways, because it runs on the scheduler's one thread - the thread
the backup, the alert pass, log retention and every other scheduled task share:

- it attempts at most **200** rows;
- it runs for at most **60 seconds**, after which what is left stays due for
  the next pass;
- an endpoint that gave **no answer at all** - a refused connection, a name
  that does not resolve, ten seconds without a response - is not tried again
  in the same pass. Its remaining rows are left as they are, untouched and
  still due, so an endpoint that is down costs one deadline a pass and not one
  per queued row. A `4xx` or `5xx` is an answer, and does not stop the pass:
  the endpoint is up, and the next row may be one it accepts.

Only one pass runs at a time. `POST /api/v1/webhooks/run` on a REST worker and
the scheduled pass on the scheduler thread would otherwise read the same due
rows and each POST them before either recorded the outcome; a receiver would
get two signed copies under one `X-hMailServer-Delivery` id. A pass asked for
while one is running waits for it and then makes its own, so `/run`'s
`delivered` counts what *that* pass sent - which may be nothing, if the
scheduled pass finished the queue a moment before.

A delivery succeeds on any 2xx. Anything else — a 4xx, a 5xx, a refused
connection, a name that does not resolve, a request that has taken more than ten
seconds — is a failure, and the waits between attempts are:

| Attempt | Wait before it |
|---|---|
| 2 | none: the next pass |
| 3 | 1 minute |
| 4 | 5 minutes |
| 5 | 15 minutes |
| 6 and after | 60 minutes |

After `WebhookMaxAttempts` (5 by default) the row is **dead-lettered**: it keeps
the body, the number of attempts and the last failure, it stops being retried,
and the administrator is told once in the error log as **HM6971**. Read it at
`GET /api/v1/webhooks/deliveries?state=dead-lettered`, in the Deck's Webhooks
page, or with `hmctl webhooks deliveries --state dead-lettered`.

A subscription switched off while something of its is queued **holds** it rather
than losing it: switching a subscription off is not the same as saying its
receiver should never hear what it missed, and a pass after it is switched on
again makes the attempt. Deleting the subscription does remove them, because a
delivery to an endpoint that no longer exists is a request nobody wants made.

Delivered and dead-lettered rows are removed after `WebhookRetentionDays` (7 by
default). 0 keeps everything, which for a per-message stream is a decision
rather than a default.

### The rate ceiling

Each subscription has a ceiling per minute — its own `max_per_minute`, or the
server's `WebhookMaxPerMinute` (60) when it names none. Past it, events are
**dropped**, counted and said once in the application log; they are not queued.

That is deliberate. A queue that grows without bound is how a busy hour becomes
a flood an hour later, and a receiver that cannot keep up is better told that it
fell behind than told about it all afternoon. The count is in the listing as
`dropped`; it is kept in memory, so it counts from the last time the server
started.

If `dropped` is not zero, either the ceiling is too low or the subscription is
too broad. Narrow what it subscribes to before raising the ceiling.

Note that the ceiling is one count per subscription across every event it
takes, and some events are driven by traffic from outside: `message.received`
by whoever writes to you, `account.locked` by whoever fails to log in.
`account.locked` is raised only for a name this server actually hosts, so an
outsider spraying invented names cannot spend it - but a real account can be
locked by an outsider, and a burst of inbound mail is a burst of
`message.received`. A subscription that must not miss a bounce should
therefore not also subscribe to `message.received` or `account.locked`; give
those their own subscription with their own ceiling.

---

## Settings

Four rows in `hm_settings` — none of them in `hMailServer.ini`.

| Setting | REST key | Default | What it does |
|---|---|---|---|
| `WebhooksEnabled` | `webhooks_enabled` | on | The master switch. It opens nothing on its own; it is what silences every receiver at once without deleting the subscriptions that name them. It does not silence the inbound webhooks ([InboundWebhooks.md](InboundWebhooks.md)), each of whose routes is on or off by itself |
| `WebhookMaxAttempts` | `webhook_max_attempts` | 5 | Attempts before a delivery is dead-lettered - and before an inbound webhook's message is kept or returned to its sender, as its route says. 1 to 20 |
| `WebhookMaxPerMinute` | `webhook_max_per_minute` | 60 | The ceiling for a subscription that names none of its own. 1 to 100000 |
| `WebhookRetentionDays` | `webhook_retention_days` | 7 | Days of delivered and dead-lettered rows to keep - and of an inbound webhook's delivered and returned ones; its kept messages are never swept. 0 keeps everything. 0 to 3650 |

On COM they are `Settings.WebhooksEnabled`, `.WebhookMaxAttempts`,
`.WebhookMaxPerMinute` and `.WebhookRetentionDays`, and the Control Panel edits
them through those properties on Monitoring & troubleshooting > Logging, the
Alerts & webhooks tab. A value outside a range is **refused** with the same
sentence over REST and over COM, not clamped.

---

## The routes

| Route | What it does |
|---|---|
| `GET /api/v1/webhooks` | the subscriptions, and the event names this build raises |
| `POST /api/v1/webhooks` | create one |
| `GET /api/v1/webhooks/{name}` | one of them |
| `PUT /api/v1/webhooks/{name}` | change one; a field left out keeps its value |
| `DELETE /api/v1/webhooks/{name}` | remove it, and everything of its still queued |
| `GET /api/v1/webhooks/deliveries` | the queue, newest first; `webhook`, `state` and `limit` |
| `POST /api/v1/webhooks/run` | one delivery pass now |
| `POST /api/v1/webhooks/test` | raise an event on request |

`POST /test` raises a real event through exactly the path a real one takes — the
same matching, the same domain switches, the same ceiling — so what arrives is
what would arrive. It answers how many subscriptions took it rather than
pretending one did, so `"queued": 0` means something: nothing subscribes to that
event, the domain has webhooks off, or every match is at its ceiling.

**No API key reaches any of these routes**, whatever its scope, and the refusal
is a 401 so a key learns nothing from asking. A subscription holds a signing
secret and the address this server will POST to: a key that could read one could
forge bodies a receiver would accept, and a key that could write one could point
the whole mail stream at an endpoint of its own. The administrator password
reaches them, and so does an account whose role holds `view_configuration` (to
read), `manage_settings` (to delete one) or `manage_server` (to create or
change one, raise a test or run a pass — everything that makes the server
connect to an endpoint; see *Where an endpoint may be*) — and a role confined to
named domains is refused all eight as server-wide, since a subscription reaches
every domain it is not confined to.

---

## The other surfaces

**The Control Deck** has a Webhooks page: the subscriptions with their queue in
words, the last twenty-five deliveries with the reason a pending one has not
arrived, an editor built from the route's own schema, and two buttons — raise a
test event, run a pass now. In all twenty languages.

**The desktop Control Panel** has the four settings on its Logging page, the
Alerts & webhooks tab, and a domain's two switches, `webhooks_enabled` and
`webhook_subjects`, on the domain dialog's Webhooks tab. The subscriptions and
their deliveries are not in it.

**`hmctl`**:

```
hmctl webhook list
hmctl webhook create --set name=ticketing --set url=https://tickets.example/hook \
                     --set-string secret=... --set events=message.bounced
hmctl webhook update ticketing --set enabled=false
hmctl webhook delete ticketing
hmctl webhooks deliveries --state dead-lettered
hmctl webhooks run
hmctl webhooks test --event message.bounced --domain example.com
```

**PowerShell**: `Get-HmWebhook`, `New-HmWebhook`, `Set-HmWebhook`,
`Remove-HmWebhook`, `Get-HmWebhookDelivery`, `Invoke-HmWebhookDelivery`,
`Test-HmWebhook`. `Set-HmWebhook` sends only the parameters it was given, so
switching a subscription off cannot clear what it signs with.

**COM**: `Utilities.GetWebhookSubscriptions(name)` and
`Utilities.SaveWebhookSubscription(json)` take and answer the same JSON
documents as the routes, with `"remove": true` to delete one. They are JSON
rather than a collection for the reason the suppression list is: a collection
would be two new interfaces and two new coclasses, and registering those needs
an elevated step this feature does not otherwise need. Both are the server
administrator's.

---

## What this does not do

- **It does not carry the message.** See the top of this document.
- **It does not post mail.** Nothing here hands another program a message; an
  address or a domain whose mail is posted to a URL is an inbound webhook
  ([InboundWebhooks.md](InboundWebhooks.md)), which signs with this recipe and
  retries on this schedule. Nothing here lets another program tell this server to
  do something either; that is the REST API, which is authenticated.
- **There is no per-subscription retry policy.** The ladder and the attempt
  count are the server's, and the attempt count is a setting for the whole
  server rather than for one subscription.
- **There is no signature overlap during a secret rotation.** One secret at a
  time; accept both at the receiver while you change it.
- **One pass at a time is per process, not per database.** Two nodes sharing
  one database would each run a pass against the same queue; a claim step in
  the table is what an active-active pair would need, and it is not built.
- **A subscription is not in the configuration backup.** `BackupExecuter` writes
  what the business objects know how to write, and `hm_webhooks` has none — the
  alert rules are in the same position. Export them with `hmctl webhook list`
  before a restore, and note that the secrets are not in that export because
  they are not in any read.
- **The ceiling forgets a restart.** It is a question about a rate, kept in
  memory; a restart forgives whatever the current minute had used, which is at
  most one minute's worth.
- **An event is raised per recipient, not per message.** A message to four
  people at one domain raises four `message.received` events, because a
  subscription can be confined to one address and each of those four is a thing
  that happened to a mailbox.
- **The delivery pass is once a minute.** That is the scheduler's finest tick. An
  administrator who does not want to wait uses `POST /api/v1/webhooks/run`, the
  Deck's button or `hmctl webhooks run`.

---

## Where it lives

| | |
|---|---|
| Server | `hmailserver/source/Server/Common/Util/WebhookManager.h` and `.cpp` |
| Schema | `hm_webhooks` and `hm_webhookqueue`, and two `hm_domains` columns, from 6050 |
| REST | `hmailserver/source/Server/Common/Util/RestApiWebhooks.cpp` |
| Deck | the Webhooks view in `hmailserver/installation/WebAdmin/index.html` |
| Tests | `hmailserver/test/RegressionTests/API/RestApiWebhooks.cs` |

The signing and the backoff ladder are `AlertManager`'s, called rather than
copied — see [AlertsAndAuditTrail.md](AlertsAndAuditTrail.md), which covers the
*other* webhook this server has: the one that belongs to an operational alert
rule rather than to a mail event.
