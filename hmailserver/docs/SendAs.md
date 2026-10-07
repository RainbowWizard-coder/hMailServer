Sending as another address
==========================

Who an account may write as - in the webmail's **From**, in `MAIL FROM` on an
authenticated SMTP session when `SmtpAuthenticatedSenderCheck` is on, and in the envelope,
`From` and `Sender` of mail a program signed in as the account queues over COM
([Roles.md](Roles.md)) - is four kinds of address:

| Kind | Given by | How |
|---|---|---|
| The account's own address | - | Always. |
| An alias that points at it | The administrator | An active alias, followed a few hops. |
| Another mailbox on this server | That mailbox's owner | The post (`p`) right on their INBOX, which any IMAP client's SETACL can grant. |
| An address at another provider | The person themselves | Proved with a code mailed to it (below). |

The webmail's identity picker (`GET /api/v1/me/identities`) lists exactly these, with
`kind` `account`, `alias`, `granted` or `verified`, and the send route accepts exactly
these - the list is the SMTP rule turned around, so what the picker offers is what the
server takes.

Proving an address elsewhere
----------------------------

**Settings, Other accounts, Send mail as** in the webmail: the person types the address
(and, if they like, the name to send under) and presses **Send the code**. The server
mails a six-digit code to that address from the postmaster of the account's domain, with
the null sender, and the person types it back. From then on the address is theirs to
write as, until they remove it.

- The code lives in the server's memory for an hour and is never stored: five wrong
  tries end it, a new one goes out no sooner than a minute after the last, and a restart
  means asking for another. Nothing in a database dump proves an address.
- An address of a domain on this server is refused. It belongs to someone here, who
  shares it with an alias or the post right; a code mailed to it would be delivered
  to them, not proved by the person asking.
- An address the account can already write as, and one added already, are refused.
- `reader_sender_addresses_max` (5 by default, at most 20; 0 takes the section away)
  bounds how many an account has, proved or not.
- The addresses are the account's own data: `hm_senderaddresses`, carried by the
  backup with the account, and removed with it.

**What this cannot change.** Mail this server sends as `someone@gmail.com` is not
aligned with `gmail.com`: it is not signed by Gmail's DKIM key and not sent from an
address Gmail's SPF record names. A receiver that honours the domain's DMARC policy will
quarantine or refuse it when that policy says so. The page reads each address's policy
and says so beside an address whose domain publishes `quarantine` or `reject`; the API
gives it as `dmarc_policy`. For such an address, send from its own provider.

A Sieve script's `:from`
------------------------

A Sieve `vacation` reply and a Sieve `notify` notification are sent from the script's
`:from` only when it is an address that reaches the account: the account's own address,
in any case, or an active alias whose value is that address. A mailbox granted with the
post right and an address proved elsewhere are not among them. Any other `:from` is not
used: the message goes from the account's own address, and the application log names
the account and what its script asked for. A `:from` that is not an address at all - none,
a display name, an address followed by a line break - is refused when the script is
stored or checked (ManageSieve `PUTSCRIPT` and `CHECKSCRIPT`, the self-service filter,
COM's `CheckSieveSyntax`), with the reason; a script stored before that check came to
`notify` (6.3.6 and earlier) still runs, and its notifications go from the account's own
address. Up to 6.3.6 a notification was sent from whatever its `:from` said - another
account's address, or any address anywhere, signed with this server's DKIM key for a
domain it signs for. The address the server finds for a `:from` must be the one the
script wrote, letters' case aside: a database whose collation ignores accents, as MySQL's
does, finds the account for the same address with an accent on one of its letters, and
that address is not the account's.

The third message a script sends, a `redirect`, says it is from whoever the message it
redirects was from. `editheader` may not add or delete `From`, `Sender`, `Resent-From`,
`Resent-Sender` or `DKIM-Signature`, the name in any case: a redirect sends the edited
message on from this server, and the server signs a message as the domain its `From`
names wherever it signs for that domain and the message carries no signature of it, so
an edit of `From`, or of the signature a message came with, would have had the server
send another's mail in their name and sign it with their domain's key (6.3.6 and earlier,
from 6.2.22). A script that edits one is refused when it is stored or checked, with the
reason; at delivery the edit is not made, of the name as the script's variables made it,
and the application log says so; and a script stored before still runs without it.
`Reply-To`, which names no sender, may still be edited.

A rule's reply and the headers a rule sets
------------------------------------------

An account may change its own rules over COM, signed in as itself, and a domain's
administrator the domain's ([MailFlowRules.md](MailFlowRules.md)). Since 7 October 2026 a
rule's **Reply** goes only from an address the rule's owner may send as: for an account's
rule, the four kinds of address above, as the webmail's send takes them, and a masked
address of the account's own; for a domain's rule, an address of the domain or of one of
its domain aliases; for a server rule, any address. The reply's name is written as one
quoted name before the address, so that a name with a comma or an address in it adds no
address to the `From`. A reply may go from no address, the null sender; its `From` is then
the name before an empty `<>`, and a name with an `@` in it is left out, so that the `From`
is `<>` alone: the reader the server picks a `From`'s address with takes a name that is one
email address for the address itself, and a reply from no address under the name
`ceo@example.com` was signed as `example.com`. And a rule's **Set header value** may set
`From`, `Sender`, `Resent-From`, `Resent-Sender` or `DKIM-Signature` only in a server rule, which only the
server administrator saves: a forward, a copy or a reply would otherwise go out in the
name a domain's or an account's rule wrote, signed with that name's domain's key. Who saved
a rule is not recorded, only whose it is, so an account's rule the server administrator
saves is held to the account's addresses as well. Either is refused when it is saved, with
the reason, and one stored before is not taken at delivery, with a line in the application
log. Up to 6.3.6 an account's rule could reply as any address and set any of the five.

Which messages the server signs as a domain
-------------------------------------------

With DKIM signing on for a domain, the server signs a message as the domain its `From`
names on the message's first delivery pass - but since 7 October 2026 only a message it is
entitled to sign:

* **A submission by a signed-in user** - an authenticated SMTP session, the webmail's send,
  a scheduled send, the send API - and every copy of it: as before.
* **A message the server made** - a bounce, an out-of-office or Sieve reply, a rule's reply,
  a notification, a report, a list's notice - whose `From` its maker chose under the rules
  above.
* **A message an IP range lets in as one of this server's domains without signing in** -
  what `My computer` lets the server's own web sites and scripts send - since the range was
  set up to trust that sender as a submitter (the session is the message's submission
  server, RFC 6409).

Every other message came in from **outside**: an SMTP session that did not sign in, and did
not send as one of this server's domains from a range that lets it, a download from another
server's mailbox (external accounts), or a message imported from a file. Its `From` is
whatever its sender wrote, and the server does not sign it as the domain that `From` names -
nor any copy of it: an account's forward, a rule's forward or copy, a Sieve `redirect`, an
alias or a distribution list expanding to an address outside, the mirror copy, a managed
list's copy, a journal rule's copy delivered as it was accepted, or the encrypted copy
for a partner domain. A forward leaves with the
signature the message came with, untouched, and - with `ArcSealingEnabled` on - with an ARC set the relaying
domain seals over the authentication results this server saw when it arrived. A managed list
that took a posting from outside whose `From` is of the list's own organisation does not
sign its copies as the list's domain either ([MailingLists.md](MailingLists.md)): that
signature would be one DMARC aligns with the `From`. A message released from the quarantine
is from outside when the `Received` field this server wrote at its arrival names a session
that did not sign in, since the quarantine keeps no other record of where it came from.

Up to 6.3.6 the server signed every message whose `From` named a domain it signs for,
whatever it was: a message from outside claiming to be from `ceo@` such a domain was signed
before local delivery, and a forward of it to an address outside left the server as a
forgery that passed DMARC. Messages already in the queue when the server is upgraded were
recorded without the mark and are signed as before.

The routes
----------

| Route | What it does |
|---|---|
| `GET /api/v1/me/sender-addresses` | `{addresses:[{id,address,name,verified,created,dmarc_policy}],max}` |
| `POST /api/v1/me/sender-addresses` | `{address, name}`: adds it unproved and mails the code; `code_sent` says whether the message was queued. 400 for an address on this server, 409 for one already writable or added, 403 past the ceiling. |
| `POST /api/v1/me/sender-addresses/{id}/verify` | `{code}`: 200 proved; 400 with `attempts_left`; 410 when no code is waiting (expired, restarted, or five wrong). |
| `POST /api/v1/me/sender-addresses/{id}/resend` | 202 sent; 429 with `retry_after` within a minute of the last; 409 when already proved. |
| `DELETE /api/v1/me/sender-addresses/{id}` | Removes it; the account no longer writes as it. |

Each is reachable by the account's own credential or session only.
