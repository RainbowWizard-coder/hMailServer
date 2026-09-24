# The send API

An application that sends mail through this server - a shop's order
confirmations, a ticketing system's replies, a monitoring probe's alerts - has
until now had one way in: SMTP with an app password. That works, and it tells
the application nothing afterwards. The send API is the other way: an HTTPS
request under an **API key of the application's own**, which may send as **one
sender and no other**, within **ceilings of its own**, and which can ask
afterwards **what became of each message** - delivered, deferred or bounced,
and why - from the message trace the server already keeps.

`POST /api/v1/me/messages` is the webmail's send and stays the webmail's: it
sends as the signed-in person and keeps a copy in their Sent folder. The send
API is for programs.

| Route | What it does |
|---|---|
| `POST /api/v1/send` | Sends a message as the key's sender. `202 {queue_id, message_id, recipients, recorded}` |
| `GET /api/v1/send` | The key's own sends, newest first, paged |
| `GET /api/v1/send/{queue_id}` | One of them, with each recipient's state |

All three answer to an **API key and nothing else** - not to the administrator
password, which names no sender, and not to an account, whose own send is the
webmail's.

---

## The key

A key sends when two things are true of it: its **role holds
`send_through_api`**, and it **names a sender** (`send_from`). Either alone
sends nothing. The permission is the role system's
([Roles.md](Roles.md)); the sender is the key's.

Three fields of the key store (`hMailServerApiKeys.ini`, beside the other
fields of a key), set when the key is made:

| Field | Store line | What it is |
|---|---|---|
| `send_from` | `SendFrom` | The one sender the key may use: an address of a domain this server hosts (`noreply@example.com`), or `*@domain` for any address of that domain (`*@example.com`). Empty - the default, and every key made before this existed - means the key cannot send. |
| `send_per_minute` | `SendPerMinute` | The key's own ceiling on messages a minute. `0`, the default, leaves only the server's ceilings. |
| `send_per_day` | `SendPerDay` | The key's own ceiling on messages in any 24 hours. `0` leaves only the server's. |

They are set on `POST /api/v1/apikeys` (the administrator password's alone, as
every key route is) and in the Control Deck's **API keys** page, which has a
field for each and shows beside every key what it sends as and its ceilings.
The route refuses, with a sentence and `400`:

* a `send_from` that is neither an address nor `*@domain`;
* a `send_from` at a domain this server does not host - a key that sent as
  somebody else's domain would be this server forging their mail. A domain
  alias is not a hosted domain for this purpose: name the domain itself;
* a `send_from` on a key that may not send: a **read-only key** (the auditor,
  which changes nothing), or a key whose role does not hold
  `send_through_api`;
* a `send_from` outside the domains a key is confined to, when it is confined;
* `send_per_minute` or `send_per_day` that is not a whole number, or given
  without a `send_from`.

```
POST /api/v1/apikeys
Authorization: Basic <administrator password>

{"label": "shop confirmations", "scope": "full",
 "send_from": "orders@example.com", "send_per_minute": 30, "send_per_day": 5000}
```

The answer carries the token once, as every key's creation does. A key's
sender cannot be changed afterwards: as with every other field of a key, a new
sender is a new key and the old one revoked.

Read back from the store, the three fail closed: a `SendFrom` that is neither
form reads as empty (the key sends nothing) and a count that is not a whole
number reads as `1`, so a hand-edit cannot widen a key. The desktop Control
Panel's key page does not show them yet; the Deck and the route do.

### The permission

`send_through_api` is a permission of the role system like the others. The
built-in `serveradmin` holds it, as it holds everything, so a key made with
`"scope": "full"` holds it; no other built-in role does, and a **custom role**
may be given it - which is how a key that may send and do nothing else is
made:

```
POST /api/v1/roles
{"name": "mailer", "description": "sends mail", "permissions": "send_through_api"}

POST /api/v1/apikeys
{"label": "shop confirmations", "role": "mailer", "send_from": "orders@example.com"}
```

Take the permission away from the role and every key that holds it stops
sending (`403`, naming the permission) though its sender is still written on
it. A key that held the permission may still read what it sent.

---

## Sending

```
POST /api/v1/send
Authorization: Bearer hmapi_…
Content-Type: application/json

{
  "from": "Example Shop <orders@example.com>",
  "to": ["ann@example.net", "Bob Jones <bob@example.org>"],
  "cc": [], "bcc": [],
  "reply_to": "support@example.com",
  "subject": "Your order 1042",
  "text": "Thank you for your order.",
  "html": "<p>Thank you for your order.</p>",
  "headers": {"X-Order": "1042", "List-Unsubscribe": "<mailto:unsubscribe@example.com>"},
  "attachments": [{"filename": "invoice.pdf", "content_type": "application/pdf", "data": "JVBERi0x…"}]
}
```

The header is the key itself - `Authorization: Bearer` and the token, with
nothing signed over the request: TLS carries it, as every REST credential
travels. With curl:

```
curl https://mail.example.com:8443/api/v1/send \
  -H "Authorization: Bearer hmapi_0123…" \
  -H "Content-Type: application/json" \
  -d '{"to":["ann@example.net"],"subject":"Your order","text":"Thank you."}'
```

| Member | |
|---|---|
| `from` | Optional when the key's sender is an address - it is then the sender - and required when it is `*@domain`. `"address"` or `"Name <address>"`; the address must be the key's sender, or of its domain. The display name is the application's to choose. |
| `to`, `cc`, `bcc` | Arrays of `"address"` or `"Name <address>"` (a string of comma-separated entries is taken too). At least one recipient between them. Bcc goes to the envelope and nowhere else. |
| `reply_to` | Optional. Where replies go. |
| `subject` | Required; it may be empty. |
| `text`, `html` | The two parts; at least one. Both makes a `multipart/alternative`. |
| `headers` | Optional. An object of header fields: `X-` fields - except the server's own `X-hMailServer-` fields, which its rules and its loop detection read - `List-Unsubscribe` and its RFC 8058 companion `List-Unsubscribe-Post`, `In-Reply-To` and `References`. Any other field is `400`: `From`, `To`, `Date`, `Message-ID`, `Received` and the MIME fields are the server's to write. A value is one line of at most 998 characters. |
| `attachments` | Optional. `[{filename, content_type, data}]`, `data` in base64, at most 20 and twelve megabytes together. |

A member the route does not take (`body` for `text`, `reply-to` for
`reply_to`) is `400` and named, rather than dropped: a message sent without
what its sender meant it to carry is worse than a refusal.

### What the server does to the message

It is written and checked by the **same code as the webmail's send**
(`ResolveSendRecipients_`, `WriteComposedMessage_` and `CheckComposedMessage_`
in `RestApiServer.cpp`), so the two cannot come to treat a message
differently:

* **Every recipient** goes through what `RCPT TO` asks of an authenticated
  sender - aliases, distribution lists, routes and the relay rules apply as
  they would - with the server's **recipient ceiling**
  (`MaxSMTPRecipientsInBatch`; over it is `400` with the server's own
  sentence, *too many recipients; the server allows 100*), the **suppression
  list**, and, when the sender is an account's address, that account's
  **restricted delivery**.
* The domain's **signature and disclaimer** are added by the rules SMTP
  submission applies; the **rules that block** are asked (`403`,
  `policy_rejected`, with the rule's text); and the message is held to the
  server's **maximum message size** (`413`).
* The **envelope sender** is the From address, so a bounce comes back to it -
  to its mailbox when it is an account's, or wherever an alias or a catch-all
  takes it; a sender that is neither (a `noreply@` nobody reads) gets no bounce,
  and the send's state is where its application learns of the failure.
* A **Received** field goes first, naming where the request came from, the send
  API and the key's label and id:
  `Received: from [192.0.2.7] (hMailServer send API, key "shop confirmations" 3f9a1c4b5d6e7f80) by MAIL with HTTPS; …`
* It is **DKIM-signed** at delivery by the domain's key, as every message of
  the domain is ([DkimKeyRotation.md](DkimKeyRotation.md)).
* **No Sent copy**, anywhere. An application is not a person and has no Sent
  folder; the webmail's send keeps one because a person expects to find what
  they wrote.
* When the sender is an **account's address**, that account's **sending
  quota** (`[SendingLimits]`, the levels in [SettingLevels.md](SettingLevels.md))
  is consumed as an SMTP submission from it consumes it - one message and its
  recipients - so an application cannot be used to send round an account's
  ceiling.

The compromised-account detection, which judges an account's *session*, is not
asked: there is no session behind a key. The key's own ceilings are its limit.

### The ceilings, and 429

| Ceiling | Whose | Answer |
|---|---|---|
| `send_per_minute` | the key's; a sliding minute, `RateLimiter` key `api-send:<key id>` | `429`, `Retry-After: 60` |
| `send_per_day` | the key's; any 24 hours, on the per-account quota's period machinery (`api-send-day:<key id>`), persisted across a restart | `429`, `Retry-After: 3600` |
| the account's sending quota | the sender account's, when the sender is one | `429`, `Retry-After: 3600`, and the SMTP refusal's sentence |
| the REST request budget | every credential's (200 requests in 10 seconds) | `429`, `Retry-After: 10` |
| the recipient ceiling, the size | the server's | `400`, `413` |

A message is **counted only once every other check has passed**, so a body
refused for a bad header or an unknown recipient costs nothing. `Retry-After`
for the day is when the next hour-long bucket of the window can close, not a
promise that the budget is back by then; the sentence says which ceiling was
reached. The minute is kept in memory, so a restart of the service forgives the
minute it was in; the day is kept in the per-account quota's state file and
survives one. The checks are made one after another - the key's minute, its
day, the account's quota - and a message the minute takes and the day then
refuses has spent its place in the minute.

The answer to a message that is taken is `202`:

```
{"queue_id": 4711, "message_id": "<…@mail.example.com>", "recipients": 2, "recorded": true}
```

`queue_id` is the message's id in the delivery queue - the message trace's
correlation key - and the one to ask about afterwards. `recorded` is false in
the one case where the message was queued and the key's record of it could not
be written (the error log says so, as `HM7180`): it goes, and the key cannot
ask about it.

---

## Asking afterwards

### One send

```
GET /api/v1/send/4711
Authorization: Bearer hmapi_…

{"queue_id": 4711, "message_id": "<…>", "sender": "orders@example.com",
 "recipients": ["ann@example.net", "bob@example.org"], "created": "2026-09-24 10:02:11",
 "state": "bounced", "trace": true,
 "recipients_state": [
   {"address": "ann@example.net", "state": "delivered", "when": "2026-09-24 10:02:13",
    "status": "", "code": 250, "reason": ""},
   {"address": "bob@example.org", "state": "bounced", "when": "2026-09-24 10:02:13",
    "status": "5.1.1", "code": 550, "reason": "550 5.1.1 no such user"}]}
```

Only for a send **this key** made. Any other id - not a number, one nobody
sent, one another key sent - is the same `404`, so a key cannot learn from this
route what other keys have sent.

A recipient's state is **derived from the message trace's events for the
queue id**, and, while the message waits, from the queue itself:

| State | From | `status`, `code`, `reason` |
|---|---|---|
| `queued` | the message is in the queue, the address is still to be tried, and it has not been | empty |
| `deferred` | an attempt failed and will be retried: the remote answered 4xx, did not answer, or the server's own shaping held it (the trace's `deferred` event); or, with the trace off, the queue says it has been tried and is waiting | the last attempt's enhanced status, its code and the remote's own reply - or the server's sentence where the remote said nothing |
| `delivered` | the trace's `delivered` event; `when` is when | `250` |
| `bounced` | the trace's `failed` event: the remote refused it for good, or the retries ran out | the enhanced status and the remote's reply |
| `refused` | the suppression list stopped it at delivery (the trace's `suppressed` event) | what the list's action was |
| `unknown` | the message has left the queue and nothing names the address - see below | why, in a sentence |

The send's own `state` is `deferred` or `queued` while any recipient waits,
and otherwise `bounced` when any bounced, `refused` when any was refused,
`delivered` when at least one was delivered and none failed, or `unknown`.

Three things about the derivation are said as plainly as the table:

* **It needs the trace.** The message trace is **off by default**
  (`MessageTraceEnabled`), because it records who corresponds with whom. With
  it off, a send reads `queued` or `deferred` while it is in the queue and
  `unknown` once it has left it, and `trace` in the answer is `false`. Turn it
  on - and choose its retention - before relying on the answer.
* **An address is reported as the trace names it.** An address the body named
  that resolved to others - an alias, a distribution list, a forward - reads
  `unknown`, and the addresses it resolved to are listed under their own names.
* **A forward or a list's re-send is a new queue id**, as the trace's own
  header says, and is not followed.

The events the states come from are the trace's: since this row the trace
records a **`deferred`** event for every attempt to a remote that ends without
a verdict (a 4xx, no answer, a dropped connection, the server's own shaping) -
followed, when the retries run out, by the `failed` event that gives up - and
the detail of a `failed` or `deferred` delivery is `[<enhanced status>] <what
was said>`: the status the server settled on and the remote's own reply. The
reports count `delivered` and `failed` as before
([Reports.md](Reports.md)); a delivery to several local accounts is now
journalled under the queue id for every recipient, where before only the one
that reused the queue row was.

### The key's sends

```
GET /api/v1/send?page=1&size=50

{"page": 1, "size": 50, "total": 3, "trace": true,
 "sends": [{"queue_id": 4713, "message_id": "<…>", "sender": "orders@example.com",
            "recipients": ["ann@example.net"], "created": "2026-09-24 10:05:40", "state": "delivered"}, …]}
```

Newest first; `size` from 1 to 200 (50 when not given), `page` from 1, and the
listing reaches back ten thousand sends - an older one is asked about by its
queue id. Each entry's `state` is the send's state as above.

The list is **per key**. An administrator's key that holds `send_through_api`
and names no sender - or any key that has sent nothing - reads an empty list:
the **server-wide** view of the same deliveries is the message trace
(`GET /api/v1/message-trace/queue/{id}`, `view_message_trace`), and it stays
permissioned as the server's.

A webhook subscription confined to the sender (`"addresses":
"orders@example.com"`, [Webhooks.md](Webhooks.md)) hears the same deliveries
**pushed**, as `message.delivered` and `message.bounced`, instead of asked
for, for the recipients outside this server - which is where an application's
mail mostly goes. An event about a recipient this server hosts belongs to that
recipient's address, not to the sender's, so a subscription confined to the
sender does not hear it; and no webhook event says `deferred`.

### How long it is kept

The record of a send (`hm_apisends`, schema 6089: the key's id, the queue id,
the Message-ID, the sender, the addresses the body named and when) is kept as
long as the trace it is answered from - **`MessageTraceRetentionDays`**, thirty
by default - and then removed by the hourly pass of the expired-records task;
`0` keeps it. A revoked key's records stay until they age out and answer
nobody.

---

## What this does not do

* **No templates and no substitutions.** The body is the message; an
  application that sends one message per recipient with a name in it makes
  one request per recipient.
* **No scheduled send.** A message goes when it is taken.
* **No per-recipient ceiling of the key's own.** `send_per_minute` and
  `send_per_day` count messages; recipients are held by the server's recipient
  ceiling per message and, for an account's address, by its quota.
* **No change of a key's sender.** A new sender is a new key.
* **No following of a message across a fork.** A forward or a list's re-send is
  a new queue id.
* **No state without the trace** once a message has left the queue: see above.
* **The desktop Control Panel's API key page does not show the three fields**;
  the Deck and the route do.

---

## Where it lives

| | |
|---|---|
| Routes | `hmailserver/source/Server/Common/Util/RestApiSend.cpp` |
| The shared send | `ResolveSendRecipients_`, `WriteComposedMessage_`, `CheckComposedMessage_` in `RestApiServer.cpp` |
| The record and the states | `hmailserver/source/Server/Common/Util/ApiSends.h` and `.cpp` |
| The key fields | `ApiKeyRecord` in `RestApiServer.h`; `LoadKeys_` and `HandleCreateApiKey_` in `RestApiServer.cpp` |
| The permission | `RolePermission::SendThroughApi` in `Common/Application/RoleManager.h` |
| Schema | `hm_apisends`, from 6089 |
| Deck | the API keys view in `hmailserver/installation/WebAdmin/index.html` |
| Tests | `hmailserver/test/RegressionTests/API/RestApiSend.cs`; the Deck's in `build/deck-script-test.js` |
