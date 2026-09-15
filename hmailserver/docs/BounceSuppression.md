Bounces read, and a suppression list
====================================

A server that keeps sending to an address that does not exist is spending its
senders' reputation: the large providers count the rate at which a sender hits
dead mailboxes, and a sender with a high rate starts landing in spam folders or
being refused outright. This server has always retried and given up. With bounce
processing on, it also reads what came back:

* every refusal met while delivering, and every RFC 3464 delivery report that
  arrives later for one of this server's senders, is **classified** against the
  RFC 3463 enhanced status code - a mailbox that does not exist, a mailbox that
  is full, a refusal on policy, a temporary failure, or something else;
* it is **counted** per recipient address and per sending domain;
* an address that has **hard-bounced** as many times in a row as the threshold
  says is put on that domain's **suppression list**;
* the next message from that domain to that address is checked against the list
  and - by default - refused, with a sentence that says why.

The list is per sending domain, visible in the Control Panel, the Control Deck,
`hmctl` and the PowerShell module, and every entry can be removed by hand.

**It is off by default.** With `BounceProcessingEnabled` off, every point where
the feature could act reads that one setting and returns: nothing is classified,
counted, stored, looked up or refused, and an upgraded server behaves exactly as
it did.

Turning it on
-------------

Control Panel → **Delivery of e-mail** → the *Bounces and the suppression list*
card. The same eight values are properties of the COM `Settings` object, keys of
`/api/v1/settings/bounces`, a group on the Control Deck's Settings page, and
`hmctl settings get bounces`. They live in `hm_settings`, not in `hMailServer.ini`,
and each is read at the moment it matters, so a change applies to the next
message without a restart.

| Setting | `hm_settings` name | REST key | Default | What it means |
|---|---|---|---|---|
| Classify bounces and keep a suppression list | `BounceProcessingEnabled` | `processing_enabled` | off | The whole feature. With it off nothing below has any effect. |
| Hard bounces in a row before an address is listed | `BounceHardBounceThreshold` | `hard_bounce_threshold` | `1` | 1 to 100. "In a row" means with no successful delivery to the address between them; a message a remote server accepted counts as delivered unless its delivery report says it failed after all - see *The run of hard bounces, when they come back as reports*. |
| Count a full mailbox as a hard bounce | `BounceMailboxFullIsHard` | `mailbox_full_is_hard` | off | A full mailbox (X.2.2) is counted either way; this decides whether it counts towards the list. |
| What happens to a listed recipient | `BounceSuppressionAction` | `suppression_action` (`refuse`, `drop`, `observe`) | `0` (refuse) | `0` refuse, `1` drop, `2` observe. See *What the list does*. |
| Days before an entry made by a bounce expires | `BounceSuppressionExpiryDays` | `suppression_expiry_days` | `0` | 0 keeps entries until they are removed; at most 3650. Measured from when the entry was made, so lowering it applies to the entries already there. Entries added by hand are not affected; they carry their own expiry. |
| Read delivery reports | `BounceReadDeliveryReports` | `read_delivery_reports` | on | Whether RFC 3464 reports are read, and whether the register they are matched against is written. Subordinate to the first setting. |
| Days bounce counts are kept | `BounceCountRetentionDays` | `count_retention_days` | `90` | A count whose last bounce and last delivery are both older than this, and whose address is not on the list, is removed by the sweep. 0 keeps counts; at most 3650. |
| Days a delivery report is still matched | `BounceReportWindowDays` | `report_window_days` | `7` | 1 to 30. How long the register keeps what it needs to believe a report; 7 is also how long a BATV tag stays valid. |

A value out of range is refused, over COM and over REST, with a sentence naming
the range - it is never silently clamped.

**The safe way to start** is `suppression_action=observe` for a week: the server
classifies, counts and lists as it will for real, delivers everything, and writes
a line to the application log for every message that would have been stopped.
Read the list, remove anything that should not be on it, and then switch to
`refuse`.

What counts as which kind of bounce
-----------------------------------

The category comes from the RFC 3463 enhanced status code, `class.subject.detail`:

| Code | Category | Counts towards the list |
|---|---|---|
| `2.x.x` | none - not a failure | no |
| `4.2.2` or `5.2.2` - mailbox full | `mailbox_full` | only with `BounceMailboxFullIsHard` |
| any other `4.x.x` | `soft` | no |
| `5.7.x` - refused on policy (spam, reputation, a blocklist, relaying, authentication) | `policy` | **never** |
| `5.1.1` bad destination mailbox, `5.1.2` bad destination system, `5.1.3` bad mailbox address syntax, `5.1.6` mailbox moved with no forwarding address, `5.1.10` null MX (RFC 7505), `5.2.1` mailbox disabled | `hard` | yes |
| any other `5.x.x` (`5.0.0`, `5.2.3`, `5.3.x`, `5.4.x`, `5.5.x`, `5.6.x`) | `other` | no |

A policy refusal says something about the message or the sending server, not about
whether the address exists, so it never lists an address whatever the threshold.

### The trust rule, and why a bare 550 is not a dead address

When a remote server refuses a recipient with a bare `550 <address>` and no
enhanced code, this server's own non-delivery report records `5.1.1` - RFC 5321
gives a 550 at RCPT TO the meaning "mailbox unavailable", and that is the right
thing to tell a person reading the report. It is the wrong thing to stop sending
on, because a great many servers answer a blocklisted sender, a poor reputation
score or a greylisting hiccup with exactly that bare 550.

So the classifier never believes a code this server worked out for itself. It
re-reads the remote's verbatim reply:

* **The reply carries its own enhanced code**, and its class agrees with the
  three-digit code (`550 5.1.1 ...`, or the last line of a multi-line reply): the
  code is trusted and the table above decides. An undefined `X.0.0` is read by the
  words of the reply, below, when they say anything.
* **The reply carries no enhanced code of its own**: the code is not trusted, and
  the words of the reply decide. Policy words are checked first - *spam, blocked,
  blacklist, blocklist, listed, policy, reputation, rbl, spamhaus, denied, not
  allowed, relay* - then full-mailbox words - *mailbox full, mailbox is full, over
  quota, quota exceeded, insufficient storage, mailbox size limit* - and only then
  dead-mailbox words - *user unknown, unknown user, no such user, no such mailbox,
  mailbox not found, recipient not found, recipnotfound, does not exist,
  unrouteable address, invalid recipient, unknown recipient*. A permanent refusal
  whose words say none of these is `other`, **never** `hard`. A reply of
  `550 Recipient address rejected: Access denied` is `policy`; `550 No such user
  here` is `hard`; `550 <gone@example.com>` is `other`.
* **No remote server answered at all** - a connection that could not be made, a
  message that could not be sent in a form the remote accepts, a failure of this
  server's own: `soft` for a temporary failure, `other` for a permanent one, never
  `hard`. Nothing about the recipient's mailbox was learned.
* **A temporary failure that outlasted the retries** is counted once, as `soft`
  (or `mailbox_full`), when the server gives up on it - never while it is still
  being retried, and never as `hard`, because nothing proved the address dead.
* **The same refusal for several recipients.** When one remote host permanently
  refuses more than one recipient of the same message with an identical reply, that
  is taken to be the session being refused - a refused EHLO, AUTH, MAIL FROM, DATA
  or BDAT chunk, which this server records against every recipient of the session -
  and not each of those mailboxes. It is counted as `policy` for an `X.7` code and
  as `other` otherwise. This server's delivery client does not yet record the stage
  a reply was given at, so the rule is a guess, and it is wrong in two ways, one in
  each direction:
  * **Identical per-recipient refusals are demoted too.** Microsoft 365 answers every
    unknown recipient with the same address-free line (`550 5.1.10
    RESOLVER.ADR.RecipientNotFound; Recipient not found by SMTP address lookup`), so
    two or more dead recipients at one such host in one message are all counted as
    `other` and none is listed - though each is listed by a message that is refused
    for it alone. This errs towards not listing.
  * **A message with a single recipient refused at any stage other than RCPT TO** -
    EHLO, AUTH, MAIL FROM, DATA or a BDAT chunk - with a remote `5.1.x` has only one
    recipient to compare, and is counted as the code says.

  Telling the stages apart properly needs a flag in `SMTPClientConnection`, and is
  the planned fix for both.

`POST /api/v1/bounces/classify`, `hmctl bounces classify "550 5.1.1 User unknown"`
and `Test-HmBounceStatus` ask the server how it would read a status or a reply,
including whether it trusted the code, and change nothing.

What is never counted
---------------------

* Anything while `BounceProcessingEnabled` is off.
* Mail whose envelope sender is not an address in a domain this server hosts
  (after domain aliases): mail relayed for somebody else, a forward this server
  rewrote into SRS (the bounce belongs to the original sender), a BATV `prvs=`
  return path, and messages with a null sender.
* Recipients delivered on this server. The list only ever applies to mail leaving
  it, and an address this server hosts cannot be put on it by hand either.
* A refusal on policy, as a step towards the list (it is counted as `policy`).
* This server's own non-delivery reports, when they arrive in a local sender's
  mailbox. They carry no Received field of this server's, so they fail the third
  check a report has to pass, even when they quote a message whose report key
  exists.

Delivery reports
----------------

The second way a bounce arrives is later: a remote server accepts the message,
fails to deliver it, and sends an RFC 3464 report back to the envelope sender.
With `BounceReadDeliveryReports` on, a report is read before it is delivered - and
it is **always delivered to the sender unchanged**, whatever was made of it.

What is read: a message whose top-level type is `multipart/report` with
`report-type=delivery-status`, carrying a `message/delivery-status` or
`message/global-delivery-status` (RFC 6533) part, and the original message's
`Message-ID` from a `text/rfc822-headers`, `message/rfc822`,
`message/global-headers` or `message/global` part. A report forwarded as an
attachment inside something else is mail about a report and is left alone. Each
recipient group with `Action: failed` is counted; `delayed`, `delivered`, `relayed`
and `expanded` are not bounces.

**The trust rule applies to reports too.** When a group's `Diagnostic-Code` is an
SMTP reply (`smtp; 550 ...`), that reply is the destination server's own words, and
the `Status` beside it may have been worked out from its three digits alone - this
server's own report writes `Status: 5.1.1` beside `Diagnostic-Code: smtp; 550
<address>`, and so does any MTA that derives its code the same way. So the reply is
judged exactly as the same reply met while delivering would be: its own enhanced
code when it has one, otherwise its words, and a bare `550` is `other`. Only a group
whose diagnostic is not an SMTP reply (`X-Postfix; unknown user`) or that has none
is taken at its `Status`, with an undefined `5.0.0` read through the diagnostic's
own enhanced code, and then its words.

Reports over 2 MB, groups past the hundredth, and a `Final-Recipient` that is not
one valid address are not read, and at most 50 distinct failed recipients of one
report are looked up in the register; a report naming more is either a very large
list, whose remaining dead addresses the next report names, or a message built to
make this server work.

### Why a report has to prove itself

Anybody can send a `multipart/report` naming any address. A server that believed
them would let a stranger put a colleague's correspondent on a suppression list
with one message - forged backscatter turned into a weapon. So a report is counted
only when **all** of these hold:

1. its envelope sender is null, as RFC 3464 requires of a report, and it is
   addressed to one of this server's addresses and no more - a report goes to the
   one envelope sender of the message it reports on (an alias that expands to
   several accounts is still one address);
2. it was not classified as spam;
3. the topmost Received field is this server's own, so it arrived over SMTP here -
   and it does not say `ESMTPA` or `ESMTPSA`: a report is never submitted by a
   signed-in user. The clause read is the **last** `by <this server> with ESMTP` in
   that field, because the HELO argument in front of it is the client's own text and
   may contain those words; nothing after it is the client's;
4. where this server writes Authentication-Results (`AuthenticationResultsEnabled`,
   off by default), its own result does not say `spf=fail` or `dmarc=fail`;
5. with BATV on, it was addressed to a `prvs=` return path that verifies - which a
   forger who never saw the signed envelope cannot produce;
6. **and it names a message this server really sent, to the recipient it
   names, for the sender it was sent to.** When a remote server accepts a message,
   this server writes one row to `hm_bouncereportkeys`: a SHA-256 of the original
   Message-ID (angle brackets stripped, lower-cased), the recipient and the envelope
   sender, each lower-cased and separated by a line feed. A report whose three
   values do not reproduce a row is ignored. A matching row is **consumed**, so the
   same report sent twice counts once.

A report that fails any check is a debug line in the log, never an error.

**What that does not stop, stated plainly.** Everybody who received a message
knows its Message-ID, its sender and the addresses in its To and Cc fields. One of
those recipients could forge a report for another visible recipient, and it would
pass checks 1, 2, 3 and 6 - and 4 and 5 too, unless the forger's server fails
SPF/DMARC or BATV is on. Bcc recipients are safe, because nobody else knows they
were sent to. An entry made from a report says `source: report`, so it can be told
apart, and removed. If that residual matters more to you than the reports do, turn
`read_delivery_reports` off: the refusals met while delivering are still read.

**What the register costs.** One small row - two 64-character hashes, a domain id,
a time and a small number - per external recipient a remote server accepted, kept
for `report_window_days`. A server sending 100,000 external recipients a day with
the default window holds about 700,000 rows. No address is stored in it as text: the
recipient is there as a SHA-256 of the lower-cased address, which is how a later
message to the same address is found and how an erasure finds the rows, and which
anybody who can guess an address and read the database can reproduce. Messages
without a Message-ID get no row and their reports cannot be matched. With
`read_delivery_reports` off, nothing is written.

### The run of hard bounces, when they come back as reports

A report is only ever about a message a remote server **accepted**, so an
acceptance cannot simply end the address's run of hard bounces: if it did, every
report would count one from nothing, and behind a smart host - where every failure
comes back as a report - no threshold above one could ever be reached. Instead, an
acceptance **sets the run aside** in the register row it writes, and the count shows
`consecutive_hard` as 0 while the report may still come:

* the report that the message failed **gives the run back**, plus one for a hard
  bounce (plus nothing for one that does not count towards the list - a reported
  policy refusal is not a delivery either);
* **no report** within `report_window_days` means the message was delivered, and
  what was set aside is dropped with the register row;
* when **a later message to the same address was accepted** before the report
  arrived, and its own report has not come, the report adds nothing now and carries
  its run forward onto that later message's row - given back only if the later
  message fails as well;
* when a message **the register does not hold** - one without a Message-ID, or one
  delivered while `read_delivery_reports` was off - was delivered to the address
  after the reported message was sent, the report adds nothing. That is known only
  for an address that already had a count row at the time, since a delivery to an
  address that has never bounced is not written anywhere.

Every case that cannot be decided adds nothing, so the run can come out short -
reports for two messages that arrive in the reverse of the order the messages were
sent in count one, not two. It comes out long in one case only: an address that had
never bounced, sent a message the register holds and then one it does not, both
before the first message's report arrives, or sent that second one within the same second as the first - and then only by that one report.

What the list does
------------------

**When it is checked.** At SMTP `RCPT TO`, for a submission - an authenticated
session, or a sender in a hosted domain from an IP range that does not require
authentication - to an address outside this server; on the webmail's send route;
and at delivery, for every message whose envelope sender is in a hosted domain.
That last check is the one every path meets: an alias or a distribution list that
expands to an outside address, a forward, a rule, a scheduled send, a message
queued before the entry was made.

| Action | At `RCPT TO` and on the webmail | At delivery |
|---|---|---|
| `refuse` (0) | Refused with `550`, the status that listed the address - `5.1.1` for a dead mailbox - or `5.7.1` for an entry added by hand, and a sentence: *gone@example.com is on the suppression list of example.org: mail to it bounced permanently (5.1.1) on 2026-09-15. An administrator can remove it.* The webmail says the same in the reader's language - that the address bounced, or, for an entry added by hand, that an administrator put it on the list (the send route's 400 answers `code: recipient_suppressed`, the `address` and the entry's `source`). Nothing is queued. | The recipient is removed from the message and returned to the sender in an ordinary RFC 3464 non-delivery report with the same status and sentence, unless the message's DSN `NOTIFY` excluded failures. The OnDeliveryFailed event fires, and AWStats records a 550. |
| `drop` (1) | Accepted. | The recipient is removed from the message silently: nothing reaches the remote server and nothing reaches the sender. A line in the application log says so. |
| `observe` (2) | Accepted. | Delivered. A line in the application log says it would have been stopped. |

When the message trace is on, each recipient the list acted on at delivery is
recorded with the event `suppressed` and the action in the detail; a refusal at
`RCPT TO` is in the SMTP log and in AWStats as a 550.

**Mail already in the queue** when an entry is made meets the list at its next
delivery attempt, and is returned or dropped then. That is what the list is for,
and it can still surprise: `observe` is the way to see it first.

**If the list cannot be read** - the database is down, a query fails - the message
is sent as if the address were not listed, and error `HM6622` is reported at most
once an hour while it lasts. Refusing mail because a list could not be read would
be worse than the list being best-effort during an outage.

Looking at the list, and changing it
------------------------------------

* **Control Panel** → *Monitoring* → **Suppression list**: pick a domain; the
  entries - address, why, status, where the entry came from, since when, when it
  expires, with the remote's own words on each row's tip - and the counts behind
  them. *Add* puts an address on by hand with a note and an expiry in days; *Remove*
  takes one off.
* **Control Deck** → *Domains* → **Suppressions ›** on the domain's row: the same
  two tables, a search, the add form, and *Remove* on each entry. The Settings
  page has the eight settings, with *Expire and prune now*.
* **REST**, all described in the OpenAPI document:

  | Route | Does |
  |---|---|
  | `GET /api/v1/domains/{domain}/suppressions?search=&limit=&offset=` | The list, newest first, with whether processing is on and the current action. |
  | `POST /api/v1/domains/{domain}/suppressions` | `{"address", "note", "expires_days"}` - 201 with the entry; 409 when listed already; 400 for an address that is not valid, one this server hosts, or `expires_days` outside 0 to 3650. |
  | `DELETE /api/v1/domains/{domain}/suppressions/{address}` | Removes an entry (the address may be percent-encoded); 404 when it is not listed. |
  | `GET /api/v1/domains/{domain}/bounces?search=&limit=&offset=` | The counts: `hard`, `soft`, `mailbox_full`, `policy`, `other`, `consecutive_hard`, the last status, bounce and delivery, and whether the address is listed now. |
  | `POST /api/v1/bounces/classify` | `{"status", "diagnostic"}` - how it would be read. Writes nothing. |
  | `POST /api/v1/bounces/expire` | The sweep, now. |
  | `GET`/`PUT /api/v1/settings/bounces` | The eight settings. |

* **COM**: `Utilities.GetSuppressionList`, `AddSuppression`, `RemoveSuppression`,
  `GetBounceCounts`, `ClassifyBounce` and `RunBounceMaintenance`; the settings are
  `Settings.Bounce*`.
* **Shell**: `hmctl suppression list|create|delete --domain`, `hmctl bounce list
  --domain`, `hmctl bounces classify|expire`, and `Get-HmSuppression`,
  `Add-HmSuppression`, `Remove-HmSuppression`, `Get-HmBounceCount`,
  `Test-HmBounceStatus`, `Invoke-HmBounceMaintenance` - see
  [CommandLine.md](CommandLine.md).

**Removing an entry** lets the next message through at once, and starts the
address's run of hard bounces again from none - so with a threshold of three, the
address is listed again after three more hard bounces, not one. That includes a
report still on its way about a message accepted before the removal: what that
message set aside is forgotten, so its report adds one. The counts themselves are
kept as history.

**Who may.** The list and the counts of a domain answer to the server
administrator and, over COM, to that domain's administrator; over REST, an API key
restricted to some domains reaches only those domains' lists, and a read-only key
reads but does not add or remove. The settings apply to every domain and the sweep
removes entries from every domain's list, so both are refused to a key restricted
to domains.

**The audit trail.** An entry added or removed by hand is recorded in the audit
trail as a *suppression list entry*, with who did it and over which interface. The
entries the server makes itself from bounces are not administrative changes and
are not recorded there; the list says where each of those came from.

Expiry and the sweep
--------------------

An entry stops applying the moment its own expiry passes, or - for an entry made
by a bounce or a report - once it is older than `suppression_expiry_days`, even
before anything removes it; the list shows such an entry as `expired`. A fresh hard
bounce for an address whose entry has expired but not yet been swept makes a new
entry.

Once at service start and every hour, a maintenance task removes expired entries,
counts older than `count_retention_days` (at most 5,000 per pass) whose address is
not listed, and register rows older than `report_window_days`. On a server that
never turned the feature on, each pass is a few indexed queries that find nothing. The
REST route, `hmctl bounces expire` and `Utilities.RunBounceMaintenance` run the same
sweep now and say what it removed.

Erasure and deleting a domain
-----------------------------

`Utilities.EraseAddressTraces` removes an address's bounce counts, its entries on
every domain's list and its rows in the register, along with the rest of what the
server keeps about it.
**The next message to that address is sent**: an erasure is somebody asking to be
forgotten, and that includes being forgotten as a dead address.

Deleting a domain deletes its counts, its entries and its register rows; all three
tables cascade from `hm_domains` on every supported database. Renaming a domain
changes nothing, because the rows are keyed by the domain's id.

The three tables
----------------

Schema 6051 adds them; nothing is written to any of them until processing is on.
Every address is stored lower-cased and every date is a `YYYY-MM-DD HH:MM:SS` string
in the server's local time.

* `hm_bouncesuppressions` - the list: `bsdomainid`, `bsaddress` (unique together),
  `bssource` (0 a bounce while delivering, 1 a delivery report, 2 added by hand),
  `bscategory`, `bsstatus`, `bsdiagnostic` (the remote's words, or the
  administrator's note, 255 characters), `bsremotemta`, `bscreated`, `bsexpires`
  (empty for none).
* `hm_bouncecounts` - per domain and address: `bchard`, `bcsoft`, `bcmailboxfull`,
  `bcpolicy`, `bcother`, `bcconsecutivehard`, `bclaststatus`, `bclastcategory`,
  `bclastbounce`, `bclastdelivered` (empty when never delivered).
* `hm_bouncereportkeys` - the register: `brkdomainid`, `brkkey` (the SHA-256 of
  Message-ID, recipient and sender), `brksent`, `brkaddress` (the SHA-256 of the
  recipient alone) and `brkrun` (the run of hard bounces the acceptance set aside).

Addresses are compared lower-cased, local part included. That is right for almost
every provider and not strictly what RFC 5321 says; plus-addressed variants are
different addresses.

Errors
------

All Medium. None is written on an expected path: a report that is not a report, or
that fails a check, is a debug line.

| Error | Means |
|---|---|
| `HM6620` | A bounce could not be counted, an address that reached the threshold could not be put on the list, or a report's run could not be carried forward to a later message. |
| `HM6621` | A delivered message's report key could not be written, so a report that later arrives for it will be ignored. |
| `HM6622` | The list could not be read, so a message was sent as if the address were not listed. At most once an hour. |
| `HM6623` | A recipient the list stopped could not be removed from the queued message's row. If another recipient keeps the message queued, the next attempt reads that recipient back and stops it - and, with `refuse`, reports it to the sender - again. |
| `HM6624` | The maintenance sweep failed; what is left is tried again next hour. |
| `HM6625` | A delivery could not be recorded against the address's count, so its run of hard bounces was not ended; or an entry removed by hand could not start its run again. Either way the address may be listed sooner than the threshold says. |
