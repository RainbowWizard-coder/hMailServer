Spam traps
==========

Addresses that were never real, and what a message to one does.

Nobody is ever given a trap address, so nobody has a reason to write to it. A
message addressed to one is therefore not mail that went astray: the sender is
working from a list they harvested, bought or guessed. That is the one piece of
evidence a mail server gets that needs no scoring and has no false positive to
weigh — the address does not exist and never did.

Nothing here is on until an administrator declares a trap. A server with no
traps does exactly what it did before, and the settings below do nothing.

What a trap does
----------------

At `RCPT TO`, a trap address is **refused exactly as any address this server does
not have** — `550 Unknown user`, the same words, made by the same code. That
matters more than it looks: a sender who can tell a trap from an unknown address
takes it off their list, and the trap stops working. Everything else happens
before the reply and is invisible from outside.

The check is `SMTPConnection::CheckSpamTrap_`, and it runs **before** the delivery
lookups rather than after them. That order is the feature: a trap in a domain with
a catch-all would otherwise be accepted like every other unknown address there and
delivered to the postmaster, greylisting would answer `451` and invite the sender
back, and a backup-MX callout would ask another server about an address only this
one knows about.

Then, for a session that could have been refused anyway:

| | What happens | The setting |
|---|---|---|
| The connecting address | Banned, through the server's own auto-ban: an expiring `hm_securityranges` row named `Auto-ban: spam trap <address>`, lifted by the same sweep that lifts a logon ban, shown on the same **IP ranges** page, and mirrored into the firewall by the same reconciliation ([AutoBanFirewall](../source/Server/Common/Util/AutoBanFirewall.h)) — that mirroring is queued rather than done inside `RCPT TO`, since it can run a program and the reply must not wait for one. | `SpamTrapBanMinutes`, 1440. 0 bans nobody. |
| The envelope sender | Added to the server's block list (`hm_senderlist` at server scope) with an expiry, so its mail to a real mailbox is held for review where the quarantine is on and marked where it is not — see [SenderListsAndDigests.md](SenderListsAndDigests.md). | `SpamTrapBlockDays`, 30. 0 blocks nobody. |
| The hit | Recorded in `hm_spamtraphits` and counted into `hmailserver_spam_trap_hits_total`. | `SpamTrapHitRetentionDays`, 90. |

The application log gets one line per hit, and one per ban:

```
"Spam trap: never-used@example.com was addressed from 203.0.113.9 by harvester@spammer.example; banned for 1440 minutes; sender blocked for 30 days."
"Auto-ban: 203.0.113.9 is banned for 1440 minutes after addressing the spam trap never-used@example.com"
```

The packaged fail2ban filter matches the second line, so a Linux installation
bans the address at its own firewall as well as in the server.

Who is never banned, and why each
---------------------------------

A ban is the one thing here that can hurt somebody who did nothing wrong, so the
exceptions are decided in one place (`SpamTrapMonitor::OnHit`) and recorded in
the hit's own note:

* **an authenticated session** — that is one of our own people, and a trap
  address in their address book is a mistake, not an attack;
* **an address whose IP range does not run the spam tests** — the operator has
  already said that range is trusted, and a trap must not overrule that;
* **an address or sender the anti-spam white list covers**, for the same reason;
* **an incoming relay** — banning the machine that fronts this server would
  refuse everybody's mail. The sender is still blocked;
* **an address on `AutoBanNeverBan`** — the list that exists precisely so that a
  ban cannot lock an administrator out of the machine's mail ports.

The sender is not blocked for an authenticated session, for the **null sender**
(a bounce, which no list may swallow), for a sender claiming a domain this server
hosts (that is a forgery, and the entry would block the real user), or where the
server's own **allow** list already names it — an administrator's decision
outranks an automatic one. A block that is already wider than the new one is
left alone rather than shortened.

"Already names it" is asked of
[SenderListCache](../source/Server/Common/AntiSpam/SenderListCache.h), which is
the one place the precedence between entries is decided, and not of a lookup for
a row whose value is exactly this address. That matters because an allow entry
usually names a **domain**: an exact-value lookup would not see
`spammer.example` while blocking `harvester@spammer.example`, and an address
entry beats a domain entry on specificity — so the automatic block would quietly
overturn the entry somebody wrote down on purpose. An entry that has **expired**
is not a decision in force and does not protect the sender; since a value is
unique within a scope, the block is written into that row, and its created date
is set to now so the listing does not show today's automatic block as something
an administrator wrote months ago.

Training the score, and what this does not do
---------------------------------------------

**A trap hit trains nothing by itself, and the reason is worth stating plainly.**
The trap is refused at `RCPT TO`, so no content ever arrives: there is nothing to
teach a content classifier from the hit, and this server has no classifier of its
own to teach. What it has is spamd's Bayes store, through the `spamc` TELL
command ([SpamAssassinLearner](../source/Server/Common/AntiSpam/SpamAssassin/SpamAssassinLearner.h)).

So one honest thing is done, and it is off by default. When a session that
addressed a trap goes on, **in the same transaction**, to send a message to a
real mailbox, that message — which the same sender sent while demonstrably
working from a harvested list — is taught to spamd as spam. Switch it on with
`SpamTrapTeachSpamAssassin`; it needs SpamAssassin enabled and a spamd started
with `--allow-tell`, and the lesson runs on the asynchronous queue against a copy
of the message, so nothing waits for it.

A trap hit with no message after it teaches nothing. That is the common case:
most senders that hit a trap send nothing else this server accepts.

**Everybody the hit spared is spared the lesson too.** An authenticated session, a
range that does not run the spam tests, a white-listed sender and an address on
`AutoBanNeverBan` teach spamd nothing, for a stronger reason than they are not
banned: a ban expires by itself and a poisoned Bayes store does not, so one of our
own people who mistypes a trap address and then sends a real message would
otherwise teach spamd that their own mail is spam. The one exception is the
**incoming relay**, which is spared the ban and not the lesson — the relay is the
machine that carried the message, not whoever wrote it.

What else is deliberately not done: a trap does not delay, tarpit or blackhole
the session beyond the refusal (a refusal is cheaper for both ends); it does not
add a spam score, because there is no message to score; and it does not tell the
domain's postmaster, because a trap that generates mail is a trap that generates
backscatter.

Declaring a trap
----------------

A trap address must be **an address in the domain it is declared on**, and it
must be an address nobody uses. An account, an alias or a distribution list with
that address is refused with the reason — and the rule holds in the other
direction too: an account, alias or list **cannot be given a trap's address**
while the trap exists, over COM, REST, the import tool or a directory sync. The
trap has to be removed first, which is a decision somebody makes rather than
something that happens quietly.

| Where | How |
|---|---|
| Control Deck | Domain editor → **Spam traps**, which also shows what has addressed this domain's traps |
| Control Deck | **Spam traps** view: the record across every domain, and the button that clears it |
| REST | `GET`/`POST /api/v1/domains/{domain}/spam-traps`, `DELETE /api/v1/spam-traps/{address}` |
| REST | `GET /api/v1/domains/{domain}/spam-traps/hits`, `GET`/`DELETE /api/v1/spam-traps/hits` |
| COM | `Domain.SpamTraps`, `Domain.AddSpamTrap`, `Domain.RemoveSpamTrap`; `Utilities.GetSpamTrapHits`, `Utilities.ClearSpamTrapHits` |
| Command line | `hmctl spamtrap list --domain example.com`, `hmctl spamtrap create --domain example.com --set address=never-used@example.com`, `hmctl spamtrap delete never-used@example.com`, `hmctl traphits list`, `hmctl traphits clear` |
| PowerShell | `Get-HmSpamTrap`, `Add-HmSpamTrap`, `Remove-HmSpamTrap`, `Get-HmSpamTrapHit`, `Clear-HmSpamTrapHit` |
| Configuration document | `hmctl config export` carries a `spam_traps` section, so traps travel with the rest of the configuration |

Just the part before the `@` is taken to mean this domain, so `never-used` and
`never-used@example.com` are the same thing on `example.com`'s page. Declaring an
address that is already a trap is not an error and makes no second trap: applying
a list of traps twice leaves the same list.

A trap is held as a local part and the domain's id, so **renaming the domain
renames its traps with it**, the way it renames accounts.

### What makes a good trap

* An address that was never published anywhere: `sales-2004@`, `webmaster@` on a
  domain that never had one, a string of random letters.
* An address that was published once, long ago, and has been dead for years — the
  classic recycled trap. Give it a year of refusals first, so that a correspondent
  who genuinely still writes to it is not banned for it.
* **Not** a typo of a real address (`saels@`): people mistype, and the sender you
  ban is the customer trying to reach `sales@`.
* **Not** an address in a domain with a catch-all. A domain whose `Postmaster` is
  set accepts every unknown address, so a trap there is refused where every other
  unknown address is accepted — which is exactly the difference a sender can
  measure. Put traps in domains without a catch-all.

What is recorded
----------------

Each hit is one row: the trap address as it was then, the domain, the connecting
address, the envelope sender, the time, whether the address was banned, whether
the sender was blocked, and a note saying what was done or why it was not. The
listing is newest first (`limit`, 1 to 1000, 100 by default), and the whole-server
listing also carries `hits_since_start`, which is the counter behind the metric.

Clearing the record removes rows and says how many. **It undoes nothing**: the
bans and the blocked senders the hits made stay until they expire or are removed
on the IP ranges page and the allow and block list.

Rows older than `SpamTrapHitRetentionDays` are swept hourly.

Metrics
-------

`hmailserver_spam_trap_hits_total` — hits since the server started, counted once
per trap per session, so a client repeating one `RCPT TO` cannot inflate it. The
SMTP connection keeps that promise: it remembers the traps it has already
answered for and refuses the repeat without counting, recording or banning a
second time. The counter is in the Prometheus exposition, in the OTLP push, and
in the metric history as
`spam_trap_hits_total`, so `rate(hmailserver_spam_trap_hits_total[1h])` charts and
alerts like any other counter. A rate that is suddenly non-zero is a list being
walked; a rate that is always zero means the traps are not in any list yet, which
for a freshly invented address is expected for months.

Backup, restore and the audit trail
-----------------------------------

The traps are in the backup with their domain, because a restore deletes each
domain first and would otherwise lose them silently. The **record of hits is
not** in the backup: it is traffic, and a restored server has not seen it.

Both tables carry a foreign key to `hm_domains` with `ON DELETE CASCADE`, as
every other table a domain owns does. Deleting a domain removes its traps and
their hits in code as well, and reports it when it cannot — but domain ids are
reissued, and a trap left behind would start refusing an address in whichever
domain took the number next, so the database guarantees it underneath the code
rather than instead of it.

`hm_spamtraps` is on the audit trail — declaring a trap and taking one back are
administrative changes, recorded with who made them **and which address it was**.
The statement itself carries a domain id and a local part going in and a row id
coming out, so the words come from the code that writes it rather than from the
statement's own parameters. `hm_spamtraphits` is not:
it is a record of what happened, written by the SMTP connection with nobody's
authority. The ban a hit creates is an `hm_securityranges` row written with no
actor, as an auto-ban from failed logons is, and the sender it blocks is an
`hm_senderlist` row written the same way — neither appears on the trail, and the
hit's own row is the record of both.

**Clearing** that record is on the trail, though, as object type `spam trap hits`
and action `cleared`, over COM and over REST alike. The rows are traffic and no
statement of the delete is recorded, so without that explicit record the one act
whose whole effect is to remove evidence would be the one act that left no trace.
See [AlertsAndAuditTrail.md](AlertsAndAuditTrail.md).

Who may do what
---------------

| | Permission |
|---|---|
| Read a domain's traps and its hits | `view_configuration`, scoped to that domain |
| Declare a trap, take one back | `manage_accounts`, scoped to that domain |
| Read the record across every domain | `view_configuration`, and never for a credential confined to named domains |
| Clear that record | `manage_settings` |
| The four settings | `manage_settings` (COM: server administrator) |

A domain administrator therefore keeps their own domain's traps without being
able to see another domain's, and an auditor reads everything here and clears
nothing. See [Roles.md](Roles.md).

Sharp edges
-----------

* **A catch-all domain gives the trap away**, as above. The refusal is the same
  as an unknown address gets *on a domain without a catch-all*; on a domain with
  one, every other unknown address is accepted and the trap is not.
* **The trap list is cached per process.** A trap added on one node of a shared
  database is in effect on that node at once and on the others when their cache
  next refreshes (any save of a trap or a domain, or a restart). The same is true
  of the allow and block list.
* **A ban is per address, not per network.** A sender walking a list from a /24
  is banned one address at a time; the firewall hook and `AutoBanCommand` are
  where a wider block belongs.
* **A trap refusal costs more work than an unknown address's, so it takes
  measurably longer.** Three rows are written before the reply where an unknown
  address writes none. The largest and least predictable part of that — telling
  the firewall, which takes a lock, reads every range and may run a program — is
  handed to the asynchronous queue rather than done inside `RCPT TO`, so what is
  left is three inserts. It is still not zero: an attacker who can time replies
  precisely, over enough attempts to average out the network, can tell the two
  apart. Nothing short of doing the work after the reply for *every* unknown
  address would close that, and the reply itself, which is what a harvester
  actually reads, is identical.
* **An expired ban lets the sender back.** That is deliberate: a trap hit is
  evidence about a moment, and addresses are reassigned. Raise
  `SpamTrapBanMinutes` rather than making it permanent by hand if that is not
  what you want.
* A trap in a domain that is **not active** does nothing: the domain refuses
  everything already, for its own reason.
