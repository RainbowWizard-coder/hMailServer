The own-reputation watch
========================

The first thing most administrators learn about their server being on a block
list is a bounce — usually forwarded by the person who was waiting for the mail,
usually days after delivery started failing quietly for everyone else. The
lookup that would have told them is the same one every receiving server makes,
and it costs one DNS query per address per list.

So this server asks, once a day, about the addresses it sends from, and raises
an alert when one of them is listed.

**On in a new installation, off after an upgrade.** With no delivery bind address,
no rule binding one and nothing in `ReputationWatchAddresses`, it has nothing to
look up and makes no query at all — which is a stock installation, so an
installation that ships it on starts checking at the moment somebody points it at
an address. An **upgrade** seeds it off instead: an existing server may already
have a delivery bind address, and an upgrade that turned this on beneath it would
begin sending that address to four third parties every day without anybody
choosing it. On an upgraded server, turn it on when you want it —
`reputation_watch_enabled` on the Deck's Settings page, in `PUT /api/v1/settings`,
in `hmctl settings set`, or `Settings.ReputationWatchEnabled` over COM.

Where the addresses come from
-----------------------------

Three places, and none of them a guess:

| Source | What it is |
|---|---|
| `bind` | `SMTPDeliveryBindToIP`, the address outbound SMTP binds to. When it is set, it is the address the internet sees this server as. |
| `rule` | The **bind to address** action of any active global rule, for the mail that leaves from somewhere else. |
| `setting` | `ReputationWatchAddresses`: the addresses an administrator says this server is seen as. |

The third exists because of NAT. A server behind it binds a private address and
leaves through a public one it cannot see, and nothing on the machine knows
which. So it is told.

**Why the public address is not discovered.** Two obvious ways were rejected:

* **Asking a "what is my address" web service.** It tells that service, every day,
  that this machine exists and is running a mail server. Its answer is the
  address an *HTTP* request left from, which on any host with policy routing, a
  proxy, a VPN or a firewall that maps port 25 separately is not the address
  *SMTP* leaves from. And the answer is unauthenticated: a wrong one produces a
  clean report about an address that is not this server's, which is the one
  failure this feature must not have.
* **Enumerating the machine's own interfaces.** Behind NAT they are private. On a
  directly connected host they are often several, and which one is used is the
  routing table's business, not the mail server's. On Windows client editions the
  temporary IPv6 addresses (RFC 8981) are new every day, so the set would be
  noise where it was not simply wrong.

An address that is **private, loopback, link-local, shared (CGNAT 100.64/10),
multicast, reserved, unspecified, or an IPv4 address written as IPv6** is never
looked up. It is not what another server sees, and a query about it would tell
the list operator what this network looks like inside. The report names each one
with the reason, rather than leaving it out silently.

The watch checks at most **32 addresses**, counted across all three sources
together — so a server with many rules that bind an address can reach the cap
without anybody naming 32 addresses anywhere. An address past it is reported as
skipped with the reason `too_many`, for the same reason: an address nobody is
checking that nobody is told about is the same failure as one reported clean.

Which lists
-----------

`ReputationWatchLists` holds one entry per list:

```
[ipv4:|ipv6:]zone[=codes]
```

separated by commas, semicolons or whitespace.

* **zone** — what the query is built under: the reversed address, a dot, the zone.
* **ipv4: / ipv6:** — the list covers only that family. Without it, both. This
  matters: an IPv4-only list answers NXDOMAIN for every IPv6 address, which reads
  exactly like *not listed* and is not the same thing. A pair the list cannot
  speak about is reported as `not_applicable`.
* **=codes** — which answers in `127.0.0.0/8` mean listed: `127.0.0.2`, a range in
  the last octet `127.0.0.4-7`, a wildcard `127.0.0.*`, several separated by `|`.
  Without them, any answer in `127.0.0.0/8` that is not an error code means
  listed. They exist because a few lists answer `127.0.0.1` for something else
  entirely — a *welcome* list — and that must not read as a block.

### What ships, and why

| Zone | Families | Why it is a default |
|---|---|---|
| `ipv4:zen.spamhaus.org` | IPv4 | The list most receiving servers use. Free below Spamhaus's own limits — **and it refuses queries that arrive through a public resolver** (see below). Spamhaus answers IPv6 queries on its Data Query Service zone, not on the free public one, so the default says IPv4 and nothing about IPv6. |
| `ipv4:bl.spamcop.net` | IPv4 | Free, no registration, answers an ordinary resolver. |
| `ipv4:psbl.surriel.com` | IPv4 | Free, no registration. |
| `ipv4:bl.mailspike.net` | IPv4 | Free for low volume. |

**Nothing shipped covers IPv6, on purpose.** An unmarked zone is asked about an
IPv6 address, and an IPv4-only list answers `NXDOMAIN` for one — which reads
exactly like *not listed*. A default that reported an IPv6 address as clean
because no list had ever looked at it is the one failure this feature must not
have, so every shipped zone is marked `ipv4:` and an IPv6 outbound address reads
as `not_applicable` against all four until a list that covers it is named. The
RFC 5782 probe below cannot catch this either: it asks about `127.0.0.2`, an IPv4
entry, so a zone that answers IPv4 correctly passes it whatever it does with
IPv6. If you send over IPv6, add a zone whose IPv6 coverage you have read —
a Spamhaus DQS zone is one — and drop the `ipv4:` from it.

Deliberately **not** shipped, each one line for an administrator who wants it:

* `b.barracudacentral.org` — free, but every resolver that queries it must be
  registered first, so on a stock install it would answer nothing.
* The **Abusix** and **Spamhaus DQS** zones — each carries a subscriber key in the
  zone name, so there is no default to ship. DQS is also an answer to the
  public-resolver refusal below, and the only one of these that covers IPv6:
  put `<key>.zen.dq.spamhaus.net` in the setting.

  **Read this before you do.** The key is then part of an ordinary server
  setting, and `reputation_watch_lists` is not treated as a secret anywhere: it
  is returned by `GET /api/v1/settings` to any credential with
  `view_configuration`, it is written into the audit trail in clear when it is
  changed (the trail redacts by name, and this name is not one of the redacted
  ones), it travels in a configuration export from `hmctl config`, and the zone
  text is copied into `hm_reputationchecks` and echoed by `GET /api/v1/reputation`.
  A subscriber key put here is readable by everybody who can read the
  configuration. The alternative that has none of those properties is the one in
  the next section: point `DNSServer` at a resolver of the server's own and keep
  the free public zone.
* `dnsbl-2`/`dnsbl-3.uceprotect.net` — they list whole networks for a neighbour's
  behaviour, so a listing says little about this server. Level 1 lists single
  addresses and is a reasonable addition.
* **SORBS** — shut down in 2024. A retired zone is exactly the case the test
  entries below exist to catch.

A list's own usage policy is the list's to change: read it before adding one, and
before relying on one that is shipped. The watch makes at most two queries per
list per day plus one per address, which is inside every free policy named here.

Telling a refusal from a listing, and neither from clean
--------------------------------------------------------

This is the part that earns the feature. Before any address is looked up, each
list is asked about the two entries **RFC 5782 section 5** requires every list to
have: `127.0.0.2`, which MUST be listed, and `127.0.0.1`, which MUST NOT be.

| What comes back | Status | What it means |
|---|---|---|
| An answer in `127.255.255.0/24` | `refused` | The list refused the query. Spamhaus's codes: `.252` the zone name is mistyped, `.254` the query arrived through a public or open resolver, `.255` too many queries from this resolver. The list's TXT is kept as the reason. |
| The lookup failed | `lookup_failed` | A timeout, a SERVFAIL, no resolver. The reason says which. |
| `127.0.0.2` is not listed | `list_not_answering` | The list is not answering *this* resolver: a retired zone, a resolver that filters it. "Not listed" from it would mean nothing. |
| `127.0.0.1` **is** listed | `list_lists_everything` | The zone answers for every address — what a retired list's name does once somebody else owns it. Its listings are not believed. |
| An answer outside `127.0.0.0/8` | `unexpected_answer` | Not a block list answer at all. A resolver that answers unknown names with a search page does this. |

Only when both test entries answer correctly is an address's own answer taken at
face value: `listed`, or `not_listed`.

The IPv6 test entry (`::FFFF:7F00:2`) is **not** used. Lists implement it
unevenly, and a list that answered the IPv4 pair correctly is answering this
resolver, which is what the probe is for.

### The public-resolver problem

Spamhaus — and it is not alone — refuses queries that reach it through Google
Public DNS, Cloudflare, Quad9 and the other large open resolvers, answering
`127.255.255.254`. If this server's `DNSServer` is one of those, the daily check
will report Spamhaus as **refused** every day, and say so rather than reporting
the server as clean. Two ways out:

1. Point `DNSServer` at a resolver of this server's own (an unbound or a BIND on
   the machine, or the network's own recursive resolver).
2. Use the list's key-based service and put its zone in `ReputationWatchLists`.

### A lookup that fails never clears a listing

Each row keeps `listed_since`: the moment the last *definite* answer found the
address listed. A check that is refused, times out, or reaches a list that is not
answering leaves that alone — so the report still says the address is listed, the
alert stays raised, and nobody is told the server is clean because nobody
answered.

**But not for ever.** A zone that is retired under the server's feet answers
nothing from then on — SORBS above is exactly that case — so a listing it once
gave would hold the alert raised until somebody edited the setting, years after
the address had been delisted everywhere. Each row therefore also keeps
`confirmed`: when a list last said something *definite* about that pair, whether
listed or not. After **a week** with no definite answer, the pair stops counting as
listed.

It does not become clean. It is `unknown`, which is the third value the whole
feature exists to keep separate: `hmctl` still exits 1 for it, the Deck still draws
it as a warning, and the report still answers `listed` as `null`. The only thing
that changes is that the alert can now clear, and that the server stops asserting a
listing nothing has confirmed for a week. The grace is measured from the last
definite answer rather than from the start of the listing, so an address listed for
a year and confirmed this morning is not aged out the moment its list goes quiet.

The settings
------------

All three are ordinary settings in `hm_settings`: on the Control Deck's
**Settings** page, in `PUT /api/v1/settings`, in `hmctl settings set`, in
`hmctl config`'s document, and on COM's `Settings` object.

| Setting | Default | What it is |
|---|---|---|
| `ReputationWatchEnabled` | on in a new installation, off after an upgrade | The daily check and the alert. A check asked for by hand still runs with it off. Nothing is *raised* with it off, because the rows are as old as the day it was switched off — but a standing `reputation.listed` is **cleared**, within a minute, with a message saying the watch has been switched off and that whatever it reported may still be true. Otherwise switching the watch off in reaction to the alert — the likeliest reaction there is — would leave the condition raised for ever, since every path that could end it is behind this setting. |
| `ReputationWatchLists` | the four zones above | One entry per list, as above. Empty means nothing is checked. |
| `ReputationWatchAddresses` | empty | The addresses this server is seen as, beside the bind address and the rules'. |

A list or an address the watch cannot use is refused **where it is written** — over
COM, over REST and on the Deck — in the same sentence, rather than being stored
and skipped every night in a log nobody reads.

What is recorded
----------------

One row per address per list in `hm_reputationchecks` (schema 6049), rewritten by
each check: the address, where it came from, the zone, the status, the answer,
the list's reason, when it was checked, when a list last said something definite
about it, and since when it has been listed. Rows for an address or a list that is
no longer configured are removed by the next check.

The table is **not** on the audit trail: the watch rewrites it at every check under
no actor, so listing it would record every lookup. What an administrator *chose* —
the three settings — is in `hm_settings`, which `PropertySet` records with the
value it had.

The alert
---------

`reputation.listed`, shipped **on**, sent at once rather than held for the digest
(a listing is what makes mail bounce, and the check runs once a day, so the
digest could only delay the one message that matters). See
[AlertsAndAuditTrail.md](AlertsAndAuditTrail.md) for what a rule can be changed
to.

* It is raised from the **stored rows**, so the alert pass never makes a DNS query,
  and again the moment a check finishes, so a check asked for by hand tells the
  administrator at once.
* The summary names every address and the list or lists it is on; the detail
  names the answer, the moment the listing was first seen, the list's reason and
  what to do about it.
* A **further** listing while the condition is already raised writes a new event,
  because a second list listing the server is news; the same set of listings does
  not. The rule's cool-down decides whether that second message is sent at once
  or held for the digest.
* It clears at the first check that finds no address listed on any configured
  list. A check that could not reach a list does not clear it.
* **The clear says why it cleared, and only one of its sentences is an all-clear.**
  "No outbound address of this server is on any of the block lists it checks" is
  written only when every configured pair gave a definite answer. When some list
  refused, or never answered, or a pair has not been checked at all, the clear
  says that instead and counts them; when the only thing that changed is that a
  listing aged out after a week with nothing confirming it, the clear says that
  too, and says in so many words that it is not the same as a delisting. Nothing
  stored at all, and a watch that has been switched off, each have their own
  sentence. A cleared alert is read as an all-clear, so it may only be one when
  there are answers to support it.

Where to look
-------------

| Surface | |
|---|---|
| REST | `GET /api/v1/reputation` — the report. `POST /api/v1/reputation/check` — check now, `409` when one is already running. `view_configuration` to read, `manage_server` to check; both refused to a credential confined to named domains, because the addresses this server sends from are not one domain's business. |
| Control Deck | **Outbound reputation**: a row per address and list, the status as a pill, the answer, the reason, and the addresses that are not looked up. **Check now** for a check. |
| `hmctl` | `hmctl reputation status`, `hmctl reputation check`. Both exit 1 when an address is listed, when a list could not be asked, **and** — while the watch is on — when a pair has not been checked at all, since a list or an address added since the last check has no answer for up to a day. With the watch **off** an unchecked pair is not a fault but the whole arrangement, so the output says the watch is off and names the setting, and the exit code is 0; a listing or an unanswered list still exits 1, because those are answers an earlier check got. |
| PowerShell | `Get-HmReputation` (with `-Results`, one object per address and list), `Invoke-HmReputationCheck`. |
| COM | `Settings.ReputationWatchEnabled`, `.ReputationWatchLists`, `.ReputationWatchAddresses`; `Utilities.GetReputationReport()` and `Utilities.RunReputationCheck()`. `RunReputationCheck` fails at once with *a reputation check is already running* rather than waiting, for the reason the REST route answers `409`: a caller queued behind a check would hold an out-of-process COM call open for the whole of it. |

The report answers `listed` as `true`, `false` or **`null`**, and the third one is
the point: it is what a list that refused, was not answering, or could not be
reached says about an address. A client that folds `null` into `false` reports a
clean server that has never been checked.

When the check runs
-------------------

A task wakes every hour and checks when the last check was; it runs when that is
a day ago, or never. The period is a day and the wake is an hour on purpose: a
task that simply ran every twenty-four hours would check at start-up on a server
that is restarted nightly, and never on one restarted more often — the period
would be the uptime rather than a day.

A check makes its lookups while it runs, so `POST /api/v1/reputation/check` is the
slowest request this API has on a server whose resolver is not answering: two
queries per list, then one per address per list, each bounded by `DNSQueryTimeout`
(ten seconds by default; `0` means wait for ever).

**A check is bounded in time: 120 seconds.** Twenty lists against 32 addresses is
over a thousand lookups, and the scheduled check runs on the scheduler's single
thread, which runs its due tasks one after another — so without a bound a resolver
that answered nothing would hold the alert pass, the backups, the certificate
renewals and the DKIM key rotation behind it for hours. The budget is tested
*between* lookups, so it bounds how many a check makes rather than how long any one
of them takes. The pairs a check does not reach are written as `lookup_failed` with
the reason — never as clean, never as a listing — and the check's `run` member
answers `complete: false`. The next check asks again.

**A second check is refused rather than queued.** One check runs at a time; a
`POST /api/v1/reputation/check` that arrives while one is running is answered
`409` with a sentence, rather than holding an HTTP request open for the whole of
somebody else's check. The scheduled check makes the same decision differently: it
asks whether a check is due *under the lock it would run under*, so a check asked
for by hand a moment earlier means the daily one simply does not run, instead of
making the day's queries to every list a second time.

What this does not do
---------------------

* **It does not delist anything.** Removal is the list's own procedure, and the
  reason a list gives usually names the page that starts it.
* **It does not check domain-based lists** (URIBL, SURBL) or the domain's own
  reputation — only the addresses mail leaves from. The incoming path already has
  its own DNSBL and SURBL settings, which are a different question: what to do
  about *other* people's addresses.
* **It does not watch a listing between checks.** A list that adds this server an
  hour after a check is found at the next one.
* **It cannot see behind a third party.** If mail leaves through a smart host, the
  address that matters is the smart host's, and this server has no way to know
  it: name it in `ReputationWatchAddresses` if the smart host is yours.

Error codes
-----------

| Code | What it means |
|---|---|
| 6900 | A check's result could not be stored or removed. The check itself happened; the report may be a check behind. |
| 6901 | What the last check found could not be read, so this check was abandoned rather than written over it. |

A lookup that fails, a list that refuses and a list that is not answering are
**not** errors: they are answers, and they are in the report.
