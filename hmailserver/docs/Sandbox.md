Attachments opened somewhere safe
=================================

ClamAV says whether a file is known to be bad. What an unknown file does when it
is opened is a different question, and answering it takes a sandbox - a machine
that opens the file, watches what it does and says so - and minutes. This server
can hand a message's attachments to one and hold the message until the answer
comes back.

**It does nothing until you name a sandbox.** The sandbox is a service outside
this server, which you run or subscribe to; this server only speaks to it. With
`SandboxEnabled` off (the default) nothing is held and nothing is submitted, and
the switch cannot be saved on while either address is empty: REST, COM and the
Control Panel refuse it in the same sentence. A configuration that has it so
anyway - an address emptied over COM while the switch was on, or a value put in
the database by hand - holds nothing and submits nothing, puts back in the queue
whatever it was holding (with the header that says it was not examined), and
says so in the application log at every start. The Control Panel's *External
setup* page says which of these is the case.

What happens to a message
-------------------------

After the anti-virus scan and before the rules, the deliverer looks at every
attachment of a message that no signed-in user of this server submitted (see
*What is exempt*). One whose file extension or media type is in `SandboxTypes`
- the executable and document set by default - is to be **submitted**, and the
message is **held**: the delivery thread records the hold and goes on to the
next message, and the sandbox's poller POSTs the file's decoded bytes to
`SandboxSubmitUrl` within a second. A message with no such attachment is
delivered as it always was.

The poller asks `SandboxVerdictUrl` about each submitted file every
`SandboxPollSeconds` (30). A `pending` answer waits for the next poll. **A
malicious answer decides the hold at once. Anything else waits until every file
has an answer**: an `error` on one file is not a reason to stop waiting for the
others - a payload beside a file the sandbox fails on quickly would otherwise be
delivered within one poll, never having had its own verdict. Once decided, the
message goes back into the delivery queue, where the deliverer carries the
decision out:

| Decision | What happens | `X-hMailServer-Sandbox` |
|---|---|---|
| every file **clean** | delivered as if never held | `clean (<n> file(s), <seconds>s)` |
| any file **malicious**, or **suspicious** with a score at or above `SandboxScoreThreshold` (70) | quarantined, with the verdict's detail as the reason; returned to its sender with 5.7.1 while `QuarantineEnabled` is off | `malicious (...)`, on the quarantined copy |
| every file answered and one of them an **error** | `SandboxTimeoutAction`: 0 delivers with a header, 1 returns it to its sender with 5.7.1 | `error on <file>: <detail> (...)` |
| no answer within `SandboxTimeoutMinutes` (15) of the hold's start | the same | `timed out after <m> minute(s) (...)` |
| **released** by an administrator | delivered without the verdict | `released by administrator (...)` |
| the sandbox **turned off**, or an address emptied, while it was held | delivered without a verdict | `not examined: the sandbox was turned off, or an address emptied, while the message was held (...)` |

A suspicious answer below the threshold counts as clean. A suspicious answer
**without a numeric score** - no score, `null`, or the digits in a string - is an
error: never clean, and never remembered, since the threshold has nothing to
judge.

The header is written at the top of the message, once. A sender's own
`X-hMailServer-Sandbox` is removed as the message enters the server's hands - on
its first delivery pass, before anything is examined, and whether or not the
sandbox is on - so that the field a reader meets is only ever this server's.

A message is examined once, on its first delivery pass, as its DKIM signature and
its mirror copy are made once: a message a remote server deferred comes round again
with its local recipients already delivered and is not submitted again. A copy
this server makes of a delivered message - an account's forward, say - is a
message of its own and is examined as one; the verdict cache answers for a
file already answered.

The quarantine is the end state of a verdict, not of a wait: the recipient's
quarantine digest lists a quarantined message, and the recipient or an
administrator releases it as they would any other. **The release of a message the
sandbox quarantined does not submit it again**: the quarantine's release marks it
(`Message::FlagReleasedFromSandboxQuarantine`) when the entry's reason is the
sandbox's own, and the deliverer leaves it alone - its remembered malicious verdict
would otherwise put it straight back. **Any other release is examined like new
mail.** The spam quarantine holds a message during the SMTP conversation, before
the deliverer ever sees it, so a message released from it has never met the
sandbox; it is held and submitted like any other, and the cache answers for a
file already judged. The webhook `message.quarantined` is raised by the quarantine
as it is for every other held message.

**The digest and the release need the quarantine on** (`QuarantineEnabled` in the
[Settings] store, off as shipped). While it is off, a malicious verdict does not
leave the message in a store no digest reads and no release is offered from: it
is returned to its sender with 5.7.1, and the application log says so.

A message returned to its sender gets the report every other permanent failure
gets (RFC 3464, status 5.7.1). A message with no sender a report can go to - an
empty envelope sender or a mailer-daemon - is **discarded** instead, and the log
says `discarded: no sender to return it to`; one marked `Auto-Submitted` is
declined by the report itself, which says so as HM4404. Delivering instead would
make an empty envelope sender a way past the sandbox.

The contract
------------

A sandbox is an HTTP service you name, not a vendor integration. Two addresses,
both https, or plain http to this machine (`127.0.0.1`, `::1`, `localhost`) - an
attachment and its verdict sent in the clear across a network could be read and
changed on the way. Both are stored trimmed, whichever surface set them.

**Submit.** One POST per file to `SandboxSubmitUrl`, as `multipart/form-data`
with four fields:

| Field | Value |
|---|---|
| `file` | the attachment's decoded bytes, `application/octet-stream`, with its file name |
| `filename` | the file name, UTF-8 |
| `sha256` | the SHA-256 of the bytes, lower-case hex |
| `message_id` | this server's queue id for the message - the `queue_id` the held list shows |

With `SandboxToken` set, every request carries `Authorization: Bearer <token>`.
The answer is any 2xx with a JSON object whose `id` (a string or a number, at
most 1000 characters) names the submission; an answer without one is polled by
the SHA-256 instead. An id longer than that is a failed submit, retried at every
poll until the hold times out, since every verdict request would otherwise ask
for an id the sandbox never gave.

**Verdict.** A GET to `SandboxVerdictUrl` with `id=<the submission id>` added -
or `sha256=<the hash>` when the submit answered no id. When the address contains
`{id}` or `{sha256}` they are replaced instead, for a service that wants them in
the path; `{id}` becomes the hash when the submit answered no id. The answer is a
JSON object:

    {"state":"pending"}
    {"state":"done","verdict":"clean","score":3,"detail":"opened, nothing happened"}

`verdict` is `clean`, `malicious`, `suspicious` or `error`; `score` is a number
from 0 to 100, which a suspicious verdict must carry; `detail` is one line of the
sandbox's own words, kept to 255 characters. Anything else - another status than
200, a body that is not that object, a state or a verdict not listed here - is a
failed request.

**Each request has its own ceiling** - a minute for a submit, fifteen seconds for
a verdict, connect, TLS and the whole answer together - and one pass of the
poller spends at most a minute on requests: past it no request is started, and
the held messages it did not reach come first at the next pass. The timeouts are
decided at the start of every pass, before any request, so a sandbox that accepts
connections and never answers cannot keep a message past `SandboxTimeoutMinutes`
by more than a pass.

    curl -s -X POST https://sandbox.example.com/submit \
         -H "Authorization: Bearer $TOKEN" \
         -F file=@invoice.exe -F filename=invoice.exe \
         -F sha256=$(sha256sum invoice.exe | cut -d' ' -f1) -F message_id=4193
    {"id":"a1b2c3"}

    curl -s "https://sandbox.example.com/verdict?id=a1b2c3" -H "Authorization: Bearer $TOKEN"
    {"state":"done","verdict":"malicious","score":97,"detail":"Drops a ransomware payload"}

**No adapter is shipped.** Cuckoo and CAPE (their REST API's `tasks/create/file`
and `tasks/report`), Joe Sandbox (`analysis/submit` and `analysis/info`) and
VirusTotal's file scan (`files` and `analyses/{id}`) each speak something close to
this, and an adapter of a dozen lines - a small web service in front of one of
them - maps its answer onto the four words above. Which of them a given
installation should use, and what its score means, is that installation's
decision. A service that shares what it is sent with others - VirusTotal shares
uploads with its subscribers - is a place the attachments of the mail you receive
go; choose it knowing that.

**The token.** Visible ASCII only - a space, a tab or a line break would end the
`Authorization` header and begin another - and at most 2048 characters; 248 while
`ProtectStoredSecretsWithDPAPI` is 0, since the legacy scheme a protected setting
is then written with holds no more. It is stored protected, as the single sign-on
client secret is, and no interface ever reads it back. **It is not in the backup
archive either**: a restore leaves the token this server holds as it is, and on a
new server it is entered again after the restore. An empty field in the Control
Panel keeps the token; its *Remove the token* button clears it, as
`sandbox_token: ""` over REST and `SetSandboxToken("")` over COM do.

The hold, and what it is not
----------------------------

A held message is `hm_messages.messagetype` 5 (`Message::HeldForSandbox`). The
deliverer's select never matches it, so it is in no mailbox and has no delivery
attempts; it is not in the quarantine and not in anybody's digest. Its files are
rows of `hm_sandboxholds`.

It is not the anti-virus scanner's fail-closed hold either: `AVFailAction` 1
keeps a message the scanner could not examine in the queue and tries the scan
again; this hold waits for somebody else's answer and ends on a clock.

**The poller** is a thread of its own, started with the servers. At every pass it
decides, first and without a request, every hold that is decided, turned off or
timed out; then it submits any file not yet submitted (the connection refused,
the service answering an error: the file is read again from the queued message)
and asks the verdict of each submitted file, within the pass's minute. A failed
request is counted - `errors` in the held list - and the first one of each file
is one line in the application log; the rest are debug lines, since a sandbox
that is down is one condition and not a line every thirty seconds per file.
`SandboxTimeoutMinutes` counts from the moment the message was held, whatever
the sandbox is doing. A stop of the service waits for a request in flight to end
- a minute at most - before it goes on.

A message whose held pass came before the rules has had no rule run on it, no
DKIM signature and no mirror copy: all of that happens on the pass that delivers
it. The `OnDeliveryStart` event script runs on both passes.

Turning the sandbox off, or emptying an address, does not strand what it holds:
at its next pass the poller puts every held message back in the queue, delivered
with the header that says it was not examined.

**The compromised-account hold takes precedence.** An account the
compromised-account detection locks has its queued mail from the hour moved to
review, and a message of its waiting here is moved too (messagetype 4). No
sandbox decision reaches it there; released from review, it meets its hold again,
and the timeout, which kept counting, decides it at the next poll.

What the administrator sees
---------------------------

`GET /api/v1/sandbox/held` lists what is held, oldest first, a page at a time -
100 by default, `?limit=` up to 1000 and `?offset=` - with `total`, the number held
in all, beside `count`, the number on the page: the queue id, the sender and
recipients, when it was held, and each file with its size, SHA-256, the submission
id, how far it has got (`not_submitted`, `submitted`, `answered`, or
`not_submittable` for one too large or unreadable), the verdict and the count of
failed requests. `active` says whether anything is being held at all (the switch
on and both addresses set). `GET /api/v1/queue` lists every held message too, as it
lists every queued one, with `"held":"sandbox"`, `held_since` and the submission
ids; an ordinary item has no `held` member.

Two decisions, under the delivery queue's permission (`manage_queue`), both on
the audit trail with the sender, the recipients and the files:

    POST   /api/v1/sandbox/held/{queue_id}/release   delivered now, without the verdict
    DELETE /api/v1/sandbox/held/{queue_id}           deleted, and nobody told

A release and a verdict cannot both win: a release records itself only on a hold
still undecided, and when a verdict, the timeout or the switch got there first the
answer is 409 and the message goes on as that decision says, with nothing recorded
as released.

**The queue's own Retry and Delete answer 409** for a held message, naming the
sandbox's routes - `{"error":"held for the sandbox's verdict; use
/api/v1/sandbox/held/{id}"}` - and clearing the delivery queue does not clear what
is held, as it does not clear what the compromised-account detection holds.
`hmctl queue list` has a `held` column, and `Get-HmQueue` gives every item a `held`
property (`sandbox` or empty), so `Get-HmQueue | Where-Object held -ne 'sandbox' |
Remove-HmQueueMessage` empties the queue without meeting a refusal. **COM's
`DeliveryQueue.Remove`** of a held message is the sandbox's discard - the row, the
file and the hold's records - and is on the audit trail as one, its detail ending
`(DeliveryQueue.Remove)`.

The list is server-wide like the queue and refused to a credential confined to
named domains; a read-only key reads it and changes nothing. The Control Deck's
queue page has a *Held for a sandbox* box with both buttons, each asking first,
and each file's state and verdict in the reader's language.

**The person never sees a held message.** The webmail shows nothing of it: the
message is not theirs yet.

The settings
------------

All in the database, on `PUT /api/v1/settings`, on COM `Settings`, and on the
Control Panel's Anti-virus page, *Sandbox* tab - where the two addresses come
before the switch, because a save writes the rows in order and the switch is
refused on until both are there. Each surface refuses a value outside its range in
the same words.

| Setting (REST) | COM | Default | |
|---|---|---|---|
| `sandbox_enabled` | `SandboxEnabled` | off | refused on while either address is empty |
| `sandbox_submit_url` | `SandboxSubmitUrl` | empty | https, or http to this machine |
| `sandbox_verdict_url` | `SandboxVerdictUrl` | empty | the same |
| `sandbox_token` | `SetSandboxToken` | empty | write-only; see *The token* |
| `sandbox_timeout_minutes` | `SandboxTimeoutMinutes` | 15 | 1 to 1440 |
| `sandbox_timeout_action` | `SandboxTimeoutAction` | `deliver` (0) | or `bounce` (1) |
| `sandbox_max_size_kb` | `SandboxMaxSizeKB` | 20480 | 1 to 81920, the size of message this server parses |
| `sandbox_max_files` | `SandboxMaxFiles` | 10 | 1 to 100 |
| `sandbox_types` | `SandboxTypes` | see below | |
| `sandbox_poll_seconds` | `SandboxPollSeconds` | 30 | 5 to 3600 |
| `sandbox_score_threshold` | `SandboxScoreThreshold` | 70 | 0 to 100 |
| `sandbox_cache_hours` | `SandboxCacheHours` | 24 | 0 to 720 |
| `sandbox_internal_mail` | `SandboxInternalMail` | off | |

Over REST the switch and the two addresses are judged together, against what the
request would leave, so one request may set all three; over COM, where each
property is saved on its own, the switch is refused while an address is empty and
an address emptied while it is on is taken - and holds nothing, as above.

The types
---------

`SandboxTypes` is a list of file extensions (`exe`) and media types
(`application/pdf`), separated by commas, semicolons or spaces; it is stored in
lower case, each once, with a leading `.` or `*.` dropped. Anything that is neither
- `exe/bat`, say, whose slash makes it no extension and whose `exe` is no media
type - is refused. The default:

    exe,dll,scr,js,jse,vbs,wsf,hta,msi,bat,cmd,ps1,jar,lnk,iso,img,docm,xlsm,pptm,
    doc,xls,ppt,rtf,pdf,zip,7z,rar

An attachment matches on its media type, or on the last extension of **either of
its names** - the `Content-Disposition` filename and the `Content-Type` name, since
a mail client that prefers the one the other ignores saves the file under a
different extension - taken **as Windows saves the file**, with trailing dots and
white space dropped: `invoice.exe.` is an `exe`. The blocked-attachment list of the
anti-virus page judges names the same way. **A forwarded message is opened**: a
`message/rfc822` part's own attachments are judged as the outer message's are,
five forwarding levels deep, and a forward nested deeper than that is submitted
whole, whatever its type. **An archive is submitted whole**: nothing is unpacked
here, and what is in it is the sandbox's to open. The MIME parser stops splitting
a message at twenty levels of nested multipart (`MimeBody::MaxNestingDepth`), and a
part buried below that is not seen as an attachment.

The same file attached twice to one message is submitted once. An attachment
larger than `SandboxMaxSizeKB`, or one whose content cannot be decoded, is not
submitted; the message is still held for any other file, and the header names the
ones that were not (`not submitted: <file> (larger than <n> KB, or unreadable)`). A
message whose only matching files are like that is delivered at once with that
header.

**A message with more files to submit than `SandboxMaxFiles`** is not held and
nothing of it is uploaded: each file is a submission against the sandbox's quota
and a request at every poll. `SandboxTimeoutAction` decides it at once, with
`not examined: <n> files to submit, more than SandboxMaxFiles (<m>)`.

A file name is stored with a character outside the Basic Multilingual Plane - an
emoji - as a question mark, in the held list and the log. The rule was made for
MySQL, whose tables were three-byte `utf8` and refused such a character until
schema 6102 moved them to `utf8mb4`; it has not been lifted since.

An encrypted message's ciphertext is one attachment by its media type
(`smime.p7m`, `encrypted.asc`); it is submitted only if its extension or type is
listed, which none is by default, and a verdict on it would be a verdict on bytes
nobody can read.

The cache
---------

A clean or malicious answer is remembered by the file's SHA-256 in
`hm_sandboxverdicts` for `SandboxCacheHours`. The same file in a later message is
not submitted again: remembered clean, the message is delivered at once with
`clean (<n> file(s), 0s)` when every file is remembered clean; remembered
malicious, the message is quarantined at once (returned while the quarantine is
off). An `error` is never remembered. 0 remembers nothing; expired rows are swept
by the poller. What that means:

* **A remembered clean answer is believed until it expires**, even if the sandbox
  would now say otherwise; set 0 to submit every file every time.
* **The cache is server-wide.** A file one domain received answers for every
  domain, and a message delivered at once with `clean (1 file(s), 0s)` tells its
  recipient that some account here received that exact file within the window.
* **A suspicious answer is remembered as what the threshold made it** when it came
  in: lowering the threshold does not judge a cached file again.

Two servers on one database may answer the same file at once; the second one's
row is refused by the unique index on the hash, quietly, and the first stands.

What is exempt
--------------

Nothing is submitted for:

* **what this server's own signed-in users submit**, while `SandboxInternalMail` is
  off: a message from an authenticated SMTP session, the webmail's send and what is
  sent through its code (a scheduled draft, a read receipt, an unsubscribe), and
  what an application sends through the send API with a key an administrator
  issued (`POST /api/v1/send`, [SendApi.md](SendApi.md)) - to
  anyone, a colleague or the outside. The server marks such a message
  when it creates it (`Message::FlagSubmittedBySignedInUser`), never from anything
  in the message. **Mail your own users send to outside addresses is therefore not
  uploaded to the third party** unless `SandboxInternalMail` says so; with it on,
  everything they submit is held and its files are uploaded as mail from outside
  is. The threat model of the default: a password stolen and used to send
  attachments to colleagues is not examined by the sandbox - ClamAV, the
  blocked-attachment list and the compromised-account detection still are;
  turn `SandboxInternalMail` on to have that mail opened too;
* a message none of whose attachments is of a submitted type;
* a message the anti-virus scanner deleted, which never reaches the sandbox;
* a message a person released from the quarantine **the sandbox's own verdict** put
  it in.

Nothing else is exempt. A message that **cannot be read** - larger than the 80 MB
the MIME parser takes, which is decided at once, or a file another process holds
for longer than five tries a minute apart - and a message whose **hold cannot be
recorded** after five tries a minute apart are not delivered unexamined:
`SandboxTimeoutAction` decides them, with `not examined: <why>` in the header when
it delivers.

What it does not do
-------------------

* **No vendor adapters.** The contract above is the whole of the integration.
* **Files only.** A link in a message is not opened anywhere; a URL-detonation
  service is a different contract.
* **Archives are not opened here.**
* **No event script.** There is no `OnSandboxVerdict`: the scripting surface's
  events are a fixed set with no anti-virus event to extend (there is no
  `OnVirusFound`). The message trace records `sandbox-submitted` and
  `sandbox-verdict` for each file, with the file, the hash, the submission id and
  the verdict, when `MessageTraceEnabled` is on; the application log has one line
  per submission and one per verdict.
* **The hold is per server.** Two servers on one database each poll the holds;
  the first to decide one puts it back in the queue, and a file not yet submitted
  may be submitted by both.

The errors
----------

| Code | When |
|---|---|
| HM7270 | the hold of a message could not be recorded or read, or the message could not be read; it stays queued, undelivered, and is tried again a minute later - five times, and then `SandboxTimeoutAction` decides it |
| HM7271 | a message could not be moved to the hold; the same |
| HM7272 | the sandbox called a message malicious and the quarantine could not store it; the message is held again, never delivered, and the quarantine is tried again at the next poll |
| HM7273 | the `X-hMailServer-Sandbox` header could not be written; the message is delivered without it |
| HM7274 | a pass of the poller ended in an unexpected error (at most once an hour); held messages wait for the next pass |
| HM7275 | the examination of a message ended in an unexpected error; the message is delivered without a verdict, as a message the scanner could not examine is by default |
| HM7276 | a decision could not be recorded; the message stays held and the next poll tries again |
| HM7277 | a value was not encrypted with the legacy Blowfish scheme because it is longer than the 248 characters it holds - any protected setting while `ProtectStoredSecretsWithDPAPI` is 0, or DPAPI failing; nothing is written in its place |
