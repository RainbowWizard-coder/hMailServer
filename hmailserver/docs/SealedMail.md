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
| **Opaque** | `multipart/encrypted`, `application/pkcs7-mime`, `application/x-pkcs7-mime`, `application/pgp-encrypted` | The server cannot read the content at all. Every opaque message is sealed; a `multipart/signed` message is sealed but **not** opaque - its first part is the text exactly as the author wrote it, and every reader on this server reads it as before. |

Both are decided from the message's top-level `Content-Type` alone, in
`SealedMessage` (`source/Server/Common/Util/SealedMessage.cpp`), and every part of the
server named below asks that one class, so the product tells its administrator one
story about a given message. What is deliberately **not** on either list is inline
("armoured in the body") OpenPGP, which is a `text/plain` message with a
`-----BEGIN PGP MESSAGE-----` block in it: the MIME layer cannot tell one from ordinary
text, so nothing here can either, and such a message is treated as plain text
everywhere.

Nothing below is a setting. None of it can be turned off, because every alternative
is a message destroyed or a lie in the log.

## The blocked-attachment list

**A sealed message is never rewritten by the blocked-attachment list.** It is delivered
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

## The "delete attachments" anti-virus action

**A sealed message is never stripped.** When the anti-virus action is *delete
attachments* and the scanner objects to a sealed message, the message is left exactly
as it arrived and the application log says so, naming the message and its content type
and ending with the sentence *Set the anti-virus action to delete the message if such
a message must not be delivered* - which is the setting that still works on a message
nobody can read.

This is not a quiet pass: the message still carries what the scanner objected to. But
the stripper keeps one text part and throws the rest of the message away, and on a
sealed message "the rest" is the ciphertext, or the signature that makes the text
worth anything; what survived was a fragment of base64 under a header rebuilt without
its `Content-Type`.

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

## The one line to remember

A sealed message leaves this server exactly as it arrived. Everything the server would
ordinarily do to its content it either does on the text it can see (a signed message)
or says in the application log that it could not do (an encrypted one). Nothing is
done silently any more, and nothing is destroyed.
