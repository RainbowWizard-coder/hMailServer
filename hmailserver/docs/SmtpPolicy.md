# SMTP policy the score cannot express

Three things one operator asked for in discussions #283, #285 and #286, each
of them a decision the spam score cannot make on its own, and every one of them
**off by default**: a stock installation behaves exactly as it did before they
existed.

| | What it is | Off means |
|---|---|---|
| **SPF's other results, scored on their own** (#286) | SoftFail, TempError and PermError each get a score of their own, and TempError can instead be answered `451` | The three scores are 0 and add nothing; a TempError is accepted, as it always was |
| **A null sender with several recipients** (#283) | `MAIL FROM:<>` naming more than one `RCPT TO` is refused, and optionally the connecting address is banned for a while | Accepted, as it always was |
| **A domain white and black list** (#285) | Sending organisations decided about by their **domain** - envelope domain or HELO suffix - rather than by the IP address of the week | The list is empty, and an empty list changes nothing |

Everything here applies to **inbound, unauthenticated** sessions of an IP range
that runs the spam tests, and never to a session whose sender the anti-spam
white list covers: it sits at the layer greylisting sits at. None of it
overrides authentication. One of our own people who has authenticated is not
"inbound".

## SPF's other results (#286)

`SpamTestSPF` has always written all seven RFC 7208 results into
`Authentication-Results` and `Received-SPF`, and it has always **scored only
Fail** (`UseSPFScore`). SoftFail, TempError, PermError, Neutral and None added
nothing, so raising the SPF score could refuse nothing but a hard fail.

Three of the unscored results are operationally useful, and they do not mean
the same thing, so they do not share Fail's score:

| Result | What it means | Setting | Default |
|---|---|---|---|
| **SoftFail** | The domain published a policy, this client is not in it (`~all`), and the domain asks that the message be accepted anyway. Many spray sources live behind `~all`. | `UseSPFSoftFailScore` | 0 |
| **TempError** | The check did not finish: DNS timed out, answered `SERVFAIL`, or the lookup limit was reached. The record may be fine. | `UseSPFTempErrorScore`, or `SPFTempErrorDefer` | 0, off |
| **PermError** | The check finished and the published record is unusable: two records, a bad mechanism, a redirect loop, too many lookups. Strict sites treat it as a fail, lenient sites ignore it. | `UseSPFPermErrorScore` | 0 |

A score above 0 is added to the message's spam score by the same path Fail
uses, so it marks at the mark threshold and refuses at the delete threshold
(`550 5.7.1` at `RCPT TO`, or at `MAIL FROM` where `DNSBLChecksAfterMailFrom`
is on). A score of 0 is "not scored", exactly as before: nothing is recorded
and the headers are the whole record of the result.

`SPFTempErrorDefer`, when on, answers a TempError with **`451 4.4.3`** for the
transaction - RFC 7208 section 8.6's own answer to a check that could not be
completed - so a resolver blip defers the message and the sending server tries
again when DNS works. The TempError score is ignored for that message, because
a deferred message is never accepted and there is nothing for a score to mark.
The 451 is answered before the score is read, and it is never answered to a
sender the domain policy list marks white.

What this is **not**:

- Not folding the three results into `UseSPFScore`.
- Not scoring Neutral ("no assertion") or None ("no policy published").
- Not treating SoftFail as a Pass for `BypassGreylistingOnSPFSuccess`; that
  bypass stays Pass-only.
- Not a 451 on PermError: the record is broken and a retry will not fix it.

Where they are: `Settings.AntiSpam` over COM (`UseSPFSoftFailScore`,
`UseSPFTempErrorScore`, `UseSPFPermErrorScore`, `SPFTempErrorDefer`),
`PUT /api/v1/settings/antispam` (`use_spf_softfail_score`,
`use_spf_temperror_score`, `use_spf_permerror_score`, `spf_temperror_defer`),
the Control Panel's Anti-spam page under *Sender authentication*, and the Deck's
Settings view.

## A null sender with several recipients (#283)

`MAIL FROM:<>` is the reverse-path of a delivery status notification, and a
notification has exactly one recipient: the address that sent the message it
reports on. A null sender that goes on to name a second recipient is not
sending one. It is backscatter, a forged report or a probe - a protocol lie,
not "maybe spam", which is why this is a refusal and not a score.

| Setting | Meaning | Default |
|---|---|---|
| `RejectMultiRcptNullSender` | The second and every later `RCPT TO` of a null sender is answered `550 5.7.1`, and the transaction's `DATA` (or `BDAT`) is refused with `550 5.7.1` too | off |
| `BanMultiRcptNullSender` | The connecting address is also banned, once per session, through the auto-ban a run of failed logons earns: an expiring IP range named `Auto-ban: multi-recipient null sender <address>`, lifted by the same sweep and mirrored into the firewall by the same reconciliation. Never for an address on `AutoBanNeverBan` or an incoming relay. Does nothing while the refusal is off. | off |
| `BanMultiRcptNullSenderHours` | How long that ban lasts, 1 to 8760 | 24 |

The `DATA` is refused as well as the extra recipients because the first
recipient must not be handed a "bounce" by a client that has just shown it is
not sending one. A **single-recipient null sender is never touched**: a real
DSN, an MDN, a challenge-response probe all pass as they did. What counts is
the **distinct addresses** the transaction has named, not how many `RCPT TO`
commands it sent: repeating one address is legal (RFC 5321) and answered as it
always was, because one address named twice is one recipient. `RSET` starts a
new transaction and a new count, which is what a transaction-scoped rule means;
a client that sends one recipient per transaction is sending one-recipient
reports, and this check is not the tool for that. The ban is off by
default so that one mis-sent report with two recipients does not ban a real
mail server; the log line the ban writes is the shape the packaged fail2ban
filter matches, beside the logon-failure and spam-trap lines.

Where they are: `Settings.AntiSpam` over COM, `PUT /api/v1/settings/antispam`
(`reject_multi_rcpt_null_sender`, `ban_multi_rcpt_null_sender`,
`ban_multi_rcpt_null_sender_hours`), the Control Panel's Anti-spam page under
*Host checks* (the card *Null sender with several recipients*), and the Deck's
Settings view. Operators who drop this in an `OnSMTPData` script today can
delete the script.

## The domain white and black list (#285)

Greylisting's white list, the security ranges and the DNS blacklists are all
keyed by IP address. That is the wrong key when the sender is a real
organisation whose outbound addresses change constantly - a cloud ESP, rotating
Microsoft 365 connectors, a small shop that moves hosters. Updating a /32 every
time they move is how those lists die. The domain does not change.

One table, `hm_domainpolicylist`, holds a handful of rows:

| Column | Meaning |
|---|---|
| `pattern` | `example.com` matches that domain alone; `.example.com` matches the domain **and everything beneath it**, anchored at a label boundary (`notexample.com` is not beneath it) |
| `match_on` | `envelope` (the envelope sender's domain), `helo` (the HELO/EHLO name), or `both` |
| `action` | `white` or `black` |
| `note` | The operator's own text |

The pair of `pattern` and `match_on` is unique. Patterns are stored lower-cased
and without a trailing dot; an address literal such as `[192.0.2.1]` is refused
as a pattern and never matched as a HELO, because this list is the one keyed
by names.

The list is consulted at `MAIL FROM`, once the envelope sender and the HELO
name are both known - before greylisting, before the score, before a single
lookup is paid for:

1. **Black** - `550 5.7.1 Mail from <name> is refused by this server's policy.`
   and the transaction stops. A fresh IP address or a passing SPF record copied
   this morning changes nothing, because the key is the name.
2. **White** - remembered for the transaction. The session **skips
   greylisting**, is **not deferred** for an SPF TempError, and is **not
   refused or quarantined on the spam score** - neither the `550` at `RCPT TO`
   nor the `554` after `DATA` at the delete threshold. Every test still runs,
   still logs, and still writes `Authentication-Results`; the **mark threshold
   still marks**, because scoring continues - it is just not the accept gate.
3. Otherwise the existing pipeline, unchanged.

When several rows match, the most specific pattern wins: a longer pattern over
a shorter one, a whole domain over the suffix of the same length, and black
over white at a dead heat.

An empty list is one flag test per `MAIL FROM` and costs nothing. Rows are read
once and cached, and every save and delete marks the cache for reload, so a row
is in force for the next session and a black row removed stops refusing at
once.

### How this differs from the lists that already existed

| List | Keyed by | When | What it does |
|---|---|---|---|
| **Anti-spam white list** | An IP range and a sender | `MAIL FROM` | Skips **every** spam test. Stronger than white here, and IP-shaped. |
| **Greylisting white list** | An IP address pattern | `RCPT TO` | "This host is allowed to retry." Nothing more. |
| **Blocked senders** | An envelope address or domain | During the SMTP conversation | Adds a score, 100 by default. A score, so a white domain here would not be refused by it. |
| **Allow and block lists** ([SenderListsAndDigests.md](SenderListsAndDigests.md)) | An address, a domain or a URL | At delivery, per recipient | Decides for one recipient, below that recipient's own list. |
| **Domain white and black list** (this) | A domain or HELO suffix | `MAIL FROM`, for the connection | White: not greylisted, not refused on the score. Black: refused before anything runs. |

A white domain here is "I know who they are, their mail compliance is a mess
and will stay a mess, and I still want the mail." A black domain is the
opposite of a DNS blacklist: the operator picked the name and is not waiting
for a zone to list the sender's new VPS.

What this is **not**:

- Not per-message scoring.
- Not the per-domain *greylist skip after SPF pass*, which still requires the
  authentication to pass. White here is stronger: the operator has already
  decided they want the mail.
- Not a substitute for SPF, DKIM and DMARC on everyone else, and not a way to
  override authentication: an authenticated session is never judged by it.
- Not consulted for an IP range that does not run the spam tests, nor for a
  sender the anti-spam white list covers.
- Not applied to the multi-recipient null sender check above, which is
  independent of it: a protocol lie is a lie whoever tells it.

For a session from an **incoming relay**, the HELO name the list sees is the
relay's own and the envelope sender is the original sender's; a black-by-HELO
row would have to name the relay, which is not what anybody wants, so key rows
for mail behind a relay on the envelope domain.

### Where it is edited

| Surface | How |
|---|---|
| REST | `GET`/`POST /api/v1/domain-policy-list`, `PUT`/`DELETE /api/v1/domain-policy-list/{id}`. Server-wide: refused for a credential confined to named domains, and the writes need `manage_settings`. |
| COM | `Settings.AntiSpam.DomainPolicyList` (a JSON document with `items`), `AddDomainPolicyListEntry(Pattern, MatchOn, Action, Note)` answering the id, `RemoveDomainPolicyListEntry(ID)` answering whether there was one. Server administrators. |
| Control Deck | *Lists*, the section *Domain white and black list*, built from the OpenAPI document like the lists above it. |
| Control Panel | *Settings > Anti-spam > Domain white and black list*. |
| Backup | The list travels with the anti-spam settings in the archive, as the other anti-spam lists do. |

Every change is in the audit trail as a `domain policy list entry`.

## What it does not do

- Nothing here trains anything or changes a score's meaning: the three SPF
  scores are ordinary scores, and the two refusals are refusals.
- The null-sender check counts the **envelope** addresses this server
  accepted; a recipient refused for another reason (unknown user, relaying
  denied) is not a second recipient, and neither is the expansion of one. A
  null sender naming a single distribution list or an alias that reaches fifty
  mailboxes has named one recipient and is let through: a report addressed to a
  two-mailbox alias is a real report, and a list that accepts the null sender
  is the operator's decision to have made.
- The domain list has no expiry on a row and no per-domain (tenant) scope: it
  is the server's, because it decides before there is a recipient to have a
  domain.
- The domain list is **ASCII**. A pattern is validated as a domain name is,
  so a U-label (`bücher.example`) is refused when it is typed and an A-label
  (`xn--bcher-kva.example`) matches only a sender that writes the domain that
  way; a sender that writes it as Unicode under SMTPUTF8 is not covered by
  either row. The server has no IDNA converter, and one belongs to the whole
  server rather than to this list.
- `hmctl` has no verb for the list; the REST routes are the way from a script.
- The white verdict does not touch the DNS blacklists' or SpamAssassin's
  *running* - only what the resulting score is allowed to do. An operator who
  wants no tests at all for a host has the anti-spam white list.

## What proves it

`RegressionTests/AntiSpam/SmtpPolicy.cs`: SoftFail, PermError and TempError
each refused at the delete threshold with their own score and accepted with it
at 0; the TempError answered `451 4.4.3` with the defer on and a PermError never
deferred; a pass untouched by the new scores; a null sender's second recipient
and its `DATA` refused, a single recipient accepted, the same two recipients
accepted with the switch off, and the ban created as an expiring `Auto-ban:`
range and lifted; a black envelope domain and a black HELO suffix refused at
`MAIL FROM` while a look-alike and the apex-only case are not; a white domain
past greylisting, past a softfail at the delete threshold, and past the
TempError deferral, with the message delivered each time; and the list and the
settings refusing what they are given in words.
