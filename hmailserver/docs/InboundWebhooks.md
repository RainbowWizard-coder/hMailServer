# Inbound webhooks

**Mail in, to a webhook: an address, or a whole domain, whose mail this server
posts to a URL.**

SendGrid and Mailgun call it inbound parse, and it is what an application
actually wants a mail server for: a helpdesk that opens a ticket from what
`support@` receives, a CRM that files what `orders@` is sent, a service that
reads the replies to `reply+<token>@`. Until this existed the only way to have it
was a rule that ran a script. The outbound webhooks ([Webhooks.md](Webhooks.md))
tell another program what this server's mail *did*; this hands it the mail.

Nothing here is on until a route is made. With none, a message's recipients are
looked at once for the shape a route's recipient has - no account behind it, at
a domain this server hosts - which nearly every message lacks; the routes table
is read at most twice a minute while mail for a hosted domain arrives, and finds
nothing; and the minute's task is two selects that find nothing, the queue's due
rows and the retention sweep's count, as the outbound webhooks' is.

There is no COM member and no Control Panel page: the routes are made over the
REST API and on the Control Deck, as the single sign-on settings are.

---

## A route

```
POST /api/v1/inbound-webhooks
{
  "address": "support@example.com",
  "url": "https://helpdesk.example/hooks/mail",
  "secret": "a-long-random-string"
}
```

| Field | What it is | Default |
|---|---|---|
| `address` | `user@example.com`, or `example.com` for every address of the domain. The domain must be one this server hosts. Stored lower case; it is the route's name in the path | required |
| `url` | `https://` to the endpoint; plain `http://` only to this machine. No space, line break or other control character - percent-encode them. See *Where an endpoint may be* | required |
| `secret` | **write-only**. Every request is signed under it; it is never returned, and it is protected at rest as an outbound webhook's secret is | required |
| `deliver` | `false`: the route alone receives the address's mail. `true`: what the address reaches without the route - its mailbox, the alias's target, the list - receives it as well | `false` |
| `on_failure` | `keep` or `bounce`: what a message the endpoint never accepted becomes. See *The queue* | `keep` |
| `format` | `form`: `multipart/form-data` in SendGrid's field names. `json`: one JSON document | `form` |
| `attachments` | `include`: carried as files. `names`: listed with their names, types and sizes, without their bytes. `none`: left out, and counted | `include` |
| `include_raw` | `true` sends the message as it arrived as well | `false` |
| `max_size_kb` | A message larger than this is not posted; it is kept or returned, as `on_failure` says. At most `81920` - the 80 MB the message parser takes, and so the most any route can make a body of - and `0` is no ceiling of the route's own, which is then 81920. The server's own message size limit applies as well | `10240` |
| `enabled` | A route switched off is as though it were not there for new mail; what its queue holds waits for it to be switched on again | `true` |

The answer adds `secret_set`, `private_network` (see below), `created`, and how
many of the route's deliveries are `pending`, `delivered`, `kept` and `bounced`.
The URL is answered whole only to a credential holding `manage_inbound_webhooks`;
any other reader is answered its scheme and host, `https://helpdesk.example/...`,
since a receiver's URL often carries a token of its own (see *Who*).

### Which route an address takes

An address route wins over its domain's. A recipient written with a plus tag
(`reply+1234@example.com`) is found by the address as it was written first, then
by the address before the tag (as the domain's own plus addressing reads it),
then by the domain - and the tag is kept in what is posted, which is usually the
point of it. A route that is switched off is passed over: an address whose own
route is off goes to its domain's route when that is on, and is what it would be
with no route at all when it is not.

A route decides that the address exists. RCPT TO is accepted for a routed
address whether or not an account, an alias or a list has it, and a route is
asked before any of them - a managed list's address and its `-subscribe`,
`-request` ... addresses included. So:

* a route with `deliver` off takes the address's mail **instead** of whatever has
  the address: its mailbox, its alias, its list. A list whose address a route
  takes never sees the message - nor, so, applies its posting rules to it: a
  domain route that posts only takes an announcement-only list's mail as well,
  from anybody. Route a list's domain with care, or route the list's addresses
  you mean;
* a route with `deliver` on posts the message **and** delivers it to whatever the
  address would have reached without the route - its account, the alias's
  target, the list's members (a managed list distributes it itself), the domain's
  catch-all - and when that is nothing, the route alone has it. What the address
  reaches is decided as RCPT TO decides it: a list's posting rules refuse a
  sender at RCPT TO with the route as without it, and a posting the list's mode
  refuses where the list has a moderator goes to the moderator and not to the
  members;
* an alias or a list whose member is a routed address reaches the route too;
* a message is posted to a route once. Where what one route's address reaches
  leads through an alias to an address of another route, that route takes it as
  well - once - and where it leads back to a route that has it, the address is
  delivered as though it had no route.

A message to several addresses of one route is posted once, with every one of
them in `envelope.to`. A message to addresses of two routes is posted once to
each.

---

## What arrives

A `POST` to the URL, signed (see *Signing*):

```
POST /hooks/mail HTTP/1.0
Content-Type: multipart/form-data; boundary=hMailServer-4193-...
X-hMailServer-Timestamp: 1789000042
X-hMailServer-Signature: sha256=<hex>
X-hMailServer-Event: message.inbound
X-hMailServer-Delivery: 4193
X-hMailServer-Attempt: 1
```

`X-hMailServer-Delivery` is the queue row's id and is the same on every attempt;
together with `X-hMailServer-Event` it tells a receiver a retry from a second
message. The ids are this queue's own, and an outbound webhook's delivery can
carry the same number: a receiver that takes both de-duplicates on the pair.

### `format: form`

`multipart/form-data`, in the field names SendGrid's Inbound Parse posts, so
that a receiver written for it reads this one. The fields marked *ours* are this
server's own, beside SendGrid's; a receiver written for SendGrid ignores them.

| Field | What it holds |
|---|---|
| `headers` | the header block as it arrived |
| `from`, `to`, `cc`, `subject` | the header fields, decoded to UTF-8 (`cc` only where there is one) |
| `text`, `html` | the plain and the HTML body, decoded to UTF-8, where the message has them |
| `envelope` | JSON: `{"to":["support@example.com"],"from":"someone@example.net"}` - the route's share of the envelope recipients, and the envelope sender |
| `sender_ip` | the address that connected |
| `spam` *ours* | `true` or `false`: whether this server classified the message as spam when it arrived. SendGrid sends a `spam_report` instead, which this server does not |
| `spam_score` | the score, only for a message classified as spam and only while the server writes its reason headers (`AddHeaderReason`), so that it is the server's figure and not one a sender wrote |
| `SPF`, `dkim`, `dmarc` | the results this server wrote into its own `Authentication-Results` field - `pass`, `fail`, `none`, ... - and only while `AuthenticationResultsEnabled` is on; a field another host wrote, or a sender wrote in this server's name, is never read. Each is one bare word: `dkim` is `pass` when any signature passed, where SendGrid writes `{@example.net : pass}` per signing domain. `dmarc` is *ours* |
| `charsets` | JSON: every text field is `UTF-8` |
| `attachments` | how many attachments are carried or named - with `attachments: none`, 0 |
| `attachments_left_out` *ours* | how many a route with `attachments: none` left out, where it left any out |
| `attachment-info` | JSON, per attachment `attachment1`, `attachment2` ...: `filename`, `name`, `type`, `content-id` where it has one, `size` in bytes - not with `attachments: none` |
| `content-ids` | JSON, from each `Content-ID` to its `attachmentN`, where any attachment has one |
| `attachment1` ... | the attachments as files, with their names and types - `attachments: include` only. A name's quote is written `%22`, its backslash `%5C` and a line break as a space, so the part's own header cannot be broken open |
| `email` | the message as it arrived - `include_raw` only, as SendGrid's raw mode sends it |
| `route` *ours* | the route's address |
| `queue_id` *ours* | the delivery queue's id for the message, the same number the outbound webhooks' `queue_id` carries |

An attachment is what the server counts as one everywhere else: a part with a
disposition of `attachment`, a named inline part, and a part that is neither text
nor multipart.

The boundary is built from the delivery's id and the SHA-256 of the message,
which the message cannot contain, so a sender cannot write a part that ends
another one; and the body is built the same way on every attempt.

### `format: json`

```json
{
  "event": "message.inbound",
  "route": "support@example.com",
  "delivery": 4193,
  "time": 1789000040,
  "time_utc": "2026-09-16 08:00:40Z",
  "server": "mail.example.com",
  "queue_id": 4711,
  "envelope": {"to": ["support@example.com"], "from": "someone@example.net"},
  "sender_ip": "192.0.2.10",
  "from": "Someone <someone@example.net>",
  "to": "support@example.com",
  "subject": "My order has not arrived",
  "headers": "Received: ...",
  "text": "...",
  "html": "...",
  "verdicts": {"spam": false, "spf": "pass", "dkim": "pass", "dmarc": "pass"},
  "attachments": [
    {"filename": "receipt.pdf", "type": "application/pdf", "size": 48213, "content": "<base64>"}
  ]
}
```

`time` is when the message was taken, not when the attempt was made, so the body
is the same bytes on every attempt. `cc`, `spam_score`, `spf`, `dkim`, `dmarc`
and an attachment's `content_id` are there only where they have a value;
`content` only with `attachments: include`, and `attachments_left_out` counts
what `attachments: none` left out. With `include_raw` the message as it arrived
is `raw`, in base64, because its bytes are whatever the sender wrote and a JSON
string holds only UTF-8. `headers` is the header block with any byte that is not
UTF-8 written as `?`. The JSON form is this server's own shape.

---

## Signing

Exactly the outbound webhooks' recipe, called rather than copied: HMAC-SHA256
over the timestamp, a full stop and the raw request body, under the route's
secret, as lower-case hex. [Webhooks.md](Webhooks.md) has the verification in
Python and JavaScript, and a receiver that verifies one verifies the other.
Verify against the bytes received, before parsing them, compare in constant time,
and refuse an old timestamp.

The secret can be set and replaced and is never handed back; an empty `secret`
in a change keeps the one that is set. To rotate it, `PUT` the new one and accept
either signature at the receiver while the change is made: the next request is
signed under the new one.

---

## The queue, the retries and what is kept

A message for a route is taken off the message where the delivery path meets it
- after the global rules, the virus scan and the sandbox have had it, and before
the list manager and local delivery, the order RCPT TO asks in - and becomes a
row of `hm_inboundhookqueue` with a copy of the message made through the message
store: a file beside the delivery queue's, which the store always reads as the
bytes it holds. Nothing is posted from the delivery thread. A drain is posted to
the server's asynchronous work queue as the message is queued, so an endpoint
hears of it in seconds, and the scheduled pass runs every minute for the retries.

One pass runs at a time, and nothing ever waits for one: a drain, the minute's
task and `POST /run` each find a pass running and go, and the running pass posts
a drain when it ends for whatever was queued meanwhile. At most one drain is on
the asynchronous queue at a time, as a task that may block - held below the
queue's size, so that an endpoint that does not answer can never take the
threads the server finishes an SMTP message on.

A pass is bounded as the outbound one is - at most 50 due rows read and 60
seconds - and reads only the rows of routes that are on: however many a route
switched off holds, they never stand in front of another route's. An endpoint
that gave no answer at all - a refused connection, a name that does not resolve,
a request its 30 seconds ran out on - is tried once in the pass, and its other
due rows wait for its next attempt, a minute at the least, without an attempt
counted: an endpoint that is down costs one deadline a pass and holds up only
itself. Any `2xx` delivers - an answer is read to its first 16 KB and judged by
its status line, so an endpoint that took the message and said so at length has
taken it - and anything else is a failure: a `4xx`, a `5xx`, a `3xx` (a redirect
is never followed).

The retries **are the outbound webhooks'**: `WebhookMaxAttempts` attempts (5 by
default), with the waits 0, 1, 5, 15 and then 60 minutes between them. There is
no setting of this feature's own.

When the attempts run out, the route's `on_failure` decides:

* **`keep`** - the row is **kept**, with the message: it can be read
  (`GET .../deliveries/{id}/message`), tried again (`POST .../deliveries/{id}/retry`,
  which starts the attempts again from none) or removed with its message
  (`DELETE .../deliveries/{id}`), and it is never swept. The administrator is
  told once, in the error log, as **HM7401**.
* **`bounce`** - the message is **returned to its sender** with the same
  delivery-status report every other failed delivery sends, one `4.4.7` record
  per recipient saying that the application that receives the address's mail did
  not accept it - except for a recipient whose sender asked, with DSN
  `NOTIFY=NEVER` or a `NOTIFY` without `FAILURE`, not to be told; the copy is
  removed. What the endpoint answered is not repeated to the sender, who has no
  business knowing where this server passes the mail on. Where the sender cannot
  be told - an empty sender, a mailer-daemon, a message marked `Auto-Submitted` -
  the message is kept instead (HM7401 says so). **HM7402** records the return.

A copy that is missing or no longer parses as a message is nothing a retry could
change: the route's `on_failure` decides at once, and **HM7404** says why - the
endpoint never saw it. A returning route returns it on its sender alone, as any
failed delivery whose message cannot be read is returned; a copy that is gone
leaves a kept row that says there is nothing to read or retry.

A message larger than the route's ceiling is not posted at all: with `bounce` it
is returned at once (`5.3.4`) - where the route delivers as well, only for an
address that reached nothing else, since the mailbox still receives it - and
with `keep` it is kept at once (**HM7403**).

A delivered row, and a returned one, are removed after `WebhookRetentionDays`
(7 by default); a kept one is not. Removing a route removes its queue and every
message the queue kept. A route switched off holds what its queue has until it is
switched on again.

The rows survive a restart: a message the server took is posted after it.

---

## Where an endpoint may be

The endpoint is reached **from this server**, so where it may be depends on who
made the route - and the server records which, as `private_network`:

* **The server's administrator** - the administrator password, or an account
  whose role holds `manage_server` and reaches every domain - may name an address
  only this server can reach, as for an outbound webhook: a receiver on the same
  network is the common case. But never a **link-local or cloud metadata
  address**: `169.254.0.0/16` and `fe80::/10`, where the clouds answer their
  instance metadata, and `100.100.100.200`, `168.63.129.16`, `192.0.0.192` and
  `fd00:ec2::254`, the ones they keep outside those ranges, with an IPv4 address
  carried inside an IPv6 one - mapped, compatible, NAT64 or 6to4 - judged as the
  IPv4. The metadata service hands out the machine's own credentials, and a
  request this server makes on somebody's say-so must not be a way to them.
* **Every other credential** - an **API key** of any scope or role, and an
  account whose role is not the server's administrator - names a **public
  address** only: loopback, the private ranges, carrier-grade NAT, link-local,
  the documentation and reserved ranges are all refused. An application's key
  cannot make this server a way into the network it sits on, and the refusal
  names the host and not what it resolved to, so a key cannot map the inside of
  the network one name at a time.

Both are judged twice: when the route is saved, for a refusal the caller sees,
and at **every connection**, by the addresses the name resolves to then, the
connection being made to the addresses that passed - so a name that resolves
somewhere harmless when the route is made and to the metadata address later
gains nothing. A route saved by anybody but the server's administrator is held to
public addresses from then on, even one the administrator made first.

No redirect is followed. Plain `http` is accepted only to this machine: the body
is the message itself. The request is made directly, never through `HttpProxy`,
which would resolve the name out of sight of the check - an endpoint this server
can reach only through a proxy cannot be a route's.

---

## Who

| To | Needs |
|---|---|
| Read the routes and the queue | `view_configuration` - a route's URL whole only beside `manage_inbound_webhooks` |
| Read a kept message | `view_configuration` and `view_mailbox`: it is the mail itself |
| Make, change or remove a route; retry or remove a delivery | `manage_inbound_webhooks` |
| Run a pass now | `manage_inbound_webhooks`, and a credential that reaches every domain |

`manage_inbound_webhooks` ([Roles.md](Roles.md)) is appended to the vocabulary.
The server administrator holds it, as it holds everything; no other built-in role
does, since routing a domain's mail to a URL is as much as reading all of it. It
is meant for an **API key's role**: an application that receives mail is given a
key whose role holds `manage_inbound_webhooks` (and `view_configuration`, to read
its routes back), confined to its domain, and it routes that domain's addresses
and no other. A route is judged in its own domain: a credential confined to named
domains is listed only their routes and deliveries - the queue is read for its
own routes, so another domain's busier queue never fills its page - and a
delivery of a route outside them is the same `404` as none.

A reader without `manage_inbound_webhooks` - a domain's administrator, the auditor
- is answered a route's URL as its scheme and host alone, `https://helpdesk.example/...`:
a receiver's URL very often carries a token in its path or its query, and the
whole of it is for whoever may change the route. Put a token in the secret, not
in the URL, where you can.

---

## The audit trail

A route made, changed or removed is recorded under whoever did it, as an
**inbound webhook** named by its address - a removal too - with its secret and
its URL recorded as `(changed)`, never by their values. The queue is traffic and
is not; but a kept message **downloaded**, **retried** or **removed** is recorded
as an event - it is somebody's mail - with the delivery, the message's queue id,
its sender and its recipients.

Nothing secret reaches a log: not the secret, not the URL's path or query, not
the message. The log lines name the route by its address; a failure to connect
names the endpoint's host, as every other HTTP failure the server records does.

---

## The routes

| Route | What it does |
|---|---|
| `GET /api/v1/inbound-webhooks` | the routes |
| `POST /api/v1/inbound-webhooks` | make one |
| `GET /api/v1/inbound-webhooks/{address}` | one route |
| `PUT /api/v1/inbound-webhooks/{address}` | change it; a field left out keeps its value, and the address cannot change |
| `DELETE /api/v1/inbound-webhooks/{address}` | remove it, its queue and what the queue kept |
| `GET /api/v1/inbound-webhooks/deliveries` | the queue, newest first; `route`, `state` (`pending`, `delivered`, `kept`, `bounced`) and `limit` |
| `POST /api/v1/inbound-webhooks/deliveries/{id}/retry` | a kept delivery tried again from no attempts, or a pending one now |
| `DELETE /api/v1/inbound-webhooks/deliveries/{id}` | a delivery removed, with the message it kept |
| `GET /api/v1/inbound-webhooks/deliveries/{id}/message` | the message it kept, as `message/rfc822` |
| `POST /api/v1/inbound-webhooks/run` | one pass now: `delivered`, `removed`, and `running` when a pass was running already and this one was not made |

A refused field is a `400` with the reason; an address that has a route, a `409`;
a domain the credential does not reach, a `403`.

## The Control Deck

**Inbound webhooks** lists the routes - the address, whether it posts only or
posts and delivers, the URL and the queue in words - and the last twenty-five
deliveries with the reason one has not arrived. A kept message has **Download**
(beside `view_mailbox`), and for a credential holding `manage_inbound_webhooks`
**Retry** and **Remove**. The editor is built from the route's own schema, its
defaults read from the create route's words. A credential confined to named
domains sees its domains' routes and has no **Run a delivery pass**.

---

## In the error log

| Code | When |
|---|---|
| HM7400 | A message could not be copied or queued for a route. The recipient stays in the delivery queue and the message is tried again on its retry schedule; when the retries run out its sender is told (`4.3.0`) |
| HM7401 | A message the endpoint never accepted is kept - or, for a route that returns, its sender could not be told |
| HM7402 | A message the endpoint never accepted has been returned to its sender, or - where the sender asked with DSN `NOTIFY` not to be told - would have been |
| HM7403 | A message larger than a keeping route's ceiling was kept without being posted |
| HM7404 | The copy a delivery kept is missing or cannot be read as a message: the endpoint never saw it, and the route's `on_failure` has decided at once - kept, or returned to its sender |
| HM7405 | The routes could not be read at all when a message had a recipient that may be a route's; that recipient stays in the delivery queue and the message is tried again, as for HM7400 |
| HM7406 | A routed recipient could not be taken off the delivery queue |
| HM7407 | A route that delivers as well could not write the mailbox's recipient to the delivery queue; it is delivered in this pass, and not if the message is tried again |
| HM7408 | A delivered message's copy could not be removed |
| HM7409 | A domain was deleted and its routes could not be; a domain made again under the name would find them |

None of them is raised on a server with no route.

---

## What this does not do

* **No COM and no Control Panel.** The REST API and the Deck, as for single
  sign-on.
* **No setting of its own.** The attempts and the retention are the outbound
  webhooks' (`WebhookMaxAttempts`, `WebhookRetentionDays`), and a route is on or
  off by itself. `WebhooksEnabled` silences the outbound webhooks and not these.
* **No redirect is followed, and no proxy is used.** See *Where an endpoint may be*.
* **No per-route retry ladder**, and no signature overlap during a secret
  rotation - as for the outbound webhooks.
* **One post per route per message**, with the route's recipients in
  `envelope.to`; not one per recipient.
* **A route changed between a message's acceptance and its delivery** - a window
  of seconds, or the length of a hold: switched off or removed, the address is
  what it would be with no route - its mailbox, where it has one, and otherwise
  returned to its sender as an unknown address (`5.1.1`). It is never sent on as
  though the address were somewhere else. A route whose `deliver` is changed in
  that window is asked as it is at delivery.
* **Not in the configuration backup**, as the outbound webhooks are not: export
  the routes with `GET /api/v1/inbound-webhooks`; the secrets are in no read. A
  restore leaves the routes where they are, and a domain deleted any other way
  takes its routes with it.
* **Mailgun's field names** (`body-plain`, `stripped-text` ...) are not sent;
  SendGrid's are, beside the few marked as this server's own, and the JSON form
  is this server's own.
* **One pass at a time is per process**, as for the outbound webhooks.

---

## Tests

`hmailserver/test/RegressionTests/API/InboundWebhooks.cs`, which the Windows suite
and the Linux one both run, on ports 10180 to 10189:

* a message sent over SMTP to a routed address arrives at a local endpoint as
  SendGrid's form - the fields, an attachment as a file, the envelope, the
  signature recomputed from the two headers and the raw body - posted once, and
  nothing reaches the mailbox the address has; a secret changed signs the next
  message and the old one does not, and an empty one in a change keeps it;
* a route that delivers as well does both; a domain route takes an address no
  account has; an address route switched off leaves its address to the domain's
  route; a plus tag is found by its address and kept;
* the JSON form with the attachment in base64 and the raw message; the
  names-only and the none attachment policies, the latter with the raw message
  as `email`, `cc`, `queue_id` and `sender_ip`; the text fields decoded to UTF-8
  from an encoded word and Latin-1, a forged `Authentication-Results` in this
  server's name and a forged reason score carried as no verdict at all, and an
  attachment's name with backslashes in it;
* an endpoint that refuses is retried and then kept, the same body on every
  attempt, HM7401 reported, the message downloaded and retried to delivery, the
  download and the retry on the audit trail, the delivery removed; a returning
  route's sender gets the report, without the endpoint's answer or address, and
  a sender who asked with `NOTIFY=NEVER` does not (HM7402); a message over the
  ceiling is returned, and over the ceiling of a route that delivers as well
  reaches the mailbox with no report;
* an answer of 40 KB with a `200` is an acceptance, posted once;
* an endpoint that takes the connection and never answers holds up neither the
  server's SMTP answers to as many messages as the asynchronous queue has threads
  and more, nor another route's message; a route switched off with 55 due rows -
  more than a pass reads - holds up no other route;
* the routes' refusals - a domain this server does not host, plain http off this
  machine, an empty secret, a URL with a space or a line break, a ceiling past
  81920 KB, a metadata and a link-local address for the administrator in every
  form listed above, a private address or a name that leads inside for an API
  key, the refusal not saying where the name leads;
* a key whose role holds `manage_inbound_webhooks` confined to one domain making
  a route there and refused another, listed and read only its own, answered the
  same `404` for another domain's delivery and refused the pass the same role
  unconfined may run; a key's change holding an administrator's route to public
  addresses, and refused a new address; a reader without the permission answered
  the URL's scheme and host, and refused a kept message without `view_mailbox`;
  an auditor reading and refused a change; the secret never read back, and a
  route switched off refusing RCPT TO for an address that has nothing else;
* the audit trail naming a route made and removed by its address, and never its
  URL; an erasure taking what the address sent out of the queue; a route going
  with its domain; `WebhooksEnabled` off silencing no route.

`InboundWebhooksLists.cs`, beside it, which only the Windows suite runs (it needs
the COM shape of a distribution list, and an event script): a route that posts
only taking a managed announcement list's mail without the list distributing it;
a route that delivers as well, on an alias of a managed list and on a managed
list's plus-tagged address, letting the list manager distribute it; on a
moderated list, sending the posting to the moderator and not the members; a
route removed while a delivery is held by an `OnDeliveryStart` script leaving
the address unknown (`5.1.1`) and posting nothing.

The Deck's harness (`build/deck-script-test.js`) walks the view: the listing and
the queue in words, Download, Retry and Remove on a kept delivery, the editor's
fields and defaults from the schema, a new route, a change with no secret sent,
a retry, a removal, the pass, a removal of a route, the empty page; a domain's
administrator offered Download and nothing that changes; a role without
`view_mailbox` offered no Download.

Not tested: a name that resolves to a refused address when the connection is made
(the refusals are tested with literal addresses and `localhost`, which are judged
the same way); a redirect answered by an endpoint, which is a failure like any
other answer that is not a `2xx`; the paths that end in HM7400, HM7404, HM7405,
HM7406, HM7407, HM7408 and HM7409, each a failure of the disk or the database; a
keeping route's message over its ceiling (HM7403), whose path is the returning
one's up to the route's choice; a message over 80 MB; the rows surviving a
restart; the secret's protection at rest, which is the outbound webhooks'; the
retention sweep, and the waits of the retry ladder after its first step; the
pass's bound of 60 seconds; a route that delivers as well reached through another
route; that no proxy is used; a list's posting rules refusing a sender at RCPT TO
under a route that delivers as well, which is the list's own check, unchanged.

## Where it lives

| | |
|---|---|
| Server | `hmailserver/source/Server/Common/Util/InboundWebhooks.h` and `.cpp` |
| Recipients | `hmailserver/source/Server/SMTP/RecipientParser.cpp`, and the delivery path in `SMTPDeliverer.cpp`, before `ListManager::TakeListRecipients` |
| The connection's rule | `HttpsClient::RequestNotLinkLocal` and `RequestPublic` |
| REST | `hmailserver/source/Server/Common/Util/RestApiInboundWebhooks.cpp` |
| Schema | `hm_inboundhooks` and `hm_inboundhookqueue`, from 6100 |
| Deck | the Inbound webhooks view in `hmailserver/installation/WebAdmin/index.html` |
| Erasure | `AddressTraceEraser`, through `InboundWebhooks::EraseAddress` |
| A domain deleted | `PersistentDomain::DeleteObject`, through `InboundWebhooks::DeleteRoutesOfDomain` |
