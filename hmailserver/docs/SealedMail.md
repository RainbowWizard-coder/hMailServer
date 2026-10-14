# What a sealed message gets on this server, and what it does not

A **sealed** message is one the two people writing to each other signed or encrypted
end to end - S/MIME or PGP/MIME - so that this server is a courier for bytes it holds
no key to. The webmail has sent and read S/MIME since 6.3.1; a mail client has always
been able to. This document is the list of what the server does with such a message
at every point where it would ordinarily read or rewrite one, because before
16 September 2026 three of those points did the wrong thing silently, and one of them
destroyed mail.

Two words are used with care throughout, because they name two different sets:

| | Content types | What it means |
|---|---|---|
| **Sealed** | `multipart/signed`, `multipart/encrypted`, `application/pkcs7-mime`, `application/x-pkcs7-mime`, `application/pgp-encrypted`, `application/pgp-signature` | The message must leave this server byte for byte as it arrived. Changing anything breaks the signature in every client that checks, or makes the ciphertext undecryptable. |
| **Opaque** | `multipart/encrypted`, `application/pkcs7-mime`, `application/x-pkcs7-mime`, `application/pgp-encrypted` - **and a body that really is ciphertext** | The server cannot read the content at all. Every opaque message is sealed; a `multipart/signed` message is sealed but **not** opaque - its first part is the text exactly as the author wrote it, and every reader on this server reads it as before. |

Sealed is decided from the message's top-level `Content-Type`, in `SealedMessage`
(`source/Server/Common/Util/SealedMessage.cpp`): declining to add a footer costs
nothing, so a label is enough. **Opaque is not decided from the label**, because every
check below that stands aside for an opaque message - the blocked-attachment list,
the stripper (for an encrypted one), the rule engine's body and content conditions,
eDiscovery's body search - would otherwise be one header away from being switched off
by anyone sending mail.
A message is opaque only when its body has the exact shape the label promises:

* **S/MIME** (`application/pkcs7-mime` or `application/x-pkcs7-mime`): one part whose
  content begins a CMS object of enveloped-data, authenticated-enveloped-data or
  signed-data, named `smime.p7m` or not named at all.
* **PGP/MIME** (`multipart/encrypted`): `protocol="application/pgp-encrypted"` and
  **exactly two parts** - `application/pgp-encrypted` holding `Version: 1`, then
  `application/octet-stream` holding one ASCII-armoured OpenPGP message and nothing
  else, whose packets are session keys followed by encrypted data (the same test
  gateway encryption makes, below) - each named `*.asc`, `*.pgp`, `*.gpg` or not
  named at all.
* **A bare `application/pgp-encrypted`** at the top: the second of those two parts on
  its own.

Anything else under one of those labels - a `multipart/encrypted` around `invoice.exe`
or around readable text, a third part beside the two, a ciphertext part called
`invoice.exe`, an OpenPGP message under no key - is **not** opaque, and is blocked,
stripped, filtered and searched like any other message. Before this was tightened for
6.3.4, the label alone decided, and one `Content-Type` line took a message past all
four.

Every part of the server named below asks that one class, so the product tells its
administrator one story about a given message. What is deliberately **not** on either list is inline
("armoured in the body") OpenPGP, which is a `text/plain` message with a
`-----BEGIN PGP MESSAGE-----` block in it: the MIME layer cannot tell one from ordinary
text, so nothing here can either, and such a message is treated as plain text
everywhere.

Nothing below is a setting. None of it can be turned off, because every alternative
is a message destroyed or a lie in the log.

## The blocked-attachment list

**An encrypted message is never rewritten by the blocked-attachment list.** It is delivered
exactly as it arrived, and one line in the application log names the message, the
sender and its content type and says that the list was not applied.

This is the defect that destroyed mail. The ciphertext of a PGP/MIME message is a part
named `encrypted.asc` (or `msg.asc`), the ciphertext of an S/MIME message is
`smime.p7m` and its detached signature is `smime.p7s`, and the MIME layer counts every
non-text, non-multipart part as an attachment - so to the blocked-attachment rewriter
they were ordinary files with ordinary names. An administrator blocking `*.asc`,
`*.p7m`, `*.p7s` or `*.*` had the content of every sealed message replaced with the
"blocked for delivery" notice, the file rewritten on disk, and **no log line and no
error**. The plaintext existed nowhere else.

What the rewriter now writes to the application log, in both directions:

* A message it rewrote: `VirusScanner - Message 1234 from a@example.com: the
  attachment(s) invoice.exe matched the blocked-attachment list. Their content has
  been replaced with a notice and the message rewritten.` Before this the rewrite left
  no record anywhere but inside the message itself.
* A sealed message it left alone: `VirusScanner - Message 1234 from a@example.com is
  signed or encrypted (multipart/encrypted; ...), so it was NOT checked against the
  blocked-attachment list: its ciphertext is one attachment by MIME type and replacing
  it would destroy the message beyond recovery. The message has been delivered exactly
  as it arrived.`
* A message it could not parse: also logged, and also left alone.
* A message labelled encrypted whose body is not: `VirusScanner - Message 1234 from
  a@example.com is labelled encrypted (multipart/encrypted; ...) but its body is not
  an encrypted message, so it has been checked against the blocked-attachment list
  like any other.` - and it is.

What "a sealed message" means here is an **opaque** one, decided from its body as
above. A signed message is checked like any other, with one part left alone: its
detached signature, because replacing it removes nothing and makes every client report
the message as tampered with. That part, too, is found by its place and not by its
label - the second of exactly two parts of a top-level `multipart/signed`, of the type
the `protocol` parameter names, called `*.p7s` (S/MIME) or `*.asc`, `*.sig`, `*.pgp`,
`*.gpg` (OpenPGP) or nothing, and holding CMS signed-data or an armoured OpenPGP
signature. A part anywhere else that calls itself `application/pkcs7-signature`, or a
signature part called `invoice.exe`, is checked like any other attachment.

An ordinary message carrying a file with the same name is still blocked; the fixture
`RegressionTests.AntiVirus.SealedMailSafety` proves both directions.

## The virus scanner

**An opaque message is scanned, and the scan cannot see its content.** The whole file
and every part are handed to the scanner exactly as for any other message, and what
the scanner examines is ciphertext. A clean result from that is not a verdict on the
message, and the application log now says so:

`VirusScanner - Message 1234 is encrypted (multipart/encrypted; ...), so its content
COULD NOT BE EXAMINED for viruses. What was scanned was the ciphertext. A clean result
here means nothing was found in bytes nobody can read, not that the message is clean.`

The line is written only for a scan that really ran and really came back clean; a
scanner that finds something in the covering parts of a message still stops it as
before. And it is written for an opaque message, not for a signed one: a
`multipart/signed` message carries its text in the clear, the scanner read it, and a
clean result on it is a clean result.

**What is deliberately not done:** the message is *not* treated as a scanner failure.
The scanner-failure action exists for an enabled scanner that could not complete, and
an installation set to hold unscannable mail would otherwise hold every encrypted
message it receives - a decision about encrypted mail an administrator has to take
knowingly, not one a scanner setting takes for them. If your policy is that mail this
server cannot scan must not be delivered, that is a rule or a Sieve script on
`body :raw` (below), not the scanner.

The plain sentence for a reader: **a message that arrives encrypted is not scanned.
Your mail client, and you, are the last check.**

### When a scanner cannot run

The Anti-virus settings page's card *When a scanner cannot run* answers the
question the rest of the page cannot: what happens to a message when a scanner
is switched on and the scan errors, times out or cannot reach its engine. That
is not a clean verdict - nobody looked. *If a message cannot be scanned*
(`AVFailAction`) is *Deliver it (default)*, which is what this server has
always done; until the setting existed the only trace was one line in the error
log, which reads the same as "scanned and found clean". *Hold it and scan it
again, then return it to the sender* is the stricter choice and the one to make
deliberately: a held message is never delivered unscanned. It is scanned again
every *Minutes between scan attempts while held* (`AVFailRetryMinutes`, 15),
delivered as soon as a scanner answers, and returned to the sender after *How
many times to hold before returning it to the sender* (`AVFailMaxHolds`, 16 -
about four hours at 15 minutes); 0 returns it at once, so unscanned mail is
never queued. The count is kept in memory, so a service restart gives a held
message a fresh budget.

## The "delete attachments" anti-virus action

**An encrypted message is never stripped.** When the anti-virus action is *delete
attachments* and the scanner objects to an encrypted message - S/MIME enveloped-data or
authenticated-enveloped-data, or PGP/MIME ciphertext, judged from its body as above -
the message is left exactly
as it arrived and the application log says so, naming the message and its content type
and ending with the sentence *Set the anti-virus action to delete the message if such
a message must not be delivered* - which is the setting that still works on a message
nobody can read.

This is not a quiet pass: the message still carries what the scanner objected to. But
the stripper keeps one text part and throws the rest of the message away, and on a
sealed message "the rest" is the ciphertext, or the signature that makes the text
worth anything; what survived was a fragment of base64 under a header rebuilt without
its `Content-Type`.

A message only **labelled** encrypted, whose parts are ordinary, is stripped like any
other, and the log says `Message 1234 is labelled encrypted (...) but its body is not
an encrypted message, so its attachments have been stripped as for any other
message.` A signed message is stripped too, and the log says the signature is now
invalid.

Two further defects in the stripper were fixed in the same pass because they came to
light there, and neither is about sealed mail:

* `IsGoodTextPart_` compared the whole media type (`text/plain`) against the word
  `text`, and had the sense of the comparison inverted; the two mistakes cancelled
  into "keep whichever part is walked first". On a message whose first part was the
  attachment, the attachment was kept and the text thrown away. It now keeps the first
  part whose main type is `text`.
* A message with no text part at all - two attachments and nothing else - was
  previously "stripped" to its first attachment. It is now stripped to the header and
  the notice with no body, which is what deleting every attachment of it means.

What the action does with a message that is not text with files attached:

* A message that is **one attachment** and nothing else - a file, a message carried
  whole, or a text file declared an attachment, each an attachment as a part of a
  multipart message would be - is stripped the same way: what is delivered is its
  header, the subject marked *Virus found*, and the notice with no body. The part's own
  `Content-` fields go with it, since they describe the part and not the notice: a
  `Content-Transfer-Encoding` left in would have a client decode the notice by it, and
  a `Content-Disposition` would offer the notice as the file. The log says `the message
  was one attachment, so the notice is all that is delivered`.
* An S/MIME message **signed as one part** - `application/pkcs7-mime` holding CMS
  signed-data, the opaque form - is opaque to the server's other readers, but it is not
  encrypted: its content is inside the signature, where the recipient's client shows
  it, a file in it included. The stripper cannot pick a part out of it, so it is one
  attachment like any other and is replaced by the notice, signature and all, and the
  log says `is signed in S/MIME's opaque form`. (A `multipart/signed` message is
  stripped like any other, as above.)
* A message that is **one text part** has no attachment to remove and is delivered as it
  is, as the text part of a multipart message is kept. The log says that it is a single
  text part and, like the encrypted case, names the setting to choose if such a message
  must not be delivered.
* A message the server **cannot open or parse** - another program holds its file, or
  has moved it away, or its parse failed - has nothing in it that can be kept, and it is
  **deleted**, as *Delete the whole message* deletes a message. Each of its recipients
  is sent the server's *message deleted* notice (the `VIRUS_NOTIFICATION` server
  message) in place of the note, whatever *Notify the recipients* says, and its sender
  is not told, as this action never tells the sender; when the message's header cannot
  be read either, the notice gives the envelope's sender and the recipient as its
  *From* and *To*. It is deleted rather than held:
  a message that cannot be read now is no more readable on a later pass, and a hold
  would end in a return to a sender that virus mail forges. The log says `Message will
  be deleted`, with the reason.
* A message whose file the server **cannot rewrite** - it can be read, but another
  program lets nobody write it, or the write does not complete - is deleted in the same
  way, its recipients told and its sender not: what would be delivered is the file, and
  it still holds the message as it arrived, or a part of what was being written over
  it. The log says that the message `could not be rewritten`.

## Rules

**A body criterion on an opaque message does not match, whichever way it is phrased,
and one line per rule per message says so.**

`GetBody()` walks the MIME tree for a `text/plain` part and `GetHTMLBody()` for a
`text/html` one; an encrypted message has neither, so the rule engine compared the
criterion against an empty string. *Contains* then missed, harmlessly. *Does not
contain* matched **every** encrypted message - so the rule an organisation writes when
it takes confidentiality seriously, *body does not contain our confidentiality notice,
move to Held*, swallowed every encrypted message it received, and the more seriously
the organisation took encryption the more of its mail vanished.

The body of an opaque message is not empty; it is unreadable, and those are different
facts. The rule engine is this server's own, so the semantics are ours to choose: a
criterion the server cannot evaluate **does not match**. Precisely:

* Under *all criteria must match* (AND), a rule with a body criterion does not fire on
  an opaque message.
* Under *any criterion may match* (OR), the other criteria are still evaluated, so
  *subject contains X or body contains Y* still fires on the subject.
* A `multipart/signed` message is read as it always was. Its text is in the clear.
* A message labelled encrypted whose parts are readable is read as it always was
  too, by the body, `content` and `sensitive_content` conditions alike: the label
  does not decide, the body does.
* One line per rule per message: `RuleApplier - Message 1234 is encrypted
  (multipart/encrypted; ...), so the body criterion of rule "Hold what carries no
  notice" could not be evaluated and did not match. ...` An administrator with thirty
  rules and one body criterion in each gets thirty lines for that message, and one
  for a rule with several body criteria.

If a rule is meant to catch encrypted mail, test the `Content-Type` header for
`multipart/encrypted` or `application/pkcs7-mime` with a custom-header criterion.
The fixture is `RegressionTests.Rules.SealedMessageRules`.

## Sieve

**The evaluation is unchanged. A line in the log is new.**

This is different from the rule engine on purpose. RFC 5173 defines `body :text` and
`body :content` over the decoded MIME parts; an encrypted message genuinely has no text
part; an empty set of values is therefore the correct evaluation, `body :contains
"x"` is false, and `not body :contains "x"` is true. A third truth value would make
this server's Sieve disagree with every other implementation, and a script is portable
or it is nothing.

So a script that files on `if not body :contains "[EXT]"` files every encrypted
message, and one on `if body :contains "invoice"` files none of them, exactly as they
would on Dovecot - and what was missing was anyone being told. The delivery now writes,
once per message and recipient, after the script has run to its own conclusion:

`SMTPDeliverer - Message 1234: the Sieve script for user@example.com tested the body
of a message that is encrypted end to end. An encrypted message has no readable text
part, so the test compared against nothing and was false for every key - which is what
RFC 5173 requires and is not changed here. Use "body :raw" to recognise such a
message.`

`body :raw` is the undecoded body exactly as it arrived, so `if body :raw :contains
"BEGIN PGP MESSAGE"` is the honest way a script can recognise a sealed message and
decide what to do with it. The fixture is `RegressionTests.Sieve.SieveSealedBody`.
This line is the one place the label alone still decides: it changes nothing about
the evaluation, and it is written only when the test did come back empty.

## The domain disclaimer and the domain signature

Both already declined a sealed message and logged it before this pass; this document
only points at [DomainTransforms.md](DomainTransforms.md), "What it is never added
to". An organisation with a legally required footer has a class of outgoing mail
without it, and the log line is where to find which messages.

## Search, the index and the webmail listing

* **The full-text index** indexes the header plus the text parts. An opaque message
  has none, so only its header is indexed - which is exactly what a search can find in
  one. Inline OpenPGP is a `text/plain` part, so its armour *is* tokenised, and past
  the per-message token cap the overflow marker makes that message a candidate for
  every search from then on: wasteful, and still correct, because the index may only
  ever exclude and the searcher reads the same armour. A comment in
  `MessageIndexer.cpp` says why this must not be "improved" on its own.
* **Webmail search of the body, and the snippet**, are blank for an opaque message.
  Headers, `from:`, `subject:`, dates, `is:` and threading all keep working, because
  PGP/MIME and S/MIME leave the RFC 5322 headers alone. The webmail already says so:
  *Encrypted; decrypted in this browser. It can be searched by its headers only.*
* **`has:attachment`** and the paper-clip in the listing both read the top-level
  header for `multipart/mixed` or a `Content-Disposition: attachment`, so an opaque
  message is not found by `has:attachment` and shows no clip - while the opened
  message lists its ciphertext part among the attachments. That is a known
  inconsistency and is left as it is: what an encrypted message "has attached" is
  only knowable after the page has decrypted it.

## What else reads the body, and is unchanged

* **Content spam scoring.** SURBL reads the decoded text parts and sees nothing in an
  opaque message; SpamAssassin is handed the file and can fire only header and network
  rules on it. Encrypted spam is therefore scored more leniently than plain spam.
  SPF, DKIM, DMARC, PTR, HELO and DNSBL are unaffected.
* **The quarantine, the archive and the journal** keep the bytes verbatim, so release
  works and the released message still decrypts - but nobody reviewing them can read
  the content.
* **POP3 and IMAP** stream the file as it is on disk, which is the right answer: a
  client that holds the key reads the message.

## Gateway encryption to a partner domain

A remote domain policy can encrypt mail to a partner at this server, to the
partner's own key ([RemoteDomains.md](RemoteDomains.md), "Gateway encryption"). It
asks a third question, narrower than either list above: **is this message encrypted
end to end already?** It is answered from what the body holds, not from the label
that says so: an `application/pkcs7-mime` (or `x-pkcs7-mime`) body whose CMS content
type is enveloped-data or authenticated-enveloped-data - whatever its `smime-type`
parameter says, and whether or not it has one - or a PGP/MIME message
(`multipart/encrypted` with `protocol="application/pgp-encrypted"`) whose encrypted
part is an OpenPGP message that begins with session key packets and then encrypted
data - and, either way, only when it has the opaque shape above: exactly the version
part and the ciphertext, or one S/MIME part named `smime.p7m` or not named. Real
ciphertext with a readable part beside it is encrypted for the partner, since that
part would otherwise travel in the clear. A zero-access mailbox asks the same
question before it seals a delivery ([ZeroAccess.md](ZeroAccess.md)). A message
wholly under a key goes to the partner exactly as it arrived. A message that is
sealed but not encrypted - a `multipart/signed` message, or opaque S/MIME
`signed-data` however it is labelled, the bare `application/pkcs7-mime` label older
S/MIME v2 clients gave it included - is encrypted for the partner whole, signature
included, and the partner's gateway opens it to the same bytes, so the signature
still verifies. So is a message only labelled encrypted whose content is readable.
The rest of the message's journey - the copies to anybody but the partner, the
stored copies - is untouched.

## The one line to remember

A sealed message leaves this server exactly as it arrived. Everything the server would
ordinarily do to its content it either does on the text it can see (a signed message)
or says in the application log that it could not do (an encrypted one). Nothing is
done silently any more, and nothing is destroyed.
