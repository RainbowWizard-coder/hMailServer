# Impersonation protection

Microsoft 365 calls it the anti-phishing policy: a message whose sender has
borrowed a colleague's name, or whose domain is one letter from yours, is the
oldest trick in business e-mail compromise and the one no spam filter scores.
This server does the same two checks, per domain, and does one of two things
with what it finds. It is off on every domain, existing and new, until an
administrator turns it on, and while it is off nothing is read.

The setting is the domain's: `Domain.ImpersonationAction` over COM,
`impersonation_action` on `GET`/`PUT /api/v1/domains/{domain}`, the
**Impersonation protection** tab of the Control Panel's domain dialog (beside
**Tagging and disclaimer**) and the group of the same name in the Control
Deck's domain editor; the column is `hm_domains.domainimpersonationaction`,
schema 6087. Beside it are the two lists the checks judge against,
`ProtectedPersons`/`protected_persons` and
`ProtectedPartners`/`protected_partners`. The decision is one unit,
`Common/Util/ImpersonationCheck`.

## Where it runs, and on what

At local delivery, on the recipient account's own copy of the message, in the
place the external-sender tag is applied (`LocalDelivery::LocalDeliveryPreProcess_`,
beside `ExternalSenderTagger::Apply`), for the same two reasons the tagger
runs there: the switch belongs to the **recipient's** domain, which is not
known until the message has been split between its recipients; and the copy
is this account's, so a message addressed both here and outside never carries
the warning out with it.

It runs only on a message from **outside**, and "outside" is the tagger's own
answer - `MessageOrigin::IsExternalSender`, the one rule in one place - made
once for the copy and handed to both through `MessageOrigin::Verdict`, so the
two can never disagree. A message from an authenticated session, from a
trusted incoming relay, or whose every address is at a domain this server
hosts is inside, and is not checked. [DomainTransforms.md](DomainTransforms.md)
says what that rule does and does not cover.

**The From header is read here.** The display name and the address the reader
sees are in the RFC 5322 `From` field, not in the envelope, and the field is
read from the copy as it will be delivered - after the account's own spam
overrides, before the rules and the Sieve script, exactly where the tagger
reads it - decoded as the reader's client decodes it (RFC 2047), the address
taken from it as the DMARC check takes it. The envelope sender is judged as
well, for the lookalike check only, when its domain differs from the From
address's.

## The two checks

### A borrowed name

The From display name, folded (the table below), equals the folded person
name of an **active** account of the recipient's domain - first name and last
name as the directory composes them, `DirectoryStore::NameOf` - or of a
protected person, while the From address is not that account's own address,
nor one of its aliases, nor the address given for the protected person. A
display name that is itself an address (`"ceo@example.test" <x@evil.test>`)
is compared as an address too.

The header, and the log line, say whose the name is:

    X-hMailServer-Impersonation: name "Ada Lovelace" is ada@example.test's; from ada.lovelace@evil.test
    X-hMailServer-Impersonation: name "The Chairman" is a protected person's; from chairman@evil.test

An account with no person name entered is protected by its address only:
there is no name to compare.

### A lookalike domain

The From address's domain - and the envelope sender's, when it differs - is
folded the same way (IDNA `xn--` labels decoded first, by a Punycode decoder of
the unit's own, RFC 3492) and is a lookalike of a domain this server hosts, an
alias domain, or a protected partner when either of these holds:

* it is **equal after the confusable folding while not equal before it** -
  `examp1e.test`, `exarnple.test`, `xn--exmple-4nf.test` (a Cyrillic а) against
  `example.test`; or
* it is **one Damerau-Levenshtein edit away inside one label**, with the same
  number of labels and every other label the same: one insertion
  (`examplee.test`), deletion (`exmple.test`), substitution (`exumple.test`) or
  transposition of two adjacent characters (`exampel.test`, `example.tset`).

and it is not itself a hosted domain, an alias domain or a partner, nor
beneath one: `mail.example.test` is `example.test`'s own, not a stranger one
label away from it. The **label-boundary rule** is the second half of that:
an edit that adds, removes or moves a dot is never counted, so
`exampletest` and `exampl.etest` are not lookalikes of `example.test` - a name
is only ever compared with a name of the same shape.

    X-hMailServer-Impersonation: domain examp1e.test looks like example.test; from invoices@examp1e.test
    X-hMailServer-Impersonation: domain examp1e.test looks like example.test; envelope sender bounce@examp1e.test

**Distance 1 is the rule and 2 is not.** On a short domain two edits reach an
unrelated name - `abc.test` is two edits from `xyz.test`, `bank.test` two from
`bark.tent` - and a check that warns about a stranger's honest domain teaches
the reader to ignore the warning. One edit is what a lookalike registration
buys: the letter dropped, doubled, swapped or replaced that the eye reads
past. Everything beyond that is the confusable table's job.

**A DMARC pass changes nothing here.** A lookalike domain is registered by the
attacker, who publishes its SPF and DKIM and DMARC records exactly as a
legitimate domain would; a message from `examp1e.test` that passes
`examp1e.test`'s own DMARC is still from `examp1e.test`. Authentication answers
"is this really from the domain it claims"; this check answers "is that
domain pretending to be one of ours". The two are independent and the
authentication badges in the webmail stand beside this warning, not instead
of it.

## The folding

Both names and domains are compared after folding, in this order:

| Step | What it does | Where |
|---|---|---|
| Unicode NFKC | A full-width or ligature spelling becomes the letters it stands for (`ｅｘａｍｐｌｅ` to `example`, `ﬁ` to `fi`). | Windows, through the same `NormalizeString` call SASLprep makes. **Not on Linux**, which has no Unicode tables in this build: there this step is skipped and the confusable table below does the rest. |
| Single confusables | `1`, `l`, `I`, `\|` to `l`; `0` to `o`; the Cyrillic letters that look Latin - а е о р с х у і ј ѕ һ ԁ and their capitals - to a e o p c x y i j s h d; Latin script ɡ to `g`; the Greek capitals Α Β Ε Ζ Η Ι Κ Μ Ν Ο Ρ Τ Υ Χ to a b e z h l k m n o p t y x, and Greek ο and ι to o and i. | Both. Before the lower-casing, which is why a capital `I` and a Greek iota fold to `l` and not `i`: in the sans-serif faces mail is read in, `Ian` and `lan` are one glyph. |
| Lower case | | Both. |
| Collapse | Every run of white space and punctuation becomes one space, and the ends are trimmed: `A. Lovelace,` and `a  lovelace` are the same name. | Names only; a domain has none. IDNA labels are decoded before the folding. |
| Pairs | `rn` to `m`, `vv` to `w`. | Both. |

The table is the roadmap's list plus the case pairs and the two Greek
lower-case letters, not Unicode's whole confusables file. It is
`ImpersonationCheck::FoldName` and `FoldDomain`, and
`ImpersonationCheckTester` holds it to these examples.

## What happens

**Flag** (`1`): the copy is given one header, ASCII, on one line, with the
reason in it, in one of the two shapes above - added as `X-hMailServer-External`
is added, a field of its own, so a sender's DKIM signature is untouched, since
a signature covers the fields it names and this is not one of them. The
subject is left alone. The webmail turns the header into a warning bar above
the message - *This sender's name matches Ada Lovelace here, but the message
is from ada.lovelace@evil.test.* / *This message's domain examp1e.test looks
like example.test.* - in the twenty-three languages the page has, and the
listing's row wears a **Suspicious sender** mark. A rule or a Sieve script
can match on the header, which is written above both. A stranger who wrote
the header themselves has it removed from a message this finds nothing to say
about, and replaced where it does.

**Quarantine** (`2`): the copy goes to the quarantine through
`QuarantineStore::Quarantine` with the reason as the row's reason and score 0,
exactly as a spam verdict at the account's delete threshold does, and this
account's copy is not delivered. The recipient's daily digest lists it, the
administrator's release delivers it, and **a release does not re-check**: a
released message re-enters at the delivery queue with the mark
`GetReleasedFromQuarantine`, past the filters that held it, as a spam release
does - a release is a person overruling this check, and re-running it would
hold the message again. With the quarantine store off
(`QuarantineEnabled`), a domain that asks for quarantine gets flag instead: a
copy that was not stored anywhere is not discarded.

Either way, one line in the application log at DEBUG with the addresses -
never the error log, because a warning is the feature working.

## What is exempt

* A sender the recipient's own **safe-sender list** allows (`SenderListCache`,
  the *Safe senders* page of the webmail: an address or a whole domain the
  person marked safe) - and the domain's or the server's allow list where
  nothing nearer decides, which is the precedence every other reader of that
  list uses. A person who vouched for a sender is not then warned about them.
* A sender domain that is itself **hosted here** (a domain or an alias
  domain) or a **protected partner**, exactly, for the From address's domain:
  both checks are skipped. A partner's mail carrying a colleague's name is the
  partner's business.

Nothing else. In particular an authentication pass is not an exemption (see
above), and a message the account's spam settings un-marked is still checked.

## The two lists

**Protected persons** (`protected_persons`, `ProtectedPersons`): people
outside the domain's accounts whose name must not be borrowed - the chairman
who has no mailbox here, the auditor, an address at another hosted domain.
Entries are separated by line breaks or commas (a name therefore cannot
contain a comma), each one of:

* `Name <address>` - the name is protected, and mail from that address may use it;
* `Name` - the name is protected from every outside address;
* `address` - the address is protected as a display name, and when it is an
  account on this server (at another hosted domain) that account's person name
  is protected too.

Stored one per line, addresses lower-cased, names as typed, duplicates
dropped; at most 2,000 characters; an entry whose address is not an address
is refused with a sentence, over REST as a 400 and over COM as the property's
error.

**Protected partners** (`protected_partners`, `ProtectedPartners`): domain
names separated by commas or spaces, ASCII - an internationalised name in its
IDNA `xn--` form - lower-cased on save and stored separated by `, `, the
restricted-delivery list's shape; at most 2,000 characters. An address, a
`.suffix`, an address literal or a name that is not a domain name is refused
with a sentence. A partner's lookalikes are judged exactly as the hosted
domains' are, and the partner itself and everything beneath it is exempt.

Both lists are in the domain's XML store and come back through a backup and a
restore, put through the same canonicaliser every other writer uses; a value
in an archive that does not pass restores empty, with a log line.

## What it costs

Off, nothing: the action is a field on the cached domain, and the check
returns before reading anything. On, a copy from **inside** costs nothing
beyond the outside/inside decision the tagger already made. A copy from
outside costs two queries per delivered copy - the active accounts' names of
the recipient's domain, and the hosted domain names - and one read of the
copy's header, which the tagger has already loaded. Neither list is cached,
so a renamed account or a new domain counts from the next message; a domain
with thousands of accounts pays for that on every outside message it
receives, and that is the honest price of not answering from a stale cache.

## The surfaces

Over COM, `IInterfaceDomain.ImpersonationAction` (0, 1 or 2; anything else
refused), `ProtectedPersons` and `ProtectedPartners` (canonicalised; refused
with the sentence). Over REST the same three keys on `GET` and
`PUT /api/v1/domains/{domain}`; `impersonation_action` takes the digit or the
word `off`, `flag` or `quarantine`, and a fourth value, a non-integer or a
boolean is a 400 naming it. The Control Panel's tab holds the action as a
choice and the two lists as text areas; the Deck's group holds the same. The
message read (`GET /api/v1/me/messages/{id}`) carries the header's text as
`impersonation`, and the listing carries `impersonation: true` on a stamped
row.

## Tests

`RegressionTests/SMTP/ImpersonationProtection.cs` (REST port 9836), each
with its negative control: the borrowed account name flagged with the reason
naming the account and the same name from the account's own address, an alias
of it and a partner domain not flagged; a name one letter off not flagged; a
protected person's name flagged and not from the address given; the six
lookalike spellings against the hosted domain, a subdomain and the domain
itself not, a partner's lookalike and the partner itself; the quarantine with
the reason, the digest, the release and the mailbox after it; the default
action doing nothing for every case; the safe-sender exemption and the
envelope-sender lookalike with a clean From; the REST and COM refusals and
round trips; the XML store through a backup and a restore.
`ImpersonationCheckTester`, run from `ClassTester::DoTests`, holds the folding
table, the two judgements and the two canonicalisers to their examples
without a delivery. `build/portal-script-test.js` opens a message of each
header shape and one without, and reads the warning bar and the row's mark.
