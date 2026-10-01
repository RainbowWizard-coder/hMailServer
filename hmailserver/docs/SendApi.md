# The send API

An application that sends mail through this server - a shop's order
confirmations, a ticketing system's replies, a monitoring probe's alerts - has
until now had one way in: SMTP with an app password. That works, and it tells
the application nothing afterwards. The send API is the other way: an HTTPS
request under an **API key of the application's own**, which may send as **one
sender and no other**, within **ceilings of its own**, and which can ask
afterwards **what became of each address it sent to** - delivered, deferred or
bounced, and why - from the message trace, **once the trace is on**: it is off
by default, and without it a message that has left the queue reads `unknown`
(see [It needs the trace](#asking-afterwards)).

`POST /api/v1/me/messages` is the webmail's send and stays the webmail's: it
sends as the signed-in person and keeps a copy in their Sent folder. The send
API is for programs.

| Route | What it does |
|---|---|
| `POST /api/v1/send` | Sends a message as the key's sender. `202 {queue_id, message_id, recipients, recorded}` |
| `GET /api/v1/send` | The key's own sends, newest first, paged |
| `GET /api/v1/send/{queue_id}` | One of them, with the state of each address its body named |

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
| `send_per_minute` | `SendPerMinute` | The key's own ceiling on messages a minute. `0`, the default, sets none of the key's own. |
| `send_per_day` | `SendPerDay` | The key's own ceiling on messages in any 24 hours. `0` sets none of the key's own - which only a key whose sender is an **account's address** may have: see below. |

They are set on `POST /api/v1/apikeys` (the administrator password's alone, as
every key route is) and in the Control Deck's **API keys** page, which has a
field for each and shows beside every key what it sends as and its ceilings.
The route refuses, with a sentence and `400`:

* a `send_from` that is neither an address nor `*@domain`, or that holds a
  space, a control character, a line break, `[`, `]`, `=` or `;` - the store
  is read line by line, and a line break in a value would be further lines of
  the key's section (`Scope=full`), a bracket a section of its own;
* a `send_from` at a domain this server does not host - a key that sent as
  somebody else's domain would be this server forging their mail. A domain
  alias is not a hosted domain for this purpose: name the domain itself;
* a `send_from` at a domain that is **switched off**: it sends nothing over
  SMTP, and a key is not a way round;
* a `send_from` on a key that may not send: a **read-only key** (the auditor,
  which changes nothing), or a key whose role does not hold
  `send_through_api`;
* a `send_from` outside the domains a key is confined to, when it is confined;
* a `send_from` that is `*@domain`, or an address that is **no account's**,
  without a `send_per_day` above 0. Such a sender has no account's sending
  quota to hold it, and a key sending as one with no ceiling of its own would
  have none but the listener's request budget - a leaked key sending without
  end. A key whose sender is an account's address is held by that account's
  quota (below) and needs no day ceiling of its own, though it may have one;
* `send_per_minute` (at most 1,000,000) or `send_per_day` (at most
  100,000,000) that is not a whole number in range, or given without a
  `send_from`.

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
Panel's key page does not show them and cannot set them - a key made there
cannot send - though the explanation it writes at the top of a new store
describes them, as the server's does; the Deck and the route show and set them.

### What `*@domain` grants

A key whose sender is `*@domain` is, to this server, **every mailbox of that
domain sending as itself**. Each recipient is judged as `RCPT TO` judges an
authenticated sender's, with the sender the body's `from`, so the key can post
to a members-only list as any member, post to a moderated list as its
moderator (whose own mail is not held for moderation), and send as the
domain's administrator's mailbox. That is what "any address of the domain"
means, and it is why such a key must carry a day ceiling. Give `*@domain` to
an application that is trusted with the whole domain, and an address to any
other.

What a key sends is still the application's, not the account's own: the
message records the account as its submitter for the domain rules, and is
marked as sent through this API (`Message::FlagSubmittedThroughSendApi`), so a
calendar on this server applies no cancellation, answer or update the key
sends in an account's name, and a room or a piece of equipment takes no
invitation it sends as that organiser's ([Calendar.md](Calendar.md), "Who may
change a stored event"; [RoomsAndEquipment.md](RoomsAndEquipment.md), "Who is
asking"). The message is delivered, as any other.

The **display name** and **`reply_to`** are the application's own and are not
checked against anything: `"Your Bank Security" <noreply@example.com>` with a
`Reply-To` elsewhere is a message the key can send. What the key vouches for is
the address.

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
{"label": "shop confirmations", "role": "mailer", "send_from": "orders@example.com",
 "send_per_day": 5000}
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
  "to": ["ann@example.net", "\"Jones, Bob\" <bob@example.org>"],
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
| `to`, `cc`, `bcc` | Arrays of mailboxes, each item **one** `"address"` or `"Name <address>"` - read as `from` is read, so a quoted display name may hold a comma or a semicolon (`"\"Doe, John\" <john@example.net>"`) and nothing may follow the address. A string of comma- or semicolon-separated entries is taken too, and split as the webmail's lists are. At least one recipient between them. Bcc goes to the envelope and nowhere else. |
| `reply_to` | Optional. Where replies go. |
| `subject` | Required; it may be empty. |
| `text`, `html` | The two parts; at least one. Both makes a `multipart/alternative`. |
| `headers` | Optional. An object of header fields: `X-` fields - except the server's own `X-hMailServer-` fields, which its rules and its loop detection read - `List-Unsubscribe` and its RFC 8058 companion `List-Unsubscribe-Post`, `In-Reply-To` and `References`. Any other field is `400`: `From`, `To`, `Date`, `Message-ID`, `Received` and the MIME fields are the server's to write. A value is one line of at most 998 characters. |
| `attachments` | Optional. `[{filename, content_type, data}]`, `data` in base64, at most 20 and twelve megabytes together. |

**A line break is refused** in every value that becomes a header - `from`, each
recipient, `subject`, `reply_to`, a header's value, an attachment's `filename`
and `content_type` - with `400`, so that no value can add a field of its own
(a `Bcc:`) to the message. Only `text` and `html` are multi-line.

A member the route does not take (`body` for `text`, `reply-to` for
`reply_to`) is `400` and named, rather than dropped: a message sent without
what its sender meant it to carry is worse than a refusal.

### What the server does to the message

It is written and checked by the **same code as the webmail's send**
(`ResolveSendRecipients_`, `WriteComposedMessage_` and `CheckComposedMessage_`
in `RestApiServer.cpp`), so the two cannot come to treat a message
differently:

* A sender whose **domain or account is switched off** sends nothing: `403`,
  saying which, as SMTP refuses both. So is a key whose sender was an
  account's address when the key was made and is no longer anybody's, when
  the key has no `send_per_day`.
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
* A **Received** field goes first, naming this server, the send API and the
  key's **label** - and not the key's id, which is its revocation handle and
  its name in the audit trail, nor the address the application called from:
  every recipient, outside this server too, reads this field, and neither is
  theirs to know. The server's log carries both.
  `Received: by MAIL (hMailServer send API, key "shop confirmations") with HTTPS; …`
  A label's quotes, parentheses, backslashes and anything outside printable
  ASCII are written as `_`.
* It is **DKIM-signed** at delivery by the domain's key, as every message of
  the domain is ([DkimKeyRotation.md](DkimKeyRotation.md)).
* It is **journalled** by the journal rules that cover it, as an SMTP
  submission is ([Journaling.md](Journaling.md)): an outbound rule when the
  key's sender is an account here, an inbound rule for each recipient mailbox
  it reaches.
* **No Sent copy**, anywhere. An application is not a person and has no Sent
  folder; the webmail's send keeps one because a person expects to find what
  they wrote.
* When the sender is an **account's address**, that account's **sending
  quota** (`[SendingLimits]`, the levels in [SettingLevels.md](SettingLevels.md))
  is consumed as an SMTP submission from it consumes it - one message and its
  recipients - so an application cannot be used to send round an account's
  ceiling. The webmail's send consumes it too.

The compromised-account detection, which judges an account's *session*, is not
asked: there is no session behind a key. The key's own ceilings are its limit.

### The ceilings, and 429

| Ceiling | Whose | Answer |
|---|---|---|
| `send_per_minute` | the key's; a sliding minute, `RateLimiter` key `api-send:<key id>` | `429`, `Retry-After: 60` |
| `send_per_day` | the key's; any 24 hours, on the per-account quota's period machinery (`api-send-day:<key id>`), kept in its state file | `429`, `Retry-After: 3600` |
| the account's sending quota | the sender account's, when the sender is one | `429`, `Retry-After` one twenty-fourth of the account's period (`3600` for the 24-hour default), and the SMTP refusal's sentence |
| the REST request budget | every credential's (200 requests in 10 seconds) | `429`, `Retry-After: 10` |
| the recipient ceiling, the size | the server's | `400`, `413` |

A message is **counted only once every other check has passed**, so a body
refused for a bad header or an unknown recipient costs nothing. The ceilings
are checked last and in turn - the key's minute, then its day, then the sender
account's quota - and each records the message as it passes; when a later one
refuses, or the message cannot then be queued, **every earlier one is given
back** (`CountedUsage` in `RateLimiter.h`). An application retrying while its
account is at its quota spends nothing of its own day, and a message refused is
never a message counted. A refused send is written to the application log once
a minute per key, not once per request.

`Retry-After` for the day and for an account's quota is when the next bucket of
the window - a twenty-fourth of it - can close, not a promise that the budget is
back by then; the sentence says which ceiling was reached. The minute is kept in
memory, so a restart of the service forgives the minute it was in. The day is
kept in the per-account quota's state file, which is written at most every ten
seconds (`StateSaveIntervalSeconds`) and not at shutdown, so a restart can
forgive the last few seconds' messages; and the per-account table holds 100,000
entries - accounts and keys together - past which a new entry is not counted
and the ceiling does not apply to it (`RateLimiter.h` says why).

The answer to a message that is taken is `202`:

```
{"queue_id": 4711, "message_id": "<…@mail.example.com>", "recipients": 2, "recorded": true}
```

`queue_id` is the message's id in the delivery queue - the message trace's
correlation key - and the one to ask about afterwards. `recipients` is how many
distinct addresses the body named, not how many the message resolved to (a
list's members are not the sender's to count). `recorded` is false in the one
case where the message was queued and the key's record of it could not be
written (the error log says so, as `HM7180`): it goes, and the key cannot ask
about it.

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
sent, one another key sent, a path of more than one segment under `/send/` - is
the same `404`, so a key cannot learn from this route what other keys have
sent.

**The answer names only the addresses the body named**, one entry each, and no
other. An address that resolved to others - a distribution list, an alias, a
masked address, a catch-all, the postmaster - is answered under its own name
and never by the addresses it resolved to: those are a list's members, the real
mailbox behind a masked address, which the sender was never told and SMTP would
never tell it. The server-wide view of the same queue id, members and all, is
the message trace's (`GET /api/v1/message-trace/queue/{id}`,
`view_message_trace`), and it stays permissioned as the server's.

A recipient's state is **derived from the message trace's events for the
queue id**, and, while the message waits, from the queue itself:

| State | From | `status`, `code`, `reason` |
|---|---|---|
| `queued` | the message is in the queue, the address is still to be tried, and it has not been | empty |
| `deferred` | an attempt was made and failed and will be retried: the remote answered 4xx, did not answer, or dropped the connection (the trace's `deferred` event); or, with the trace off, the queue says it has been tried and is waiting | the last attempt's enhanced status, its code and the remote's own reply - or the server's sentence where the remote said nothing; with the trace off, how many tries |
| `delivered` | the trace's `delivered` event; `when` is when | `250` |
| `bounced` | the trace's `failed` event: the remote refused it for good, or the retries ran out | the enhanced status, the code and the remote's reply. A permanent refusal's code is the remote's own 5xx; a give-up's is the last reply's own 4xx (a remote that kept answering `451` reads `451`), and `550` where no remote replied |
| `refused` | the suppression list stopped it at delivery (the trace's `suppressed` event) | what the list's action was |
| `unknown` | nothing names the address - see below | why, in a sentence |

`reason` is the **first 255 characters** of what the trace kept - its detail
column's width, of which the bracketed `[x.y.z]` status and its space before
the reply take eight -
so a long or multi-line reply reaches the application cut.

The send's own `state` is `deferred` or `queued` while any recipient waits,
and otherwise `bounced` when any bounced, `refused` when any was refused,
`delivered` when at least one was delivered and none failed, or `unknown`.

Four things about the derivation are said as plainly as the table:

* **It needs the trace.** The message trace is **off by default**
  (`MessageTraceEnabled`), because it records who corresponds with whom. With
  it off, a send reads `queued` or `deferred` (with its tries) while it is in
  the queue, and an address the queue has let go - delivered, bounced or
  refused, nobody recorded which - reads `unknown`; `trace` in the answer is
  `false`, and the answer and the listing carry a **`trace_note`** that says
  so. Turn it on - and choose its retention - before relying on the answer.
* **An address that resolved to others** reads, while the queue holds its
  resolved addresses, the state they hold taken together - `deferred` or
  `queued` while any waits, else the worst of what happened - with the
  status, the code and the remote's reply withheld (a reply can name a
  member), and the reason *This address resolved to other addresses on this
  server; the state is theirs, taken together, and they are not named.* Once
  the queue has let them go, nothing ties the trace's events for them back to
  the address the body named, and it reads `unknown`.
* **`unknown` says which of the true things it is**: no longer waiting while
  the message is still queued for others, or the message gone from the queue;
  with the trace off, that nothing recorded what became of it; with it on,
  that the trace names no event for it - which is what a recipient **this
  server refused or removed without a trace event** reads (a full mailbox, a
  rule that deleted the message, a virus scan), as well as one that resolved
  to others, or one whose trace rows have been removed.
* **A forward or a list's re-send is a new queue id**, as the trace's own
  header says, and is not followed.

The events the states come from are the trace's: since this row the trace
records a **`deferred`** event for every attempt made to a remote that ends
without a verdict (a 4xx, no answer, a dropped connection) - not for a
recipient the server's own shaping held back (a back-off, a connection
ceiling, a per-minute rate), which nobody tried and which is rescheduled every
few minutes without being counted - followed, when the retries run out, by the
`failed` event that gives up; and the detail of a `failed` or `deferred`
delivery is `[<enhanced status>] <what was said>`: the status the server
settled on and the remote's own reply. The reports read `delivered` and
`failed` and nothing else ([Reports.md](Reports.md)). A delivery to several
local accounts is now journalled under the queue id for every recipient, where
before only the one that reused the queue row was; the `message.delivered`
webhook of each local recipient therefore carries the queue id too, and the
queue file's size ([Webhooks.md](Webhooks.md)).

### The key's sends

```
GET /api/v1/send?page=1&size=50

{"page": 1, "size": 50, "total": 3, "trace": true,
 "sends": [{"queue_id": 4713, "message_id": "<…>", "sender": "orders@example.com",
            "recipients": ["ann@example.net"], "created": "2026-09-24 10:05:40", "state": "delivered"}, …]}
```

Newest first; `size` from 1 to 200 (50 when not given), `page` from 1, and the
listing reaches back ten thousand sends - an older one is asked about by its
queue id. Each entry's `state` is the send's state as above, read for each
entry by the indexed lookups one send's answer makes (the record by key and
queue id, the queue, the trace by queue id - the newest 500 of its events).

The list is **per key**. An administrator's key that holds `send_through_api`
and names no sender - or any key that has sent nothing - reads an empty list:
the **server-wide** view of the same deliveries is the message trace, and it
stays permissioned as the server's.

A webhook subscription confined to the sender (`"addresses":
"orders@example.com"`, [Webhooks.md](Webhooks.md)) hears the same deliveries
**pushed**, as `message.delivered` and `message.bounced`, instead of asked
for, for the recipients outside this server - which is where an application's
mail mostly goes - and it does so with the trace off. An event about a
recipient this server hosts belongs to that recipient's address, not to the
sender's, so a subscription confined to the sender does not hear it; and no
webhook event says `deferred`.

### How long it is kept

The record of a send (`hm_apisends`, schema 6089: the key's id, the queue id,
the Message-ID, the sender, the addresses the body named - whole ones, lower
case, as many as 4,000 characters hold; one that would not fit is left out, and
the key is not answered about it - and when) is kept as long as the trace it is
answered from - **`MessageTraceRetentionDays`**, thirty by default - and then
removed by the **hourly** pass of the expired-records task, which removes the
trace's expired events in the same pass; `0` keeps both.

* **Revoking a key** (`DELETE /api/v1/apikeys/{id}`) deletes its records with
  it. A key taken out of the store another way - the Control Panel's revoke, an
  edit of the file - loses its records at the next hourly pass, which removes
  the records of every key id the store no longer holds (and does nothing when
  the store is missing or empty, which is not a store that was emptied of
  keys).
* **Erasing an address** (`POST /api/v1/addresses/erase`,
  [PersonalData.md](PersonalData.md)) deletes the records of what the address
  sent and takes it out of the recipients of the rest; a record left naming
  nobody goes.
* **The backup does not carry it** ([Backup.md](Backup.md)): queue ids do not
  survive a restore, so a restored server's keys list no sends.

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
* **No state without the trace** once a message has left the queue, and none
  for the addresses a named address resolved to: see above.
* **The desktop Control Panel's API key page does not show or set the three
  fields**; the Deck and the route do.

---

## Where it lives

| | |
|---|---|
| Routes | `hmailserver/source/Server/Common/Util/RestApiSend.cpp` |
| The shared send | `ResolveSendRecipients_`, `WriteComposedMessage_`, `CheckComposedMessage_` in `RestApiServer.cpp` |
| The ceilings and their refund | `ConsumeAccountQuota_` in `RestApiServer.cpp`; `RateLimiter` and `CountedUsage` in `Common/Util/RateLimiter.h` |
| The record and the states | `hmailserver/source/Server/Common/Util/ApiSends.h` and `.cpp` |
| The key fields | `ApiKeyRecord` in `RestApiServer.h`; `LoadKeys_` and `HandleCreateApiKey_` in `RestApiServer.cpp` |
| The permission | `RolePermission::SendThroughApi` in `Common/Application/RoleManager.h` |
| Schema | `hm_apisends`, from 6089, indexed by key and time and by queue id; `hm_messagetrace` indexed by queue id from the same step |
| The hourly pass | `RemoveExpiredRecords.cpp` |
| Deck | the API keys view in `hmailserver/installation/WebAdmin/index.html` |
| Tests | `hmailserver/test/RegressionTests/API/RestApiSend.cs`; the Deck's in `build/deck-script-test.js` |
