Safe senders, allow and block lists, and the quarantine digest
===============================================================

Who decides that a message is wanted, and how a person sees what was held for
them. Three features that are one idea seen from three sides:

* **Safe senders and blocked senders** — each account's own list, kept by the
  account holder in the webmail.
* **Allow and block lists** for a domain and for the whole server, kept by an
  administrator in the Control Panel, the Control Deck or over REST.
* **The quarantine digest** — one message to each account listing what the
  quarantine is holding for it, with a link that delivers each message.

And the administrator's side of the same queue: **reviewing the quarantine**,
where held messages are grouped, released, deleted, and their senders allowed or
blocked on these lists or on the domain white and black list, one message or a
whole burst at a time.

They are one table (`hm_senderlist`, schema 6046) with a scope on each row, one
matching rule, one precedence and one REST shape. Nothing here is on until
somebody adds an entry or switches a domain's digest on.

The list
--------

### What an entry is

An address or a domain, whether it **allows** or **blocks**, why it is there,
and optionally when it stops mattering.

| Entry | Covers |
|---|---|
| `someone@example.com` | That one address, in any case. No wildcards. |
| `example.com` | `example.com` and every subdomain of it — `mail.example.com`, not `notexample.com`, not `example.com.attacker.net`. |
| `@example.com` | The same as `example.com`. Stored as `example.com`. |
| `https://bad.example/anything` | A **URL**: stored as `bad.example/`, and covering a link in the message whose host is `bad.example` or a subdomain of it, whoever sent the message. Its host is read as the link extractor reads a link's: the part after the last `@` (`http://a@b@bad.example/` is `bad.example/`), up to a port, a `/`, `?`, `#` or `\`, percent-decoded as UTF-8, and a host that is not ASCII read as a browser reads it, by UTS 46: the characters a browser deletes from a host deleted (`sh%C2%ADop.example`, with a soft hyphen, is `shop.example/`), fullwidth letters and the ideographic full stops read as the ASCII ones, and a name in Unicode written in its `xn--` form, as DNS carries it (`https://bücher.example/` is `xn--bcher-kva.example/`, and covers a link written either way); with each leading `www.` dropped while two labels or more are left, and an address in dotted decimal - an IPv4-mapped IPv6 address (`http://[::ffff:192.0.2.61]/`) or one under the NAT64 prefix `64:ff9b::/96` is the IPv4 address it carries (`192.0.2.61/`). A host no link can have there — any other IPv6 address, a name with a character a name cannot hold, a name in Unicode that UTS 46 refuses or that is longer than any name can be, a name of one label — is refused. |

An entry covers a message when it matches **either** the envelope sender
(`MAIL FROM`) **or** the address in the message's `From` header. The header is
the address a person sees; mail sent through a mailing service carries the
service's own address in the envelope and the brand's in the header, and a list
that looked only at the envelope would never match the newsletter somebody was
trying to block or to rescue.

A message with the null sender — a bounce, a delivery status notification — is
never decided by any list, whatever its `From` header says.

A URL entry is about what the message points at, not who sent it, and is
consulted only when neither address decided anything: the links are read from
the body the way the SURBL check reads them, each for the host it goes to - the
host after the link's user name, if it has one, without its port,
percent-decoded as UTF-8, a host that is not ASCII read by UTS 46 as a browser
reads it (the characters it deletes from a host - a soft hyphen, a zero-width
space - deleted, fullwidth letters and the ideographic full stop read as ASCII
ones, a name in Unicode in its `xn--` form), and read as it is written too, as
6.3.6 read it, and read by UTS 46 again as far as each place a mail program may
end a link in running text at - the first character past ASCII, as it is
written, of each kind of punctuation and symbol by what UTS 46 maps it to, where
the punctuation the host ends with begins, and the first ideographic full stop
before text past ASCII - so that a link in running text followed by a closing
quotation mark, a guillemet, a dash or CJK punctuation - which UTS 46 takes into
the host's last label - also names the host before that character, where the
mail programs that make the link end it, whatever that host is written in
(ASCII, fullwidth letters, an internationalised name) and whatever symbols it
holds (`http://i♥.ws”` names `xn--i-n3p.ws`); a host in Unicode longer than any
name can be (more than four times 253 code points once the ones a browser
deletes are gone), or holding a character UTS 46 maps to one a host may not
hold (`U+FDFA`'s spaces, a no-break space), is not read by UTS 46 at all; an HTML part's character references read
first, as a browser reads them - every named one HTML defines (`&eopf;vil.example`
is `evil.example`, `b&uuml;cher.example` is `xn--bcher-kva.example`); and an
address in its own order, an
IPv4-mapped IPv6 address (`[::ffff:192.0.2.1]`) as the IPv4 address it carries;
in a text part also the name before a `)`, `"` or `}` in the user name, where a
mail program ends the link, and the name before a zero-width no-break space,
where a webmail's script ends it; a link with no scheme where it opens an HTML
attribute's value (`href="//host/"`) or a query parameter's (`?u=//host`) - and
the nearest scope with a URL entry covering any of them decides, with the same
precedence and the same tie-break as for senders. **Every link counts, however
many come before it:** until 6.3.6 only the first 16 hosts of a message were
matched, so an entry's link after sixteen others was never seen. Block, and the
message is held or marked for that recipient like a blocked sender's; allow, and
a link the URL blacklists list — a shortener, a newsletter service — no longer
marks the message for that recipient. The body is read for links only when a
recipient's account, domain or server has a URL entry, so an installation with
none pays nothing, and it is read **once for the message**, against the URL
entries of every scope, however many of its recipients have one: each
recipient's decision is made from that one reading. Until 6.3.6 the message was
parsed and read again for each such recipient, which a message with a large text
part made costly; now a message costs one reading of its text, as the SURBL
test's does. Each link is looked up by the names an entry for it could have -
`cdn.shop.example`, `shop.example`, `example` - so the reading's time does not
grow with the number of entries. **A message larger than the anti-spam size
limit** (`maximum_message_size_kb` of `/api/v1/settings/antispam`, COM
`AntiSpam.MaximumMessageSize`, 1024 KB unless changed - the size past which the
SURBL test and the other tests after `DATA` do not look at a message at all) is
judged by the links in the first part of
its text and of its HTML, as far as that limit into each: since 6.3.6 the
reading stops there, where it went on to the end of a message of up to the MIME
parser's 80 MB - some 8 seconds of a delivery thread for 20 MB. A link the limit
cuts through is left out rather than read as the part of its host before the
cut, so no host is matched that the whole message does not link to, and a link
after the limit is not seen: a block entry's link there does not hold the
message, and an allow entry's does not rescue it. With the limit set to 0 the
parser's 80 MB is the limit, as it is for the tests.

The matching rule is the one the server-wide **Blocked senders** list has always
used: both now call the same code.

### What allow and block do

The lists are read at **local delivery**, once per recipient, after the spam
tests have run. A conversation shared by several recipients cannot be refused
for one of them, so this is the only place a per-recipient decision can be made.

| | The message was classified as spam | The message was not |
|---|---|---|
| **Allowed** | Delivered unmarked for this recipient: the spam flag, the `X-hMailServer-Spam` and reason headers and the subject tag are removed from this recipient's copy. The account's own mark and delete thresholds do not run. | Delivered as it is. |
| **Blocked** | Held in the quarantine for this recipient, when the quarantine is on. Marked as spam and delivered, when it is not. | The same: held, or marked. |

A block **never deletes**. Where the server holds suspected spam, a blocked
sender's message is held for the account that blocked it, appears on its Held
mail page and in its digest, and can be released; where it does not, it is
marked (`X-hMailServer-Spam: YES`, an `X-hMailServer-Reason-1` naming the list,
the configured subject tag) and lands wherever that account already sends spam.
No score is recorded on a blocked message, because nothing measured it.

A message **released** from the quarantine — by an administrator, from the
webmail or from a digest link — is never held again by a list on its way back
in. A release is a person overruling the filters.

The other recipients of the same message are not affected by one recipient's
list.

**What runs after the list.** The list is the first thing local delivery does
with a recipient's copy, then the account's own spam thresholds, then the
domain's outside-sender tagging ([DomainTransforms.md](DomainTransforms.md)):
the `[EXTERNAL]` subject tag, the `X-hMailServer-External` header and the
first-contact note. So the tagging reads the subject after the list has
removed or added the spam tag. An allowed sender from outside is still tagged
as external, because allowing a sender says the mail is wanted, not that it
came from inside. A blocked sender's copy that the quarantine holds is neither
tagged nor remembered as a correspondent until it is released; one that is
marked and delivered instead, because the quarantine is off, is tagged and
remembered like any other delivered message.

### Precedence

**Nearest first: the account, then its domain, then the server.** The first of
the three that has anything to say about the sender is the only one consulted.

| Account's list | Domain's list | Server's list | Result |
|---|---|---|---|
| — | — | block | blocked |
| — | allow | block | allowed |
| block | allow | block | blocked |
| allow | block | — | allowed |

Inside **one** list, when two entries both match:

1. The more specific entry wins. An address beats a domain; a longer domain
   (`mail.example.com`) beats a shorter one (`example.com`). So an allow for
   `invoices@example.com` is an exception to a block on `example.com`.
2. On an exact tie — an allow on one of the two addresses and a block on the
   other, equally specific — the block wins.

A list never holds two entries for the same value: adding a value that is
already there **rewrites** the existing entry (the REST answer is `200` rather
than `201`). Moving an address from safe to blocked is one action.

An expired entry is ignored whether or not the hourly sweep has removed it yet.

**Why the account comes first.** Microsoft 365 does it the other way: its
tenant block list beats a user's safe senders. Here the message is addressed to
the account holder, who is the person able to tell whether they wanted it, and
an administrator who needs a sender stopped for everyone, whatever anybody's
own list says, already has a mechanism nobody's list can overrule — **Blocked
senders**, which refuses the message during the SMTP conversation before any
recipient's preferences are reachable.

### How this differs from the lists that already existed

| List | When | For whom | What it does |
|---|---|---|---|
| **Blocked senders** (Anti-spam) | During the SMTP conversation | Every recipient at once | Adds a score, 100 by default, which refuses the message. Nobody's own list overrules it. |
| **White list** (Anti-spam) | During the SMTP conversation | Every recipient at once | Skips every spam test for a sender and an IP range. |
| **Domain white and black list** (Anti-spam; [SmtpPolicy.md](SmtpPolicy.md)) | At `MAIL FROM`, before greylisting and the score | Every recipient at once, keyed by the sender's domain or HELO suffix rather than by address | Black refuses the session outright; white skips greylisting and the score's refusals while every test still runs. |
| **Allow and block list** (this) | At delivery | Each recipient separately | Allows or blocks for the recipients it applies to, below their own lists and their domain's. |

### Neither address is proof

Both the envelope sender and the `From` header are whatever the sender wrote.
An allow entry is exactly as forgeable as the address it names: somebody who
knows that a reader trusts `example.com` can write `example.com`. That is true
of every safe-sender list there is. Keep allow entries to correspondents whose
domains publish DMARC, and prefer a single address to a whole domain.

Blocking has the same limit. **Blocked senders** matches the envelope address
the sender claims, so it stops a correspondent who keeps using one address; it
is no defence against spoofing, and stops nothing that changes its address.

### Where each list is edited

| List | Webmail | Control Panel | Control Deck | REST | COM |
|---|---|---|---|---|---|
| The account's own | Settings → **Safe senders** | — | — | `/api/v1/me/sender-list` | — |
| A domain's | — | Domain → **Allow and block list** tab | Domain editor → **Allow and block list** | `/api/v1/domains/{domain}/sender-list` | `Domain.SenderList` |
| The server's | — | Spam filtering → **Allow and block list** | Lists → **Allow and block list** | `/api/v1/sender-list` | `Settings.AntiSpam.SenderList` |

An account's own list is its holder's alone: there is no administrator path to
it, and one account's entry is `404` to another. **Block this sender** and
**Report phishing** in the webmail add the sender to the account's own blocked
senders. (They used to write a rule into the account's Sieve script — a move to
Junk, or a *discard* when there was no Junk folder, which destroyed the message
and, for a phishing report, the evidence. A rule could not cover a domain, could
not expire, and was invisible to an administrator asked why a sender was not
getting through. Rules written that way before are left exactly where they are.)

Expiry can be set over REST and in the Deck; the Control Panel and the webmail
do not offer it yet.

The server's list is also written from the quarantine: **Whitelist** and
**Blacklist** on a held message, a group or the checked messages put the sender's
address on it as allowed or blocked, and then release or delete (see
[Reviewing the quarantine](#reviewing-the-quarantine)).

One thing writes to the server's list by itself: a **spam trap**. A message to
an address that was never real puts its envelope sender on the server's block
list with an expiry, under a reason naming the trap and the address it came
from - unless this list already allows that sender, in which case the
administrator's decision stands and the hit records that it did. See
[SpamTraps.md](SpamTraps.md).

### REST

All three list routes answer the same object and take the same body.

```
GET    /api/v1/sender-list
POST   /api/v1/sender-list          {"value": "example.com", "blocked": true,
                                     "reason": "…", "expires": true,
                                     "expires_time": "2026-12-31 23:59:59"}
PUT    /api/v1/sender-list/{id}     any subset of the same fields
DELETE /api/v1/sender-list/{id}
```

The same four verbs under `/api/v1/domains/{domain}/sender-list` and
`/api/v1/me/sender-list`. An entry is `{id, scope, value, blocked, reason,
expires, expires_time, expired, created}`. A value that is not one address or
one domain, an unknown field, or an expiry without its time is a `400` naming the
problem; nothing is stored. A list holds at most a thousand entries.

A read-only API key cannot write any list. A key restricted to named domains
can read and write those domains' lists and cannot reach the server's. An
account holding a role reads a list with `view_configuration`, writes a domain's
with `manage_accounts` in that domain and writes the server's with
`manage_settings`: a domain administrator keeps its own domains' lists, and an
auditor reads every list and writes none. See [Roles.md](Roles.md).

Reviewing the quarantine
------------------------

The Control Panel's **Quarantine** page and the Deck's **Quarantine** view are the
review queue: every message the server held rather than delivered, newest first,
at most the thousand newest over REST and the five hundred newest over COM - and
the search reaches every one of them, however old (*Searching*, below).
Quarantining is off until `QuarantineEnabled` is on in `hMailServer.ini`; until
then spam over the delete threshold is refused during the SMTP conversation and
nothing is held.

### What each row shows

| Column | What it is |
|---|---|
| **Held** | When it was held. |
| **HELO** | The HELO or EHLO name the client gave in the SMTP session that brought the message in, as it gave it (schema 6105, `hm_quarantine.quarantinehelo`). A message held during the session records the session's own name; one held later, at delivery - a rule, a block list above, an account's own spam threshold, the sandbox, impersonation protection - takes the name from the `Received` header this server wrote when it accepted the message, and only from this server's own header, never from one another server wrote. Empty for a message held before 6105, for a client that gave no HELO, and for a message that did not arrive over SMTP. |
| **Sender** | The envelope sender; `<>` is the null sender. A long address ends in an ellipsis and its tooltip is the whole of it. |
| **Recipients** | The first envelope recipient and how many more; the tooltip lists them all. |
| **Score** | The spam score it reached; 0 for a hold that was not a score's (a rule, a block list). |
| **Subject** | The subject, read from the held file. |

Why it was held is the row's tooltip in the Control Panel and a line under the
subject in the Deck. Every column sorts from its header; a sort orders the
messages **inside** each group and leaves the groups where they are.

The icon before the HELO name opens the message's **headers**, and the icon before
the subject a **plain-text preview**: its text part, decoded, or when it has none
its HTML part with the markup, style sheets and scripts taken out. Nothing is
rendered as HTML, anywhere. The headers are cut at 65,536 characters and the
preview at 16,384, and each says so when it was cut. If those and the columns are
not enough to decide, release the message to the person it was addressed to rather
than guess.

### Groups

**Group by** folds the rows under their **sender domain** (the default), their
**HELO domain**, or **None**. The domain is the organization's - the registrable
domain by the Public Suffix List, lower-cased - so `bounces.notify.example` and
`notify.example` are one group, and a Microsoft 365 connector's
`mail.protection.outlook.com` HELO names are all `outlook.com`. The server works
it out (`sender_domain` and `helo_domain` over REST, `SenderDomain` and
`HeloDomain` over COM) so that every page groups a message the same way. Messages
with no sender domain (the null sender) or no HELO domain (none recorded, or an
address, bracketed or not) are one group of their own. The group whose newest message is
newest comes first. A group's row folds and unfolds; folding hides its messages
and keeps their checks.

### Searching

The search box over the list finds a message whose **sender**, **recipients**,
**subject** or **reason** - why it was held - holds what is typed, in any case. The
text is matched as it is written: `%`, `_`, `[` and `/` are the characters they are
on every database, never wildcards, and the spaces round it are not part of it. A
text matched across two columns is no match; each column is searched on its own, as
the row stores it - the envelope sender, the recipients as they were accepted
(comma separated, cut at 1,000 characters), the subject and the reason, each cut at
255. Letters outside ASCII are matched in any case as far as the database folds
them.

While the page holds every message held, the search narrows its rows as you type.
While it holds only the most recent - the five hundred newest in the Control
Panel, the thousand newest in the Deck - it asks the server a moment after the
typing stops, so that a message older than the page is found too, and says so:
*Searched every held message on the server: 37 match the search.* Where more match
than a page holds, the most recent of them are listed and the notice says how many
match in all; a longer search narrows them. A server older than the Control Panel
cannot search, and the page then says that it looked at the most recent only. A
search is the list's, and is not on the audit trail, as the list is not
([AlertsAndAuditTrail.md](AlertsAndAuditTrail.md)): the quarantine is one of the
server's working lists. Reading a held message's headers or its text is recorded.

An account's own held mail - what the webmail's **Held mail** page lists - is the
newest thousand messages held for that address, however many others the server
holds (its `total` says how many are held for it; `offset` and `limit` page through
the rest), where it was the newest thousand held for anybody with the account's
kept. `GET /api/v1/me/quarantine?q=` searches every message held for the address,
in the sender, the subject and the reason. Never in the recipients: they say who
else a message was for, which is not the account's to learn.

### The four actions

Every message, every group and the header carry the same four icons:

| Icon | On a message | On a group | In the header |
|---|---|---|---|
| Green tick | Release it to its recipients | Release every message in the group | Release every checked message |
| Red cross | Delete it | Delete the group | Delete the checked messages |
| Open list with green lines | **Whitelist** its sender, then release it | The same for each distinct sender or domain in the group | The same for the checked messages |
| Dark list with red lines | **Blacklist** its sender, then delete it | The same | The same |

The box before each row checks it; a group's box checks every message in it,
folded or not, and shows a dash when only some are; the header's box checks
everything. The header's icons act on every checked message; with nothing
checked they say so and do nothing.

**Delete asks once**, Yes or No, with **No** as the default for Enter and Escape
in the Control Panel and in the Deck (whose dialog is its own, because the
browser's `confirm()` answers OK to Enter). The sender was told `250`, so nothing
will retry and there is no other copy. A release is not asked about.

**Whitelist and blacklist ask which list**, and write one of the two that already
exist - never a third:

| Choice | What is written | Read when | What it does |
|---|---|---|---|
| **The sender's address** (the default) | The address on the **server's allow and block list** (`hm_senderlist`, above) | At delivery, for every recipient | Allowed: delivered unmarked, and the accounts' own thresholds do not run. Blocked: held for the recipient where the quarantine is on, marked where it is not. |
| **The domain** | `.example.com` - the organization's domain and everything beneath it - on the **domain white and black list** ([SmtpPolicy.md](SmtpPolicy.md)), matched on the envelope sender's domain; when the page is grouped by **HELO domain**, the HELO name's organization instead, matched on the HELO name | At `MAIL FROM` | White: not greylisted, not refused or held on the spam score. Black: refused with `550` before anything else runs. |

The address is the narrow choice and the default. It has one limit worth knowing:
the server's allow list is read at delivery, so a message whose **server-wide**
score reaches the delete threshold during the SMTP conversation is held there
before the list is read. The domain white list is the one that stops that.

Before anything is written, the dialog asks the server what the choice would
write (a dry run) and lists it: one entry per distinct sender or domain, whether
each is new, already there, or on the list the other way (an entry already there
is changed to this action rather than duplicated), and the messages no entry can
be made for - the null sender, no HELO recorded, a HELO that is an address - which are
then left where they are. A domain that is itself a **public suffix** (`co.uk`,
a one-label HELO name such as `localhost`, a name from the list's private section
such as `github.io`) would cover every domain registered beneath it, so the
dialog names it and writes it only after a box is ticked. There is no list of
free-mail providers in the server to warn about `gmail.com` and the like; that is
what the default, the sender's address, is for. Blacklist then deletes, so in its
dialog Cancel has the keyboard.

The entries are written first and the messages acted on after, so a message whose
entry could not be written - the server's list full at a thousand entries, a
database error - is left in the quarantine with the reason, rather than released
with its sender never allowed. An entry written from here carries the reason
*Added from the quarantine review.* and is in the audit trail as any list entry is.

**Remove expired** deletes now what the retention sweep would (everything held
longer than `QuarantineRetentionDays`; 0 keeps everything). **Refresh** reads the
queue again. **Digest settings** is the two settings below: a dialog in the
Control Panel, the Settings view filtered to them in the Deck.

### REST and COM

```
GET  /api/v1/quarantine                 each message: id, sender, recipients, subject,
                                        reason, score, size, created, helo,
                                        sender_domain, helo_domain
GET  /api/v1/quarantine?q=invoice&offset=0&limit=100
                                        {"query", "held", "total", "offset", "count",
                                         "messages"}
POST /api/v1/quarantine/bulk            {"ids": [12, 13], "action": "allow",
                                         "list": "domain", "domain_from": "helo",
                                         "dry_run": false, "confirm_public_suffix": false}
POST /api/v1/quarantine/expire          {"removed": 3, "retention_days": 30}
GET  /api/v1/quarantine/{id}/headers    {"id", "headers", "truncated", "limit"}
GET  /api/v1/quarantine/{id}/preview    {"id", "text", "truncated", "from_html", "limit"}
```

`bulk` takes 1 to 1000 ids (an id named twice is acted on once) and `action` -
`release`, `delete`, `allow` or `block`; `list` (`address` or `domain`) and
`domain_from` (`sender` or `helo`) go with `allow` and `block` only. It answers
`200` with `entries` (`value`, `list`, `match_on`, `public_suffix`, `outcome` -
`added`, `changed` or `unchanged`, or `add`, `change` or `unchanged` for a dry
run - `error`, `ids`) and `results` (`id`, `ok`, `outcome` - `released` or
`deleted` - and `error`), with `succeeded` and `failed`; a request that is not one
is a `400` and changes nothing, and an entry naming a public suffix without
`confirm_public_suffix` is a `409` naming it and changes nothing.

The list without parameters is the bare array it always was. With any of `q`,
`offset` and `limit` it is a page of **every** held message, newest first (of
those held in one second, the last held first), as an object: `query`, `held`
(every message held), `total` (the ones that match), `offset`, `count` (the ones
on this page) and `messages`, each as the list writes it. `q` is looked for as
*Searching* above says, and an empty `q` matches every message, so
`?offset=1000&limit=1000` is the page after the list's, which is in the same
order. `q` is at most 255 characters; `offset` is a whole number from 0 to
999999999 and `limit` one from 1 to 1000 (100 if it is left out); anything else
is a `400`, never a page quietly cut or moved. The search is the list's route:
the same permission, and not on the audit trail.
`GET /api/v1/me/quarantine` takes the same three for the signed-in account's own held
mail (`limit` 1000 if it is left out) and answers `enabled`, `query`, `total`,
`offset`, `count` and `messages`, with `q` looked for in the sender, the subject and
the reason only.

All five are server-wide and refused for a credential confined to named domains.
The list and the headers and preview need `view_quarantine`, and the headers and
preview `view_mailbox` beside it, since they are the mail itself; `bulk` and
`expire` need `manage_quarantine`, and `allow` and `block` `manage_settings`
beside it, because they change who is let in for everyone - the same permission
the two lists' own routes ask for.

Over COM, `Settings.AntiSpam.Quarantine.Bulk(request)` takes and answers the same
JSON, `DeleteExpired()` is the sweep, and each `QuarantinedMessage` has `HeloName`,
`SenderDomain`, `HeloDomain`, `ReadHeaders()` and `ReadPreview()`.
`Search(Text, Offset, Count)` loads `Count` (1 to 500) of the messages that match
`Text` (at most 255 characters, matched as *Searching* says), newest first, after
the first `Offset` (0 to 999999999), which `Item` then indexes as it indexes the
page `Refresh` loads, and returns how many match in all; `Search("", 500, 500)` is
the page after `Refresh`'s, and `Refresh` gives the newest page back. A value out of
its range is a COM error, and the page loaded before stays as it was. Server
administrators. The HELO name is `HeloName` and not `Helo`: a type library keeps
one spelling of a name whatever its case, and `Client.HELO` has it, so a member
spelled `Helo` would have renamed `Client.HELO` in every early-bound program.

The quarantine digest
---------------------

### What it is

Once a day, one message to each account of a domain that has something **newly**
held for it — held since its last digest — listing each message's subject,
sender, when it was held and why, with:

* a link under each entry that delivers that message;
* when there is more than one, a link that delivers everything the digest listed
  (and nothing held after it was written);
* a footer link that stops the digest for that account, which is also the
  message's `List-Unsubscribe` header (with `List-Unsubscribe-Post`, so a mail
  client's unsubscribe button works in one click).

An **empty digest is never sent**. An account that has had one is not sent
another for twenty hours, and never one that repeats what it was already shown.

The message is `text/plain`, UTF-8, from the domain's postmaster (or
`postmaster@<domain>`) with the null envelope sender and `Auto-Submitted:
auto-generated`. It has no image, no HTML and no link to anywhere but this
server's two digest pages, and it never asks for a password.

### Turning it on

Three settings, all in the database (`hm_settings` and `hm_domains`); none is in
`hMailServer.ini`.

| Setting | Where | Default |
|---|---|---|
| **The address the links point at** — `QuarantineDigestBaseUrl`, such as `https://mail.example.com` | Control Panel → Quarantine → **Digest settings**; COM `Settings.QuarantineDigestBaseUrl` | empty — **no digest is sent** |
| **How long a link lives** — `QuarantineDigestLinkHours`, 1 to 720 | the same | 72 |
| **Send a digest** and **the hour** (0–23, local time) — per domain, and per account over it | Control Panel → Domain → **Quarantine digest** tab; Deck → domain editor; REST `quarantine_digest`, `quarantine_digest_hour` on the domain; COM `Domain.QuarantineDigest`, `Domain.QuarantineDigestHour`. An account's own switch or hour, over its domain's: Control Panel → Account → **Levels**; Deck → account editor; REST the same two names on the account (null takes the domain's); COM `Account.SettingOverride` ([SettingLevels.md](SettingLevels.md)) | off; 7 |

The quarantine itself (`QuarantineEnabled`) has to be on: with nothing held there
is nothing to list. The address must be an absolute `http://` or `https://` URL
of the listener the webmail is served from; anything else is refused when it is
set, because a link nobody can follow is worse than no message. If a domain has
the digest on and the address is empty, the server says so once in the
application log.

### When it is sent

The task wakes every five minutes. In a domain's digest hour it sends only its
share — the accounts still waiting, divided by the runs left in the hour — so a
domain of a thousand accounts is spread across the hour instead of arriving as
one storm on a server that is also taking the morning's real mail. At most five
hundred digests go in any one run. A restart does not send anybody a second
digest.

An account with a switch or an hour of its own is sent at its own hour, and its
domain's other accounts at the domain's; a domain with the digest off is still
looked into for its accounts that switched it on for themselves.

To send now — for a new setup, or to see what one looks like — call
`AntiSpam.SendQuarantineDigests()` over COM. It ignores the hour and the
twenty-hour rule and nothing else: an account that opted out, a missing address
and an empty digest still send nothing. It returns how many were sent.
`AntiSpam.SendQuarantineDigestsAtHour(hour)` runs the pass as the scheduler would
in that hour - each account at its own hour, nothing spread, the twenty-hour
rule not asked.

### The links

A link is a capability: anybody holding the message can follow it. So each one
is 32 random bytes recorded against **one account and one entry**, usable
**once**, and dead after `QuarantineDigestLinkHours`. It is not an id and not a
signature over an id — there is nothing to guess or forge. An unknown, used,
expired, deleted or another account's token all get the same answer.

**Following a link does not act on its own.** It opens a page that says what it
will do — *Deliver this message to your inbox? From X: subject*, *Deliver the 3
messages the digest listed?*, *Stop these digests?* — with one button. Mail
security products follow every link in a message to scan what is behind it; a
link that released mail on its own would have one of them releasing every held
message, spam included, into every inbox. The page has no script, loads nothing
remote, cannot be framed and sends no referrer.

A released message is delivered to that account only; the entry stays for the
message's other recipients.

### Stopping it

Each account decides for itself whether the digest reaches it: the footer link,
the mail client's unsubscribe button, or Settings → **Safe senders** → *Send me
the digest* in the webmail (`GET`/`PUT /api/v1/me/quarantine-digest`). Stopping
the digest does not stop mail being held — it is still on the Held mail page.

What this does not do
---------------------

* **A link whose host a browser refuses is not refused here.** A host in
  Unicode with a label UTS 46 refuses - a character it disallows, Latin and
  Hebrew in one label - goes nowhere in a browser; the link is still read, for
  the name before such a label and the labels after it, and a URL entry for
  either covers it. Until 6.3.7 every label in Unicode was read that way, so a
  link written in fullwidth letters named nothing, and one to an
  internationalised name only the labels after its last label in Unicode.
* An administrator cannot make a domain or server block override an account's
  own safe senders. **Blocked senders** is that mechanism.
* The digest is not HTML. (The hour, per domain until 23 September 2026, may now
  be an account's own as well.)
* The Control Panel and the webmail do not edit an entry's expiry.
* The `/held/…` pages are not rate-limited beyond what 256-bit tokens make
  unnecessary.
