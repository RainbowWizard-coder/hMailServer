# Restricted delivery

Accounts, or a whole domain, that may only exchange mail with named domains:
a school whose pupils write to the school and to nobody else, a regulated
desk that talks to the firm and its counterparties. Google Workspace calls
the same thing *restrict delivery*. The decision is made once, in
`Common/Util/RestrictedDelivery.cpp`, and three places call it.

## The list is the domain's

`Domain.RestrictedDomains` over COM, `restricted_domains` on
`GET`/`PUT /api/v1/domains/{domain}`, the **Restricted delivery** tab of the
Control Panel's domain dialog, the **Restricted delivery** group of the Deck's
domain editor; the column is `hm_domains.domainrestricteddomains`, schema
6084, and the domain's XML store carries it so a backup keeps it.

The list is the domains the domain's accounts may exchange mail with, at most
2,000 characters, domain names separated by commas or spaces, lower-cased on
save, each one of:

| Written as        | Covers                                                            |
|-------------------|-------------------------------------------------------------------|
| `partner.example` | that domain, and nothing beneath it                               |
| `.example`        | `example` and everything beneath it, anchored at a label boundary |

The suffix form is the spelling the domain policy list already uses, and the
matcher is the policy list's own (`DomainPolicyEntry::Covers`): `.example`
covers `example` itself as well as `mail.example`, and does not cover
`notexample` or `example.evil.test`. The list is ASCII, as the policy list
is: an internationalised domain name is written in its IDNA form (`xn--...`).
A name that is not a domain - an address, a path, an address literal - is
refused with a sentence over REST (400) and over COM, and so is a list longer
than 2,000 characters or one with a character outside ASCII.

Empty, the default, means the domain is not restricted and nothing below
runs. An upgraded server has every domain empty and every account following
it, so it delivers exactly as it did.

## An account follows its domain unless it says otherwise

`Account.RestrictedDelivery` over COM, `restricted_delivery` on the account
routes (0, 1 or 2, or the words `domain`, `restricted` and `exempt`), a
three-way choice on the Control Panel's account dialog and on the Deck's
account editor; `hm_accounts.accountrestricteddelivery`, in the account's XML
store.

| Value | Word         | Meaning                                                                                                                       |
|-------|--------------|-------------------------------------------------------------------------------------------------------------------------------|
| 0     | `domain`     | Follows the domain: restricted when the domain has a list, unrestricted when it has none. The default.                         |
| 1     | `restricted` | Restricted even when the domain names nothing; the account is then confined to its own domain - the desk that talks to nobody outside. |
| 2     | `exempt`     | An account in a restricted domain that may exchange mail with anyone - the head teacher.                                       |

## Always allowed, whatever the list

- The account's own domain, under its own name or any of its alias domains
  (`hm_domain_aliases`): an address at an alias name is an address in the
  domain.
- The domain's postmaster address.
- The null sender, `MAIL FROM:<>`: bounces and delivery status notifications
  must reach a restricted account, or it never learns that a message failed.
- Mail this server writes itself - quota warnings, the digest, the reports -
  goes out from the postmaster or the null sender and so arrives.

Other domains this server hosts are **not** allowed unless named. "May only
exchange mail with named domains" means named: a second hosted domain is
outside until it is on the list.

## Where it is enforced

Both directions, through the one `RestrictedDelivery::Check`:

**1. Inbound, at `RCPT TO`.** When the recipient is a local account that is
restricted, and the envelope sender - the `MAIL FROM` address - is not
allowed, the recipient is answered

    550 5.7.1 <address>: this mailbox accepts mail from named domains only.

per recipient, so one refused `RCPT` does not end the transaction; the other
recipients of the same message are accepted or refused on their own terms.
The envelope is the contract, as it is for every other `RCPT`-time rule; the
`From` header is not known at `RCPT` and is not consulted. Every session is
judged, authenticated or not: an authenticated colleague in another hosted
domain is a sender too, and a `550` now is kinder than the non-delivery
report the delivery-time decision below would send them.

**2. Outbound, at `RCPT TO`.** When this server is the submission server for
the message - the session authenticated, or a local sender that a range
admits without authentication - and the submitting account is restricted, a
recipient whose domain is not allowed is answered

    550 5.7.1 <address>: this account may send to named domains only.

The account is the one that authenticated, else the `MAIL FROM` address's.
An account restricted with an empty domain list is refused everything outside
its own domain; an exempt account is refused nothing.

**3. At local delivery**, for a recipient reached by expansion rather than by
`RCPT TO` - a distribution list member, an alias target, a catch-all. A
restricted recipient whose message's envelope sender is not allowed is not
delivered to: the recipient is dropped before the quota, the auto-reply and
the account-level copy are touched, and the application log names the rule.
A non-delivery report goes to the sender only when the sender is local; an
outside sender was told `550` at `RCPT` for a direct address, and for a
list, silence is the list's ordinary behaviour for a member that refused.
The sender of a message to a list got `250` at `RCPT`, because the list's
address is not an account and is not restricted; the exempt members receive
it and the restricted ones do not.

A restricted account's **forward** - the account forward, an account rule's
forward, a Sieve `redirect` - to a domain that is not allowed is not made;
the log says why, and the message is still delivered to the mailbox. Its
**out-of-office reply** - the stored one, the domain-wide one, a Sieve
`vacation` - is not sent to a sender who is not allowed, silently, with a
log line, because the reply's recipient is the sender being answered and a
restricted account does not write to a stranger, not even to say it is away.

Every refusal is a `DEBUG` log line naming the two addresses and the list;
the SMTP log records the `550`s; nothing is written to the error log, since a
refusal is the feature working.

## What it is not

- Not the sender lists (`SenderListsAndDigests.md`), which act per recipient
  on a score, nor the domain policy list, which decides for a connection
  before there is a recipient. This is a contract between named domains and
  a mailbox, both ways.
- Not a check of the `From` header, the `Reply-To` or the display name. A
  message from an allowed domain with a stranger in its `From` header is
  accepted, as it is everywhere else at `RCPT` time.
- Not a filter on what is already in the mailbox: switching a list on
  changes what arrives and leaves from that moment.
- Not a remote-domain policy: those say what may be sent to a domain from
  anywhere on this server; this says with whom one domain's accounts may
  correspond at all.
- Not a gate on the administrator's copies: a journal rule's copy of a
  restricted account's mail goes where the rule says, because the copy is
  the administrator's and not the account's forward.

## Tests

`RegressionTests/SMTP/RestrictedDelivery.cs`: an outside sender refused for a
restricted account with the exact sentence and accepted for an exempt one,
for a listed domain, for a subdomain under a `.suffix` and as the null sender
(whose bounce is delivered); a restricted account refused an unlisted domain
and another hosted domain that is not named, accepted for a listed domain
and its own; an account restricted with an empty list confined to its own
domain and an exempt account refused nothing; a distribution list with a
restricted and an exempt member reaching the exempt one alone, and both once
the sender is named; a forward from a restricted account withheld while the
mailbox gets the message, and made once the target is named; the REST
refusals (2,001 characters, an address in the list, a fourth account value)
and round-trips lower-cased; and the two COM properties round-tripped
through `Save`.
