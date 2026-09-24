Mailing lists
=============

A distribution list sends a message to its members. A *mailing list* is a
distribution list people can join and leave themselves, whose every member gets
a copy of their own with a way out that works, which keeps an archive, offers a
daily digest and notices when an address has died. That is what a small
organisation runs Mailman for, and this is the same thing inside the mail
server: a distribution list with the **list manager** switched on.

Nothing changes for a list until it is switched on. Every list an upgrade finds
is unmanaged, every member confirmed and sent each message, and such a list
expands to its members when a message arrives exactly as it always has - the
membership query, the sender rules, moderation and the bounce address
([DistributionLists.md](DistributionLists.md)) included.

Switching it on
---------------

On the list: *Managed* in the Control Panel's list dialog, the `managed` field on
the Control Deck's list editor and on `PUT /api/v1/lists/{address}`, or
`DistributionList.Managed` over COM. The rest of the list manager's fields sit
beside it and do nothing while it is off.

For the links in the list's mail - the confirmation page, each member's own page
with its one-click unsubscribe, the owner's approval page, the subscribe form,
the archive - the server has to know where they are reached from outside:
`list_base_url` on `PUT /api/v1/settings` (the Control Deck's Settings page),
such as `https://lists.example.com`, stored in `hm_settings` as `ListBaseUrl`.
**Every one of those pages is served by the REST listener**, which is off until
`RestApiPort` is set: switch it on, and make it reachable at that address over
HTTPS, before setting the base URL - a link that leads nowhere is worse than no
link, because the providers that follow it conclude the list is broken. Empty,
the default, offers no link at all: the copies carry the mailto addresses alone,
and a confirmation asks for a reply. The setting refuses what the list manager
could not use: a URL that is not `http://` or `https://`, one with a space, a
comma or an angle bracket, and one that is not printable ASCII - a host name
that is not ASCII is written in its punycode form.

**Only an `https://` base gives the copies RFC 8058's one-click**
(`List-Unsubscribe-Post`), which that RFC requires to be HTTPS. An `http://`
base gives the links for a person to follow, and no one-click.

The addresses a list answers
----------------------------

For a managed list `team@example.com`:

| Address | What it does |
|---|---|
| `team@example.com` | Post to the list. Who may post is the list's mode, its authentication requirement and its moderator, exactly as for any list. |
| `team-subscribe@example.com` | Ask to join, from the address to be subscribed; and confirm, by replying to the confirmation. |
| `team-unsubscribe@example.com` | Ask to leave: the envelope sender, when it is on the list, is sent a link and a token, and leaves when it follows the one or replies with the other. A stranger writing here is logged and not answered. |
| `team-request@example.com` | The help text, with every address above. |
| `team-owner@example.com` | The person who runs the list, followed as an alias is. |
| `team-bounces@example.com` | Delivery reports for the list's copies. |
| `team-bounces+jo=example.org@example.com` | The same, with the member encoded (VERP), when the list's `verp` is on. |

None of them is an account and none of them keeps anything: each is a recipient
the list manager answers in the delivery path and takes off the message. They
are read before plus addressing, so a domain whose plus character is `-` does
not read `team-request@` as `team@` with a tag; and an account, an alias or a
list of exactly one of those names is the administrator's and wins over the
manager.

The owner is the list's `owner`, or when that is empty its moderator, or the
domain's catch-all, or `postmaster@` the list's domain. It is exactly one e-mail
address, and cannot be the list itself or one of the addresses above - a save
that tries is refused; `team-lead@example.com` is none of them, and may own
`team@`.

The texts the manager answers with are the server's translatable messages -
`LIST_CONFIRM`, `LIST_REFUSED`, `LIST_HELP`, `LIST_APPROVE`, `LIST_BOUNCED`,
`LIST_LEAVE`, `LIST_UNSUBSCRIBED` and a `_SUBJECT` for each - edited on the
Control Deck's Server messages page like every other, with `%MACRO_LIST%`,
`%MACRO_ADDRESS%`, `%MACRO_LINK%`, `%MACRO_OWNER%`, `%MACRO_REASON%` in the
bounce notice and, in the help, the list's own addresses. `%MACRO_OWNER%` is the
owner's own address only in the notices sent to the owner; anybody else - a
stranger refused, a member told they have left - is given `team-owner@`, which
reaches the same person without handing out their address. Each answer is sent
from `team-request@` with a null envelope sender and
`Auto-Submitted: auto-replied`; a message that is itself automatic
(`Auto-Submitted`, `Precedence: bulk`, `junk` or `list`, or a `List-Id`) is never
answered, so two servers cannot answer each other.

Joining
-------

`subscribe_policy` decides who may join:

* **0 closed** - the default. Members are added by the administrator; a request
  by mail or by page is answered with `LIST_REFUSED` and subscribes nobody.
* **1 confirm** - anyone. The address asked for is sent `LIST_CONFIRM`, whose
  subject and page link carry a token; a reply carrying it, or the button on the
  page, makes the address a member.
* **2 moderated** - anyone the owner lets in. After the address confirms, the
  owner is sent `LIST_APPROVE` with a link to an approval page and a new token.
  The page's button approves **only for a webmail session of the owner's own
  account**: the token says the link came from the owner's mailbox, the session
  says who pressed it, and either alone is not the owner - no session, or
  another account's, is refused. The link is opened from the mail client, which
  does not bring the webmail's cookie; the button, pressed on the page, does. A
  reply by mail approves nothing: its envelope sender is free text, and this
  server's `Received` field says a session signed in, not as whom. Or on the
  Control Deck: *Approve* beside the request in the list's member table
  (`POST /api/v1/lists/{address}/members/{member}/approve`).

A request is by mail to `team-subscribe@`, or on the page
`/lists/team@example.com/subscribe` under the base URL. What the confirmation is
sent to is the address being subscribed, so somebody asking in another person's
name makes the list write to that person once and subscribes nobody. It writes
once: one confirmation an hour per address per list. And the list answers at
most twenty requests an hour from one requester - the page's client address, or
the server that handed a mail to this one - so that one stranger cannot spend
the list's answers and close it to everybody else, and at most two hundred an
hour in all, confirmations, refusals, help and requests to leave together; the
first request past a ceiling is logged. The page answers the same sentence
whether or not the address was already there, so it tells nobody who is on the
list.

An account the list's membership query answers is a member already: a request in
its name sends nothing and makes no row, so a stranger cannot keep it from the
list's mail by asking in its name.

A token is 32 random bytes as 64 hexadecimal characters, one per member row,
and is replaced when it confirms. A request nobody confirms or approves lapses
after a week and is removed.

Leaving, and choosing how the mail comes
---------------------------------------

Each member's delivery is one of:

* **each** - every message, as it is distributed;
* **digest** - one message a day at the list's hour; on a list whose digest is
  off, each message, so that switching the digest off does not silence the
  people who chose it;
* **none** - still a member, sent nothing. A person on holiday.

A member leaves on their own page - the https link in `List-Unsubscribe`,
`/lists/unsubscribe?t=<token>` - which also changes their delivery; its GET
only shows the page, and its buttons POST. RFC 8058's one-click - a POST of
`List-Unsubscribe=One-Click` to that link, which is what Gmail's and Yahoo's
unsubscribe buttons send - leaves with no page and no session. A member who
writes to `team-unsubscribe@` from their own address is sent `LIST_LEAVE`, with
their page's link and their token, and nothing more happens until they follow
the link or reply carrying the token: the envelope sender is free text, and
anybody who knew a member's address could otherwise take them off the list.
Having left by mail, they are sent `LIST_UNSUBSCRIBED`. A member with an account
here sees the lists on this server they belong to on the webmail's Subscriptions
page (*Lists on this server*), where they choose their delivery or leave. Every
one of these pages is held to 120 requests an hour from one address; a request
from this machine is not counted.

**An account the membership query answers** has no row, so it chooses no
delivery of its own: it is sent each message. Its copies carry a link of their
own - the address and the list in it, and an HMAC of the address under the
list's own secret, which only this server can make - and at its page, by its
one-click, or by `team-unsubscribe@` answered with that link and token, it can
leave. A row is then kept saying the address has left (state `left` on the
members route), which the query's answer does not get past; a member with a row
of their own whom the query also answers is left with the same row when they
leave, since removing it would put them back on the list at the next posting.
That row is not a member and never lapses. An address that has left comes back
through the administrator, who removes its row - the Control Panel's recipient
list, or COM; a request by mail or page answers it as already there.

What a member's copy carries
----------------------------

A managed list sends every member a copy of their own. The list's copy is made
once from the posting:

* the **subject prefix** (`subject_prefix`, at most 64 characters) is put at the
  start of the subject, once - a subject that already contains it, as a reply's
  does, is left alone;
* **Reply-To** is the list's address when `reply_to` is 1, in place of any the
  posting had, and the posting's own (or none) when it is 0;
* the **footer** (`footer`, at most 2,000 characters, lines kept) is appended to
  the text and the HTML part as the disclaimer adder appends a rule's text - to
  neither part of a signed or encrypted message (S/MIME, PGP/MIME), which
  appending would break, and not again to a reply that already quotes it;
* whatever `List-*` fields the posting brought are taken off.

Each member's copy then carries, before the posting's own fields:

```text
List-Id: <team.example.com>
List-Post: <mailto:team@example.com>              (NO for an announcement list)
List-Help: <mailto:team-request@example.com?subject=help>
List-Subscribe: <mailto:team-subscribe@example.com?subject=subscribe>, <https://lists.example.com/lists/team@example.com/subscribe>
List-Unsubscribe: <mailto:team-unsubscribe@example.com?subject=unsubscribe>, <https://lists.example.com/lists/unsubscribe?t=...>
List-Unsubscribe-Post: List-Unsubscribe=One-Click
List-Owner: <mailto:team-owner@example.com>
List-Archive: <https://lists.example.com/lists/team@example.com/archive>
Precedence: list
X-Loop: team@example.com
X-hMailServer-List-Member: <a tag, see Bounces>
```

The links are there only with a base URL, and `List-Unsubscribe-Post` only with
an `https://` one. `List-Archive` is there when the archive is on. A copy to an
account the membership query answers carries that account's own link (above)
and no member tag, since it has no row to count bounces on. The envelope sender
of every copy is `team-bounces@`, or with VERP the member's own form of it, so a
failure comes back to the manager and never to whoever posted.

A posting that carries this list's own `X-Loop` - a member's server forwarding
it back in, or two lists that are each other's member - is not distributed
again. A posting with a null sender is a report, not a posting, and is not
distributed - and a list's `bounce_address`, which a list without the manager
gives its postings as their envelope sender, is never given to a managed list's
posting nor to a report. Nor is a posting this server marked as spam
distributed; the application log says so, naming the sender, since it goes to
nobody.

**A posting the list manager cannot read** - its file could not be opened or
loaded, as an antivirus or a backup holding it makes happen, or the list's copy
could not be written - is not lost. It stays in the queue and is tried again on
the SMTP retry schedule (`HM7250` says so), and when the retries run out its
sender is told with a class 4 status, `4.3.0`, as when local delivery cannot
save a message: the failure is this server's, not the list's or its members'.

**DKIM, DMARC and the copies.** The list changes the posting - the prefix, the
footer, the fields - so the author's DKIM signature cannot survive on a copy the
list changed; such a copy carries none of the posting's signatures, since a
failing signature is scored worse than none. A copy the list did not change
keeps an outside author's signature, which still verifies. **When the list's
domain has DKIM on, every copy is signed as that domain**, after the list's
fields are in place, so the signature covers `List-Unsubscribe` and
`List-Unsubscribe-Post` - which RFC 8058 asks of a sender before a receiver
offers its one-click, and which Gmail and Yahoo check. A signature of the list's
own domain that the posting carried - made before the list's fields existed - is
taken off for the new one. With DKIM off for the list's domain the copies go
unsigned by it, and the providers do not offer the one-click. ARC sealing
(`DKIMSigner`, as for any forward) applies only with `ArcSealingEnabled` on,
which it is not by default. **The list does not rewrite `From`.** A posting from
outside, from a domain that publishes `p=reject`, reaches outside members with a
`From` that fails DMARC when the list changed the message, and Gmail and Yahoo
may refuse it; Mailman's answer, replacing the author's address with the list's,
is not built. An organisation whose lists take mail from such domains should
know this before it switches a list to managed.

The digest
----------

With `digest` on, the list writes one digest a day at `digest_hour` (0 to 23,
the server's local time; 7 by default) to every member whose delivery is the
digest: a table of contents - each message's subject and sender - and then each
message since the last digest as a part of a `multipart/digest` (RFC 2046, the
MIME form of RFC 1153's digest), with the member's own `List-*` fields. Nothing
new, no digest: a digest that says nothing happened is how people learn to
filter the digest. At most 100 messages and 10 MB a digest; what does not fit
goes in the next one. The task runs every five minutes, so a digest goes within
five minutes of its hour, and never twice within twenty hours.
`DistributionList.SendDigest` over COM sends it now, whatever the hour and
however recently the last one went.

A digest switched on starts from the newest message the archive holds, not the
oldest: a list that kept an archive before its digest was switched on - or had
the digest off for a while, when its digest members were sent each message -
does not send those again. Where the last digest stopped, and when, is written
by the digest alone; saving the list from a dialog opened before the day's
digest does not put the old position back.

The digest is built from copies the list keeps. A list with the digest on and
the archive off still keeps them - there is nothing else to build it from - as
the digest's alone: its archive page answers 404 and never shows them, and each
is deleted once a digest has carried it, or all of them once the digest is off.

The archive
-----------

With `archive` 1 (members) or 2 (public), every message the list distributes is
kept: its list copy as a file under the data directory's `Lists\<list id>\`
folder and a row in `hm_listarchive`. `archive_days` deletes a message that much
older (0 keeps everything), but never one the digest has not yet sent - whether
or not the archive is still on: **switching the archive off takes its pages
away and keeps what they held** until its days run out, and switching it on
again brings it back. Deleting the list deletes its folder and its rows.

* **The page**, `/lists/team@example.com/archive`: the months that hold messages,
  a month's messages (`?month=2026-09`) and one message (`?id=`). A public
  archive answers anyone; a members' archive answers a member's token
  (`?t=`, the one in their `List-Unsubscribe` link) or a webmail session of a
  member, and 403 to anybody else - a member of another list included.
* **For programs**, `GET /api/v1/lists/{address}/archive[?month=]` and
  `GET /api/v1/lists/{address}/archive/{id}`, with the administrator's reads.

A message is shown **as text**: its text part, or for a message with only HTML
the text its HTML holds with every tag, script and style taken out - read in a
single pass, however many tags it has, and then escaped. At most 256 KB of its
text is shown, and a megabyte of its HTML read; past either, a sentence says it
was cut. No HTML from the archive is put in front of a reader, not even in a
sandbox: a list takes postings from strangers. A sender is shown by name, or as
"name at domain", and no member is ever listed. A page lists the months from a
column that holds each message's month and asks for as many messages as it
shows, never every row. There is no IMAP access to an archive and no NNTP: the
archive is the list's, not a mailbox anybody signs in to.

**The archive and the backup.** The rows of `hm_listarchive` are not in the
backup. The files are under the data directory, and a backup with **Back up
messages** copies the whole of it, so they travel with such a backup - a
members-only list's messages included, as plain text - and come back with no
row naming them ([Backup.md](Backup.md)).

Bounces
-------

A delivery report to `team-bounces+jo=example.org@` is about `jo@example.org`,
whatever it says; one to `team-bounces@` is about the final recipients of its
RFC 3464 report. It is read with the server's own report parser
(`DeliveryReportParser`): a `5.x.x` status is a **hard** bounce, a `4.x.x` a
**soft** one, and one whose status cannot be read counts as soft. **Only a
failure is a bounce**: a group whose `Action` is `delayed` - the "still trying"
warning many servers send after a few hours, when the message may yet arrive -
or `delivered`, `relayed` or `expanded`, counts for nothing. Each member is
counted once per report, however many of its groups name them. A report larger
than 2 MB - a server that returns the whole failed message, attachments and all
- is read that far: the report's own part and the returned header come first.

**Anybody can write to the bounce address**, so a report is believed only when it
carries the member's tag: every copy carries `X-hMailServer-List-Member`, a
SHA-256 of the member's token and the list's address, and an RFC 3464 report
returns the failed message's header, where the tag is. This server's own reports
of a refused delivery do; a remote server's usually do. A report without the
member's tag, or about somebody who is not a member, is logged and dropped. The
worst a forger who has seen one of the member's copies can do with a report is
pause that member's delivery, never unsubscribe them - and leaving by mail asks
the member first, so a forged request to leave writes to the member and does
nothing more.

The counts, in `hm_settings`, on `PUT /api/v1/settings`:

| Setting | REST key | Default | What it means |
|---|---|---|---|
| `ListHardBouncesToUnsubscribe` | `list_hard_bounces_to_unsubscribe` | 1 | Hard bounces that stop a member (1 to 100). |
| `ListSoftBouncesToUnsubscribe` | `list_soft_bounces_to_unsubscribe` | 5 | Soft bounces within the window that stop a member (1 to 100). |
| `ListBounceWindowDays` | `list_bounce_window_days` | 30 | The days soft bounces are counted within: a soft bounce more than this after the first of the current count starts a new one (1 to 365). |
| `ListBaseUrl` | `list_base_url` | empty | The base of every link, as above. |

A value out of range is refused, and the stored one kept.

A member who reaches either count is set to **none** - still a member, sent
nothing - with the reason recorded, and the owner is sent `LIST_BOUNCED` once,
at that moment. A successful delivery does not clear a soft count; the window
does. The counts are added where they are stored, so two reports handled at
once both count. The owner's *Reset bounces* on the Control Deck
(`POST /api/v1/lists/{address}/members/{member}/reset-bounces`,
`DistributionListRecipient.ResetBounces` over COM), or the member turning their
delivery back on, clears the counts. The reset sends each message again to a
member **the bounces stopped** - the recorded reason says so - and never to one
who chose to pause, whatever their counts; a member who had chosen the digest
and was stopped by bounces is sent each message after the reset, and chooses
the digest again on their page. The bounce itself goes to no mailbox - the
bounce address has none - and is logged.

**How this meets the server's own suppression list**
([BounceSuppression.md](BounceSuppression.md)). The copies are sent from the
list's domain, so with bounce processing on, a member whose server refuses the
list's mail is counted against that domain and, past its threshold, goes on its
suppression list. With `BounceSuppressionAction` **refuse**, the default, the
copies to them are then refused at delivery, and the report of that refusal
comes back to `team-bounces@` and is counted by the list as a hard bounce; with
**drop** the copies are taken off without a report and the list is told
nothing; with **observe** they are delivered. The two do not share counts: the
suppression list is the domain's, applies to all its mail and is emptied by
hand; the list's counts are the list's and are reset by its owner.

Where it is set
---------------

* **Control Deck** - the list editor: the manager's fields (the footer in a text
  box that keeps its lines), and the member table with each member's delivery,
  state and bounces, *Approve* and *Reset bounces*. The four settings on the
  Settings page.
* **Control Panel** - the list dialog's manager group.
* **REST** - `managed`, `subscribe_policy`, `archive`, `archive_days`, `digest`,
  `digest_hour`, `reply_to`, `subject_prefix`, `footer`, `verp` and `owner` on
  the list routes; a list's `members` there are its confirmed members only - a
  request still waiting, or an address that has left, would otherwise become a
  member of the list a configuration export is applied to. `delivery`,
  `confirmed`, `state` (`member`, `pending`, `awaiting_owner` or `left`),
  `subscribed`, `hard_bounces`, `soft_bounces`, `last_bounce` and `reason` on
  each row of a managed list in `GET /api/v1/lists/{address}/members` (an
  unmanaged list's rows are the address and the source, as they always were);
  the verbs and the archive above; and `GET`, `PUT` and
  `DELETE /api/v1/me/lists[/{address}]` for the reader's own.
* **COM** - `DistributionList.Managed`, `SubscribePolicy`, `Archive`,
  `ArchiveDays`, `Digest`, `DigestHour`, `ReplyToList`, `SubjectPrefix`,
  `Footer`, `Verp`, `Owner` and `SendDigest`; `DistributionListRecipient.Delivery`,
  `HardBounces`, `SoftBounces`, `Confirmed` and `ResetBounces`. A number out of
  range, or a prefix or footer too long, is refused where it is written; an owner
  that is not one address, or is one of the list's own, and a prefix on two
  lines, are refused when the list is saved, in the sentence the save gives.
  COM's `Save` writes a member's address and delivery; the manager's own columns
  - its state, token and counts - are written by the manager alone, so an object
  read before the manager's last write cannot put them back.
* **The backup** carries the list's settings and its secret, and each member's
  delivery, state, token and counts; a list restored from a backup taken before
  the manager is an unmanaged list.

What replaces Mailman, and what does not
----------------------------------------

Replaced: joining and leaving by mail and by page, with confirmation and the
owner's approval; per-member delivery - each message, a daily digest, or a pause;
the RFC 2369 fields, and RFC 8058's one-click where the base URL is https and the
list's domain signs; the subject prefix, Reply-To and a footer; VERP and bounce
counting that pauses a dead address and tells the owner; an archive by month;
moderation of postings (the list's moderator, as before).

Not replaced, deliberately or not yet:

* no posting from the web - a list is written to by mail;
* no configuration of a list by its owner on a page of the list's own: the owner
  works through the Control Deck (a domain administrator's role), approves on the
  approval page, and there is no list password;
* no approval by reply: the owner approves on the page, signed in, or on the
  Control Deck;
* no NNTP gateway and no IMAP access to an archive;
* no `From` rewriting for DMARC, as above;
* no digest in plain RFC 1153 text, only its MIME form; no weekly digest; the
  digest's own table of contents is in English, and so are the list's pages
  (the confirmation, the member's page, the approval page, the subscribe form
  and the archive), as the quarantine digest's pages are - the messages the list
  sends are the translatable server messages;
* the archive is not searchable, and its rows are not in the backup;
* no bounce *probe* before a member is paused, as Mailman sends one: the member's
  tag is what stands in its place;
* a report that returns no header - some old servers send only text - cannot
  carry the tag and is dropped, so a member behind such a server is never paused;
* an account the membership query answers chooses no delivery of its own, and
  its bounces are not counted;
* one owner address per list.

What is tested
--------------

`SMTP/MailingLists.cs`, against a REST listener on port 9966, thirty-four tests
of the Windows suite (the fixture is not linked into the Linux one):

* joining by mail with the confirmation and the reply, with the negative control
  of a posting that does not reach the unconfirmed address; a closed list's
  refusal, which names `team-owner@` and not the owner; the moderated list's
  approval - a reply by mail, even signed in, approving nothing, the page's
  button refused without a session and with another account's, and taken with
  the owner's own; the page and the confirmation link, whose GET does not act; a
  request forged in somebody else's name, answered once and sent none of the
  list's mail; the twenty-first request from one requester in an hour
  unanswered, and another list's answered;
* leaving by mail - the request answered with a link and a token, the reply
  leaving, the notice naming the member - by the token page and by the
  one-click POST, with a wrong token, a request's token and an empty POST
  changing nothing;
* an account the membership query answers: a request in its name making no row,
  a request made before the query answered an address not keeping it from the
  list's mail, and leaving by its one-click and by mail, the others still sent
  the list's mail;
* every field of a member's copy over an https base, the link without the
  one-click over http, and the copy without a base URL; the prefix once, the
  footer on both parts, on neither part of a signed posting and not again on a
  reply quoting it; Reply-To both ways with the posting's own; VERP in the
  envelope of a local and an outside member; a copy signed as the list's
  domain, exactly one signature, covering `List-Unsubscribe-Post` and verified
  with the domain's key;
* a posting that cannot be read kept in the queue and delivered once it can be,
  and its sender told `4.3.0` with no retries left;
* the digest against the member on each message and the member on none, and a
  digest switched on starting from the newest message; the webmail's own
  delivery;
* the archive by month, one message as text with its script gone and its sender
  as "name at domain", switched off and on again keeping its message, the
  members-only refusal, another list's member refused, and the files deleted with
  the list; a message of megabytes of tags shown within seconds and cut;
* a hard bounce with the owner told once and a forged one not counted, a plain
  (non-VERP) report counted only with the member's tag, a delay warning not
  counted, five soft bounces, a bounce about a stranger, the reset, and the
  reset leaving a member's own pause alone;
* the REST and COM round trips and refusals - two owners in one, the list's own
  addresses, and `team-lead@` taken - and the four settings refusing what the
  manager would not use.

`Infrastructure/BackupRestore.cs` carries the manager's settings, a member's
delivery, and a row's state, counts and token through a backup and a restore.
The schema probe (`DBUpdater`) has one probe per column the step adds to
`hm_distributionlists` and `hm_distributionlistsrecipients`, three for the new
table's columns and one for its foreign key. `ListManagerTester` and
`ListArchiveTester` pin the address grammar, VERP both ways, the token rule, the
tag, the query member's token, the base URL rule and the month arithmetic when
the server runs its class tests. `build/deck-script-test.js` and
`build/portal-script-test.js` check the Control Deck's member table and the
webmail's card.

Not tested: the per-list ceiling of two hundred answers an hour; the page budget
of 120 an hour (loopback is exempt from it); the multipart form of the one-click
POST (the tests send it form-encoded); a list closed after a request, and a
request lapsing after a week; the scheduled digest's hour, its twenty-hour
spacing and its cap of 100 messages or 10 MB; `archive_days` retention; a
report over 2 MB; name-owner@ falling back to the catch-all and to
`postmaster@`; `-` as the plus-addressing character beside `team-request@`; an
account, alias or list named `team-request@` winning over the manager; the
members-only archive through a webmail session; COM's `ResetBounces`.
