# Journaling and dual delivery

A copy of a domain's mail, or of one account's, sent to an address. Exchange
calls it *journaling* and sends an envelope report with the message attached;
Google calls it *dual delivery* and sends the message itself. They are the same
mechanism with two shapes of copy, so one rule here says which shape it wants.

Nothing is journalled until a rule exists. With an empty table the whole of this
costs one comparison per accepted message.

## What a rule says

| Field | Meaning |
|---|---|
| `scope` | `domain` or `account` - whose mail the rule is about. |
| `target` | The domain name, or the account address. It must be a domain this server hosts, or an account in one. |
| `direction` | `inbound` (a message with a local recipient the rule covers), `outbound` (a message from an authenticated sender it covers), or `both`. |
| `journal_address` | Where the copy goes. One address, local or anywhere else, checked as an address before the row is written - a value that is not one, or that carries a control character, is refused with the reason. It may be a mailbox in the very domain the rule journals; what it may not be is the account an account rule names. |
| `mode` | `report` or `copy` - see below. |
| `enabled` | A rule that is off copies nothing and keeps everything it said. |
| `note` | Why the rule exists, for whoever finds it later. At most 255 characters. |

The rules are stored in `hm_journalrules` (schema 6075). The row holds the domain
id and, for an account rule, the account id; the name is resolved back out of
those when a rule is read, so nothing breaks when a domain or an account is
renamed under it. A domain's rules go with the domain when it is deleted, and a
rule naming an account goes with the account.

## The two shapes

**`report`** - the Exchange journal shape. A `multipart/mixed` message whose
first part is the envelope report and whose second part is the original message
attached as `message/rfc822`, written through byte for byte. The report is four
fixed fields, one per line, in one order:

```
Sender: alice@example.com
Subject: Quarterly figures
Message-Id: <abc@example.com>
Recipient: bob@example.test
Recipient: carol@example.test
```

The envelope is the point. A message's `To` and `Cc` headers are what the sender
typed; the `Recipient:` lines are where the message actually went, blind copies
and expanded lists included. A message with no envelope sender - a bounce -
writes `Sender: <>` rather than leaving the field looking unwritten.

**`copy`** - the message itself, as it was accepted, with one header line
(`X-hMailServer-Journal`) in front of it and nothing else changed: not the body,
not its transfer encoding, not its signature. What a migration or a second mail
system wants is the mail, not a report about it. The file is written byte for
byte rather than through the MIME writer, which would rebuild the tree of a
message this is supposed to reproduce.

Both shapes are queued through the ordinary delivery queue, so a journal address
somewhere else is delivered, retried and bounced exactly as any other mail is,
and a local one lands in a mailbox.

The envelope sender of a journal message is the journalled domain's postmaster,
not the person who wrote the message. A journal address that stops answering is
the administrator's problem, and bouncing it to the sender would tell somebody
outside this server that their mail is being copied.

## When it runs

Once per accepted message, in the SMTP session, after the row is saved and the
message file is settled - the same place, and the same recipient list, the
on-disk archive decision is made from. Not at delivery: the deliverer runs again
for every retry and for every route a message splits into, and a compliance
archive that gained a second copy because a remote host was busy would be worse
than useless.

One report or copy per **matching rule** per message, not per recipient. A domain
rule that matches a message addressed to three accounts of the domain sends one
report, with all three on it.

And once per message a **fetch account** collects, at the same point in that
path: after the row is saved, before the deliverer is told. Mail a mailbox
collects from another provider by POP3 or IMAP is that mailbox's mail like any
other, and an archive that held everything except what a domain happened to
fetch would have a hole in it that nothing said was there. A download has no
authenticated sender, so it is the **inbound** half of a rule that speaks for
it, and it has no SMTP session, so its lines go to the application log.

## Not looping

One guard, in `Journaling::Evaluate`, and it takes nobody's word for anything.

Every journal message this server writes carries `X-hMailServer-Journal`, and
its value is the rule's id, a full stop, and a **mark**: 128 random bits the
server process chose when it first needed one and told nothing and nobody. The
evaluation reads that header before it does anything else and stops only when
the mark is this process's own. A journal message that comes back into this
server - relayed by the far end, forwarded by the journal mailbox's owner - is
therefore never journalled again, and the guard stops the copy, not the
delivery: the mailbox still gets its mail.

A message carrying the header with any other value is journalled exactly as it
would have been without it, and the attempt is noted in the log. This is the
point of the mark. The header's name is in this document, so a guard that
honoured the header as written would mean that anybody under an outbound
compliance rule could keep a message out of the archive by typing one line into
their mail client, and anybody outside could do the same for mail to a
journalled domain. An archive that the people it is about can opt out of is not
an archive.

The mark is per **process**, and nothing is stored for it. A restart forgets the
marks it wrote, so one of this server's own reports that returns after a restart
is journalled once more - and the report *that* makes carries a mark the running
process does know, so it stops there. A loop cannot run away. The price of
keeping no secret on disk is one duplicate, in a configuration that is already
sending mail round in a circle.

The mark is not a signature over the message, and it is worth being plain about
what it therefore is not proof against. It travels in the messages it marks,
because a guard that has to recognise a message coming back has no other way to;
so whoever can read this server's journal mail - the journal address itself, and
anything relaying to it - could quote a mark back in a message of their own and
keep that one message out of the archive. The journal address is the destination
this feature already trusts with every copy it makes. If that relay is a worry,
journal to a local mailbox, or over an authenticated route, exactly as you would
for the copies themselves.

There is no second guard, and no rule is skipped for where its journal address
points. **Journalling a domain into a mailbox of that domain works**, which is
the most natural way to write one of these: `example.com` to
`archive@example.com` sends one report per message into that mailbox and
nothing cascades, because a journal message is written straight into the
delivery queue and never passes an evaluation point. (It used to be refused at
run time, silently: the rule matched, the copy was dropped with a line in the
log, and every surface went on showing the rule as enabled. An empty compliance
archive nobody was told about is the worst thing this could produce, so the
guard went.)

The one arrangement refused, and refused when the rule is **written** rather
than found out later, is an account journalled into itself: that puts every
message in the mailbox twice and archives nothing anywhere.

## Where it is edited

* **REST** - `GET/POST /api/v1/journal-rules`, `PUT/DELETE
  /api/v1/journal-rules/{id}`. Server-wide, and refused to a credential confined
  to named domains even though a rule names one: a rule says that somebody's mail
  is copied to an address outside their sight, which is the server
  administrator's decision to make. Reading needs `view_configuration`; writing
  needs `manage_server`, so the auditor role reads the rules and writes none.
* **The Control Deck** - the *Journaling* view: every rule with whose mail, which
  way, what is sent and where it goes, a switch on each row, and a form to add
  one.
* **The Control Panel** - the *Journaling and dual delivery* page, over three
  `Utilities` methods (`GetJournalRules`, `SaveJournalRule`, `DeleteJournalRule`).
* **hmctl** - `hmctl journalrule list|create|update|delete`, and
  `Get-HmJournalRule`, `New-HmJournalRule`, `Set-HmJournalRule`,
  `Remove-HmJournalRule` in the PowerShell module.

Every one of them goes through one function on the server, `Journaling::SaveRule`,
so the three small vocabularies (`domain`/`account`, `inbound`/`outbound`/`both`,
`report`/`copy`) mean the same thing whichever surface you use, and a refusal
reads the same sentence.

Writing, changing or removing a rule is on the audit trail, because
`hm_journalrules` is one of the tables the statement chokepoint records.

## What it deliberately does not do

* **It is not the archive.** `ArchiveDir` and `ArchiveDomains` keep files on this
  server; a journal rule sends mail elsewhere. Neither replaces the other, and
  turning one on does not turn the other off.
* **It is not eDiscovery.** Finding the mail of some people that mentions some
  words, holding it against deletion and exporting it is a case's
  ([Discovery.md](Discovery.md)): a case searches the mailboxes, their deleted
  items and the archive on this server, and a journal copy has left it. A
  journal address that is a mailbox here is searched like any other custodian.
* **It is not an account's forwarding.** A forward is the account holder's, and
  can deliver *instead of* the mailbox. A journal copy is the administrator's,
  and never changes what the mailbox gets.
* **It is not a mail flow rule's forward or copy action.** Those have conditions,
  run per delivery attempt, and can be edited by the account.
* **It is not retention.** Once a journal message leaves the queue it belongs to
  whatever receives it. This server keeps no second copy, expires nothing and
  withdraws nothing; deleting a rule stops the next copy and does not reach the
  ones already sent.
* **It is not encryption or redaction.** The copy carries whatever the message
  carried. A journal address outside your control sees everything the recipient
  sees.
* **It has no conditions.** A rule covers all of a domain's or an account's mail
  in the direction it names. There is no "only messages over 10 MB" and no "only
  from these senders"; a mail flow rule is the place for that.
* **It does not cover mail a person sends from the browser.** A message the
  webmail queues directly (`POST /api/v1/me/messages`), a send-later draft and a
  message written by an event script are queued past both evaluation points and
  are not journalled. The outbound half therefore covers SMTP submission, which
  is what a desktop or phone client uses, and not the browser. Mail that
  **arrives** is covered whichever way it arrives - the SMTP session or a fetch
  account - so it is the outbound half that has this gap and the inbound half
  that does not.
* **It does not journal a message that was refused.** A message rejected at RCPT
  TO or by a rule never existed as far as this is concerned; a message held for
  review IS journalled, because it was accepted.

## Proving it works

`RegressionTests.SMTP.Journaling` drives real deliveries: a domain rule producing
one report with the original attached and every envelope recipient on it (two
envelope recipients, one RCPT TO each, and only one of them named by a header),
an outbound rule for an authenticated sender, copy mode keeping the message's own
headers, a domain journalled into a mailbox of its own domain, a rule switched
off and on again, an id no rule carries reaching no rule, and the refusals a rule
is checked against before it is written - an unhosted target, a word outside the
three vocabularies, no journal address, one that is not an address, one carrying
a line break, and an account journalled into itself.

One test is entirely about the loop guard: a sender writes
`X-hMailServer-Journal` themselves and is journalled anyway; the value this
server put on the report it really made is taken out of that report and sent back
in, and *that* one is not journalled again.

The envelope report's own shape, the mark, the boundary's characters and the
header writer are pinned by a unit test that needs no database, no message file
and no delivery queue: `JournalingTester`, run from `ClassTester::DoTests`.
