Zero-access mailboxes
=====================

A mailbox this server holds and cannot read. The person switches it on for their own
account, from the webmail's Security page, once an administrator has allowed it
server-wide. From then on every message delivered to the account is encrypted - after
the spam and virus filters, the rules and the Sieve script have had the text - to the
account's own OpenPGP key, and what is written into the mailbox is a PGP/MIME message:
the headers in the clear, the body and every attachment under the key. So is the copy
of what the person sends from the webmail; their drafts, and anything a mail client
uploads over IMAP, are stored as they are. The key's
secret half is held in two forms only the person can open: sealed under a passphrase
the browser derives from the account password with Argon2id, and sealed under a
recovery key the person wrote down before the switch was thrown. This is the Proton
model, and it is written down here as the trade-offs it is.

What it costs, said before the switch
-------------------------------------

The webmail says this on the Security page above the switch, in fewer words, and
refuses to switch until the recovery key has been typed back and the sentence about a
lost password has been ticked. In full:

* **The administrator cannot read the mail.** The message source and the account
  export over the administrative API answer 403 for a zero-access mailbox
  (`GET /api/v1/accounts/{address}/messages/{id}/source`, `POST .../export`,
  `GET .../export.zip` - the Deck's *Everything (.zip)* link answers the same 403),
  and an administrator who resets the password and signs in as the person finds the
  key sealed under a password the page never saw. The headers are
  answered by the routes that answer headers, because the headers were kept in the
  clear - see below.
* **The administrator cannot search it, and neither can the server.** The full-text
  index (`MessageIndexer`) and IMAP SEARCH read the headers and the armour of the
  encrypted part, never the text: a search for a word in the subject finds the
  message, a search for a word in the body does not. Search over the body is the
  webmail's own business, over what it has decrypted in the browser, and it is not
  built in this row; the page searches the headers the server kept.
* **The administrator cannot recover it.** This server holds two sealed copies of the
  secret key and can open neither. A forgotten password without the recovery key is
  the mail lost, for ever, and nobody - not the administrator, not this project - can
  help. Switching zero access off decrypts nothing already stored: mail that arrived
  while it was on stays encrypted to the key, and opens with the key.
* **IMAP and POP see ciphertext.** Both hand out the message as it is stored, which is
  a valid PGP/MIME message (RFC 3156) that any client holding the key opens -
  Thunderbird with the secret key imported, or GnuPG on a saved file. Until such a
  client holds the key, an account with this on is a webmail account. No bridge that
  decrypts for a local client exists yet.
* **The plaintext was on the disk, honestly.** The row's phrase is "the plaintext
  never touches the disk", and what is true is this: the message was on disk in the
  clear from the moment it was accepted until the moment it was sealed - the queue
  file, and the account's copy while the filters read it. The account's copy is
  overwritten with zeros in place before the sealed message takes its name, which is
  the most a program can do; whether the bytes are gone is the filesystem's decision
  (a copy-on-write filesystem, a snapshot or an SSD's wear levelling may keep them).
  The queue file is deleted as every queue file is, not overwritten; the account's
  copy is overwritten the same way when a seal fails and the delivery is refused.
  Under a legal hold ([Discovery.md](Discovery.md)) a message the account's own filter
  drops - a rule's Delete, a Sieve discard or reject, a forward that keeps no copy, the
  spam delete threshold - is kept among its deleted items instead: that copy is made
  in the recoverable store and sealed there at once, its plaintext overwritten the same
  way, and one that cannot be sealed is not kept in the clear: the message goes on to
  the mailbox as if nothing had dropped it, sealed there as any delivery is, or
  refused as above when it cannot be. Nothing of the plaintext is written to any log.
* **Only what is delivered is sealed, and the Sent copy.** A message sent from the
  webmail leaves this server in the clear for its recipients, as any message does, and
  the copy filed in the sender's own Sent folder is sealed to the mailbox key on the
  way in. Nothing else the person writes is: a **draft** is stored as it is, since the
  page reads it back to go on writing, and a message a mail client uploads over
  **IMAP APPEND** is stored as the client sent it - an account with this on is a
  webmail account, and a client that holds the key can seal what it uploads itself.
  The switch-on page says this in one sentence beside the consequences.
* **A journal rule sees the plaintext.** Journaling and dual delivery
  ([Journaling.md](Journaling.md)) run at acceptance, before delivery, and the copy
  they make is of the message as it arrived. The organisation's journal is the
  organisation's; this row is about the administrator reading the *mailbox*. An
  installation that wants no plaintext anywhere has no journal rule for the account's
  domain, and says so in its own policy.
* **The filters have read it.** The spam score, the virus scan, the global and
  account rules, the forward and the Sieve script all ran over the text, because they
  cannot run over ciphertext; a rule that moves a message to a folder of the mailbox
  moves the sealed message there, but a rule or a Sieve `fileinto` that files it into a
  **public folder** files the plaintext there, because a public folder is not the
  mailbox and is not encrypted to anyone. A forward sends the message on in the clear,
  since it is a copy made before the seal; a person who forwards everything has
  forwarded everything. A message the spam filter holds in the **quarantine** is held
  there in the clear until it is released or expires; a release goes back through the
  queue and is sealed on the way in.
* **What runs after delivery sees nothing.** An invitation's calendar part is under
  the key, so a reply to an event this account organised is not filed into the
  calendar automatically; the reader deals with it after the page has decrypted it,
  as any other client would. The push notification carries the kept headers.

The headers the server kept
---------------------------

Everything above the body stays in the clear: the fields of the message as it
arrived - `From`, `To`, `Cc`, `Date`, `Subject`, `Message-ID`, `References`, the
`Received` trace, `Authentication-Results`, the server's own `X-hMailServer-*` fields,
the spam score - except the `Content-*` fields and `MIME-Version`, which described the
body that is now under the key and are replaced by the PGP/MIME envelope's own. The
server adds one field, `X-hMailServer-Zero-Access: sealed on delivery`, so that the
webmail can say what happened rather than "an OpenPGP message" and so that a test can
find the shape by name.

What is encrypted is the **entire original message**, headers and all, as the literal
data inside the OpenPGP message. So the reader sees exactly what arrived, and a client
that understands protected headers (Thunderbird) shows the inner ones; the outer
headers exist so that the listing, the threading, the search and the spam folder
work without the key.

A message that arrives already encrypted - a PGP/MIME message from a correspondent,
one the page sealed and sent to itself, or an S/MIME envelope - is stored as it
arrived. It is under a key already, and a second layer to the mailbox key would make
it a message no other client shows.

How it works
------------

**On delivery.** In `LocalDelivery::DeliverToLocalAccount_`, after
`LocalDeliveryPreProcess_` has run the account's spam overrides, the rules, the
forward and the Sieve script and just before the message row is saved,
`ZeroAccess::SealFile` reads the account's copy, chooses the key to encrypt to
(`OpenPgpWriter::ChooseEncryptionKey`: the newest living subkey flagged to encrypt
whose binding signature verifies against the primary key, else an RSA primary so
flagged; Curve25519 and RSA are the two kinds written to),
writes the sealed message beside the original, overwrites the original in place, and
gives the sealed file the original's name. A zero-access account never shares a
delivery file with other recipients of the same message (`SharingPossible_`), since
its copy is rewritten. A failure - a key row gone, a key the writer cannot encrypt to,
a disk that will not take the file - **refuses the delivery** rather than storing the
message in the clear: the sender is told (4.3.0, the same shape a save failure has),
the error log carries HM7070 with the reason, and the file is overwritten and removed.
The Sent copy of a message sent from the webmail (`POST /api/v1/me/messages`, and the
release of a message held for review) goes through the same seal
(`ZeroAccess::SealStoredCopy`); a copy that cannot be sealed is not kept, the error log
carries HM7071, and the send itself goes ahead. If the switch is found on with no key
row beside it - a restore that dropped the table, a database edited by hand -
`GET /api/v1/me/zero-access` answers `enabled` true with `broken` true and a sentence,
every delivery is refused until then, and the Security page shows the switch-off, which
is the way out.

**The OpenPGP message** (`Common/Util/OpenPgpWriter`) is a version 3 public-key
encrypted session key packet (RSAES-PKCS1-v1_5 for an RSA key; for a Curve25519
key the RFC 6637 scheme - an ephemeral X25519 agreement, the one-step KDF with the
recipient's fingerprint in its input, the RFC 3394 AES key wrap) and a version 1
symmetrically encrypted integrity-protected data packet: AES-256 in OpenPGP CFB with
the random prefix and its repeated octets, a literal data packet holding the message,
and the modification detection code. Nothing is compressed. This is the whole of what
the server does with OpenPGP: it never signs or decrypts, and the one signature it
verifies is the binding of the subkey it is about to encrypt to.

**The subkey's binding is verified**, since 24 September 2026, when gateway
encryption to a partner's key ([RemoteDomains.md](RemoteDomains.md)) began to share
the chooser: a subkey binding (and a subkey revocation) is taken only when it
verifies against the primary key over the two key packets (RFC 9580 section 5.2.4;
RSA, Ed25519 and ECDSA on the NIST curves; SHA-1 only for a signature made before
2019). Every key GnuPG or the webmail made verifies, and keeps working. What changes
for a mailbox key is that a key whose subkey binding was altered after it was signed
- an expiry edited in without re-signing, a subkey appended by somebody else - has no
subkey to encrypt to and is refused at the switch-on, as is a key whose primary is of
a kind this cannot check (DSA, a Brainpool curve). A mailbox already switched on with
such a key has its deliveries refused (HM7070, the sender told with 4.3.0) until the
key is replaced or the switch is thrown off; none was known to exist when the check
arrived, since the webmail makes Ed25519 and RSA keys and GnuPG signs its own.

**Proved against an implementation that is not this one.** `OpenPgpWriterTester` runs
inside the server (`MainOperations.TestInternals`): the X25519 arithmetic against RFC
7748's vector, the packets' shape against RFC 9580, the message opened again with the
session key the test fixed. The Linux build's `openpgp-writer` tool hands what the
writer makes to GnuPG 2.4 on every CI run (`linux-build.yml`), once to the Curve25519
fixture key and once to the RSA one, and gpg's plaintext must be byte for byte the
input; and `build/fixtures/openpgp/server-sealed-to-alice.eml`, a delivery the writer
sealed, is opened by the page's own module and by gpg in `check-portal-openpgp.js`.

**The key.** The account's key is one of its own in the webmail's ring
([OpenPGP.md](OpenPGP.md), `hm_pgpring`), where its secret half is already sealed
under the passphrase the person chose when the key was made. A zero-access mailbox
needs the key opened at every sign-in without a passphrase being typed, and a second
way in for the day the password is lost, so the switch-on stores two more copies of
the same secret key in `hm_zeroaccess` (schema 6083), both ordinary OpenPGP secret
keys sealed in the browser:

* `zasecret`, sealed under a passphrase the page derives from the account password
  with **Argon2id** (RFC 9106) - the 16-octet salt and the three parameters are
  stored beside it - rendered as 64 hexadecimal characters. Signing in is then enough
  to open the mail: the page derives, opens the copy, and keeps the opened key in
  memory for the session and nowhere else.
* `zarecovery`, sealed under the **recovery key**: 30 random octets the browser drew,
  shown once as eight groups of six base32 characters (240 bits), which the person
  wrote down and typed back before anything was stored. Typed with or without the
  dashes, in either case, it opens the copy. Saved to a file, the copy imports into
  GnuPG with the recovery key as its passphrase.

This server checks that each copy is an encrypted OpenPGP secret key of the
fingerprint named and that the key is one it can encrypt to, which is all it can
check; it cannot check that either opens, which is why the page types the recovery
key back before it sends anything. **The key must not expire**: a key whose primary
or encryption subkey carries an expiry is refused at the switch-on with a sentence
(and the page's list leaves it out), because from the day it expired every delivery
would be refused and the password copy could not be sealed again, with nothing on
the page having said so; the key page offers *Never* when a key is made. A key
already chosen is never re-checked at a password change, so a key that has gone bad
since cannot stand between a person and their password.

**Argon2id in the browser.** The Web Crypto API has no Argon2, so the page's OpenPGP
module carries BLAKE2b and Argon2 - some three hundred lines on 32-bit halves, no
library vendored - proved against RFC 7693's and RFC 9106's own vectors in
`check-portal-openpgp.js`, and at the mailbox's own parameters against the reference
implementation (argon2-cffi), since the RFC's vectors run at 32 KiB. The reference
index of RFC 9106 section 3.4.1.2 squares a 32-bit word, which a double cannot do
exactly; the module computes that square's high word from 16-bit halves, so the
derivation is the RFC's at every size. The parameters are RFC 9106's second
recommendation, three passes over 64 MiB in four lanes. Measured on 18 September 2026: 1.6 seconds in Node
24 on the development machine, once per sign-in; a phone is expected to take several
seconds, and it was not measured on one. The row stores its parameters, so a page
that lowers the memory for a device that cannot afford it (the server accepts 8 MiB
to 1 GiB, one to ten passes, one to sixteen lanes) is read back with what it stored.

**A password change.** Through the webmail, the change carries the key sealed again
under the new password in the same request (`POST /api/v1/me/password` with a
`zero_access` member), and the server refuses a change without it for a zero-access
mailbox (409, `zero_access_rewrap`), because a change that went ahead would leave
the key sealed under a password nobody has. A password an administrator sets, or a
reset link sets, cannot re-wrap: the page never saw the old one. At the next sign-in
the page finds the copy will not open, and asks for the previous password or the
recovery key; either opens the key, and the page seals it again under the current
password at once (`PUT /api/v1/me/zero-access/rewrap`). A sign-in by passkey, or on
an app password, has no password to derive from; the reader then offers **Unlock
your mailbox**, which asks the same question.

**Switching off** (`DELETE /api/v1/me/zero-access`) clears the account's switch and
removes the two copies. The key stays in the ring, the mail already sealed stays
sealed and opens with it, and new mail is stored as it arrives. Removing the key from
the ring while the switch is on is refused (409, `zero_access_key`), and so is revoking
it through the key page, for the same reason: a revoked key encrypts nothing.

Switches and surfaces
---------------------

* `ZeroAccessAllowed` in `hm_settings` (`zero_access_allowed` under
  `/api/v1/settings/openpgp`, the Deck's OpenPGP settings page): OFF. An
  administrator allows the opt-in with their eyes open, once; allowing it switches
  nobody on. Turning it off again stops new opt-ins and changes nothing for a
  mailbox already switched.
* `accountzeroaccess` on `hm_accounts`: the person's switch. Read-only to an
  administrator everywhere - `Account.ZeroAccess` over COM, `zero_access` in
  `GET /api/v1/accounts/{address}`, a line on the Control Panel's account dialog and a
  card on the Deck's account editor - because a switch an administrator could throw
  would not be a promise about what an administrator cannot read.
* `GET`, `PUT`, `DELETE /api/v1/me/zero-access` and `PUT .../rewrap`, self-service;
  the three writes are refused on an app password. The OpenAPI document describes
  each.
* The backup carries `hm_zeroaccess` (`AccountStores`), so a restore brings the
  switch and the copies back together; a restore of an archive taken before the
  switch was thrown brings back an ordinary mailbox.

What is not built
-----------------

A local bridge that decrypts for an IMAP or POP client; search over the decrypted
text in the browser; encryption of mail already in the mailbox when the switch is
thrown (only what arrives afterwards is sealed); Argon2 measured on a phone.

Tests
-----

`OpenPgpWriterTester` in the server; `build/check-portal-openpgp.js` (BLAKE2b and
Argon2 against the RFCs, the server-sealed fixture opened by the page and by gpg);
the Linux CI step above; `build/portal-script-test.js` (the switch-on that refuses
until the recovery key is typed back and the consequence ticked, the sign-in that
opens the key from the password, the reader opening a sealed delivery, the recovery
key after a reset, the password change that carries the key, the switch-off, the
server-wide switch off, a key that expires left out, the switch on with no row, and
the sign-out that forgets it all); and `RegressionTests/SMTP/ZeroAccess.cs` on a bench: a
delivery's file is PGP/MIME with the kept headers and no plaintext, to a Curve25519
key and to an RSA one, IMAP FETCH answers the ciphertext, the administrator's read is
403, search finds the subject and not the body, a journal rule's copy is plaintext, a
message that arrived encrypted is stored as it arrived, the Sent copy of a message
sent from the webmail is sealed, a seal that cannot be made refuses the delivery and
tells the sender, a key that expires is refused at the switch-on, the mailbox key
cannot be removed or revoked while the switch is on, the opt-in is refused while the
server-wide switch is off, and with the switch off nothing changes - each with its
negative control.
