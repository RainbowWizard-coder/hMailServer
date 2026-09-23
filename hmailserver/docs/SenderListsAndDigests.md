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
| `https://bad.example/anything` | A **URL**: stored as `bad.example/`, and covering a link in the message whose host is `bad.example` or a subdomain of it, whoever sent the message. A leading `www.` is dropped, as the link extractor drops it. |

An entry covers a message when it matches **either** the envelope sender
(`MAIL FROM`) **or** the address in the message's `From` header. The header is
the address a person sees; mail sent through a mailing service carries the
service's own address in the envelope and the brand's in the header, and a list
that looked only at the envelope would never match the newsletter somebody was
trying to block or to rescue.

A message with the null sender — a bounce, a delivery status notification — is
never decided by any list, whatever its `From` header says.

A URL entry is about what the message points at, not who sent it, and is
consulted only when neither address decided anything: the links are extracted
from the body the way the SURBL check extracts them, and the nearest scope with
a URL entry covering any of them decides, with the same precedence and the same
tie-break as for senders. Block, and the message is held or marked for that
recipient like a blocked sender's; allow, and a link the URL blacklists list —
a shortener, a newsletter service — no longer marks the message for that
recipient. The body is read for links only for a recipient whose account,
domain or server has a URL entry, so an installation with none pays nothing.

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

### Where each list is edited

| List | Webmail | Control Panel | Control Deck | REST | COM |
|---|---|---|---|---|---|
| The account's own | Settings → **Safe senders** | — | — | `/api/v1/me/sender-list` | — |
| A domain's | — | Domain → **Allow and block list** tab | Domain editor → **Allow and block list** | `/api/v1/domains/{domain}/sender-list` | `Domain.SenderList` |
| The server's | — | Spam & virus filtering → **Allow and block list** | Lists → **Allow and block list** | `/api/v1/sender-list` | `Settings.AntiSpam.SenderList` |

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

* **URLs** are not list entries. The roadmap row for the server's list names
  "senders, domains and URLs"; only senders and domains are here.
* An administrator cannot make a domain or server block override an account's
  own safe senders. **Blocked senders** is that mechanism.
* The digest is not HTML. (The hour, per domain until 23 September 2026, may now
  be an account's own as well.)
* The Control Panel and the webmail do not edit an entry's expiry.
* The `/held/…` pages are not rate-limited beyond what 256-bit tokens make
  unnecessary.
