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
is this account's, so a message addressed both here and to an outside
recipient never carries the warning out to that recipient. What does carry
it out is a forward: an account's forward, a rule's or a Sieve `redirect`
sends the copy as it is in the mailbox, header and all - exactly as it sends
the tagger's header - and the doc says so rather than leaving it to be found.

It runs only on a message from **outside**, and "outside" is the tagger's own
answer - `MessageOrigin::IsExternalSender`, the one rule in one place - made
once for the copy and handed to both through `MessageOrigin::Verdict`, so the
two can never disagree. A message from an authenticated session, from a
trusted incoming relay, or whose every address is at a domain this server
hosts is inside, and is not checked. [DomainTransforms.md](DomainTransforms.md)
says what that rule does and does not cover. A message the quarantine
released is never checked again (below).

**The From header is read here.** The display name and the address the reader
sees are in the RFC 5322 `From` field, not in the envelope, and the field is
read from the copy as it will be delivered - after the account's own spam
overrides, before the rules and the Sieve script, exactly where the tagger
reads it - decoded as the reader's client decodes it (RFC 2047), the address
taken from it as the DMARC check takes it. It is parsed only for a copy from
outside, which is the only one judged. The envelope sender is judged as
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
is compared as an address too, against the account's address and its
aliases.

The header, and the log line, name the spelling shown, the person here it
matches and whose name that is:

    X-hMailServer-Impersonation: name "Ada Lovelace" matches Ada Lovelace (ada@example.test); from ada.lovelace@evil.test
    X-hMailServer-Impersonation: name "Ada L0velace" matches Ada Lovelace (ada@example.test); from ada.lovelace@evil.test
    X-hMailServer-Impersonation: name "The Chairman" matches The Chairman (a protected person); from chairman@evil.test
    X-hMailServer-Impersonation: name "ceo@example.test" matches the address (ceo@example.test); from x@evil.test

An account with no person name entered is protected by its address only:
there is no name to compare. A shown name, or a person's name, longer than
120 characters is cut there with an ellipsis, so that the header line stays
within what RFC 5322 allows a line; the log and the quarantine row carry the
same text.

An alias of the account is covered twice over: an alias address is one of
the account's own addresses to this check, and it is at a hosted domain,
which is exempt from both checks in any case.

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
label away from it. "Itself" is judged with both sides IDNA-decoded and
nothing more, so a hosted `xn--bcher-kva.example` written as `bücher.example`
in a From header is that name and not a lookalike of its own A-label - while
`bucher.example`, one letter off it, is.

The **label-boundary rule** is the second half of that: an edit that adds,
removes or moves a dot is never counted, so `exampletest` and `exampl.etest`
are not lookalikes of `example.test` - a name is only ever compared with a
name of the same shape. But a lookalike is used with a label in front of it,
as `secure.examp1e.test` or `invoices.xn--exmple-4nf.test`, so the candidate
and each of its parent names down to two labels are judged in turn,
stopping at the first parent that is one of ours (everything beneath that is
ours too): `mail.examp1e.test` is caught by the `examp1e.test` inside it, and
the reason names that.

    X-hMailServer-Impersonation: domain examp1e.test looks like example.test; from invoices@examp1e.test
    X-hMailServer-Impersonation: domain examp1e.test looks like example.test; from invoices@mail.examp1e.test
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
| Invisible characters deleted | The format characters no client draws are removed, not turned into spaces: the soft hyphen U+00AD, the combining grapheme joiner U+034F, the zero-width spaces and joiners and marks U+200B to U+200F, the bidi controls and separators U+2028 to U+202E, U+2060 to U+2064, and the byte order mark U+FEFF. `Ada Lovelace` with a zero-width space and a soft hyphen inside it is `Ada Lovelace`. | Both. |
| Single confusables | `1`, `l`, `I`, `\|` to `l`; `0` to `o`; the Cyrillic letters that look Latin - а е о р с х у і ј ѕ һ ԁ and their capitals - to a e o p c x y i j s h d; the Cyrillic capitals Т Н В М К to t h b m k, the palochka Ӏ ӏ to `l`, Ԛ ԛ to `q`, Ԝ ԝ to `w`, Ѵ ѵ to `v`; Latin script ɡ to `g`; the Greek capitals Α Β Ε Ζ Η Ι Κ Μ Ν Ο Ρ Τ Υ Χ to a b e z h l k m n o p t y x, and Greek ο and ι to o and i. | Both. Before the lower-casing, which is why a capital `I` and a Greek iota fold to `l` and not `i`: in the sans-serif faces mail is read in, `Ian` and `lan` are one glyph. |
| Lower case | | Both. |
| Collapse | Every run of white space and punctuation becomes one space, and the ends are trimmed: `A. Lovelace,` and `a  lovelace` are the same name. | Names only; a domain has none. IDNA labels are decoded before the folding. |
| Pairs | `rn` to `m`, `vv` to `w`. | Both. |

The table is the roadmap's list plus the case pairs, the Cyrillic capitals
above and the two Greek lower-case letters, not Unicode's whole confusables
file. It is `ImpersonationCheck::FoldName` and `FoldDomain`, and
`ImpersonationCheckTester` holds it to these examples.

**What slips past it, said plainly.** Word order and decorations are not
folded: `Holloway, Chris` and `Chris Holloway (CEO)` are not `Chris Holloway`.
A name carried only in the local part of the address with no display name
(`chris.holloway@evil.test`) is not a display name and is not compared. A
right-to-left override that reverses the text on screen, a composed
diacritic (`Lovelacé`), digits other than `0` and `1`, and the Greek
lower-case letters ν ρ κ υ are not in the table. And a domain whose account
is named **Support** or **Info** will have every vendor's "Support" and
"Info" flagged, which is the check doing what it was asked; give such
accounts the vendors as protected partners, or a person's name.

## What happens

**Flag** (`1`): the copy is given one header, one line, with the reason in it,
in one of the two shapes above, written through the RFC 2047 encoder in
UTF-8 so a shown name that is not ASCII reaches the reader as it was shown -
added as `X-hMailServer-External` is added, a field of its own after the
fields the message arrived with, so a sender's DKIM signature is untouched,
since a signature covers the fields its `h=` tag names and this is not one of
them. The subject is left alone. The webmail decodes the header and turns it
into a warning bar above the message - *This sender's name matches Ada
Lovelace (ada@example.test) here, but the message is from
ada.lovelace@evil.test.* / *This message's domain examp1e.test looks like
example.test.* - in the twenty-three languages the page has, and the listing's
row wears a **Suspicious sender** mark. A rule or a Sieve script can match on
the header, which is written above both. A stranger who wrote the header
themselves has it removed from a message this finds nothing to say about,
and replaced where it does; with the action off nothing is read, and a
forged header is left as it arrived, for the reader's client to show or not
as it pleases.

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
* A From domain that is itself **hosted here** (a domain or an alias
  domain) or a **protected partner**, exactly - its U-label and A-label
  being one name: both checks are skipped for the From address. The
  envelope sender is still judged: a partner's From over a lookalike return
  path is flagged, naming the envelope.

Nothing else. In particular an authentication pass is not an exemption (see
above), a message the account's spam settings un-marked is still checked,
and a person's own name from their personal address at another provider is
flagged until they, or their reader, put that address on the safe-sender
list - which is what Microsoft 365 does too, and the support question it
raises is answered here.

## The two lists

**Protected persons** (`protected_persons`, `ProtectedPersons`): people
outside the domain's accounts whose name must not be borrowed - the chairman
who has no mailbox here, the auditor, an address at another hosted domain.
Entries are separated by line breaks or commas; a comma inside a quoted name
is part of the name (`"Holloway, Chris" <chris@board.example>`), and a quote
that is opened and not closed on its line is refused with a sentence. Each
entry is one of:

* `Name <address>` - the name is protected, and mail from that address may use it;
* `Name` - the name is protected from every outside address;
* `address` - the address is protected as a display name, and when it is an
  account on this server (at another hosted domain) that account's person name
  is protected too.

Stored one per line, a name holding a comma in quotes, addresses
lower-cased, names as typed, duplicates dropped; at most 2,000 characters; an
entry whose address is not an address, or with a quote inside the name, is
refused with a sentence, over REST as a 400 and over COM as the property's
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
returns before reading anything. On, the header block is loaded once for the
copy - by the tagger when the domain tags, by this check otherwise - and a
copy from **inside** costs nothing beyond that and the outside/inside
decision. A copy from outside costs the parse of its From header and the
comparisons against what the domain's judgement reads, which is held per
domain id (`ImpersonationCheck::Facts`): the domain's active accounts with
their names folded once and their aliases beside their addresses, the
protected persons, and the hosted and partner names decoded and folded once.
Those facts are read - two queries, one for the accounts and one for the
aliases, plus the hosted names - when a domain is first judged, and forgotten
whenever an account, an alias, a domain or an alias domain is saved or
deleted (`PersistentAccount`, `PersistentAlias`, `PersistentDomain`,
`PersistentDomainAlias`), and after sixty seconds whatever happened, as a
safety net under the invalidation; a domain of twenty thousand accounts pays
for its list once a minute at most, not once per copy. The edit distance is
computed only for labels whose lengths are within one of each other.

## The surfaces

Over COM, `IInterfaceDomain.ImpersonationAction` (0, 1 or 2; anything else
refused), `ProtectedPersons` and `ProtectedPartners` (canonicalised; refused
with the sentence). Over REST the same three keys on `GET` and
`PUT /api/v1/domains/{domain}`; `impersonation_action` takes the digit or the
word `off`, `flag` or `quarantine`, and a fourth value, a non-integer or a
boolean is a 400 naming it. The Control Panel's tab holds the action as a
choice and the two lists as text areas; the Deck's group holds the same. The
message read (`GET /api/v1/me/messages/{id}`) carries the header's decoded
text as `impersonation`, and the listing carries `impersonation: true` on a
stamped row.

## Tests

`RegressionTests/SMTP/ImpersonationProtection.cs` (REST port 9836), each
with its negative control: the borrowed account name flagged with the reason
naming the person and the real address, and the same name from the account's
own address, an alias of it and a partner domain not flagged; a name one
letter off not, a look-alike spelling yes, and the alias address as a display
name compared as an address; a Cyrillic display name sent as an encoded word,
its reason read back decoded through the message route and the row marked in
the listing; a protected person's name flagged and not from the address given
for them, and a nameless account named by its address; the five lookalike
spellings and a lookalike with a label in front of it against the hosted
domain, a subdomain and the domain itself not, a partner's lookalike and the
partner itself; an alias domain exempt and its lookalike flagged; a forged
header removed, replaced and - with the action off - left alone; the
quarantine with the reason, the digest, the release and the mailbox after
it, and the store off flagging instead; the default action doing nothing for
every case; the safe-sender exemption at the account scope and at the
domain's, and the envelope-sender lookalike with a clean From; the REST and
COM refusals and round trips, a quoted name with a comma among them; the XML
store through a backup and a restore. `ImpersonationCheckTester`, run from
`ClassTester::DoTests`, holds the folding table (the invisible characters and
the Cyrillic capitals included), the Punycode decoder (RFC 3492's own sample
A among its vectors), the two judgements - a name beneath a lookalike, the
U-label of a hosted A-label - and the two canonicalisers to their examples
without a delivery. `build/portal-script-test.js` opens a message of each
header shape and one without, and reads the warning bar and the row's mark.
