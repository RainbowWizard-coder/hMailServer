Mailbox encryption at rest
==========================

A mailbox whose messages are stored encrypted to a key of its own. The key is the
account's; it opens when the person signs in with their password and closes when they
have gone; mail that arrives while nobody is signed in is encrypted all the same, because
encrypting needs only the key's public half. When a password is lost, the domain's
recovery key - opened with a passphrase the administrator keeps and this server never
stores - opens the key again. This is Dovecot's *mail-crypt* with per-user keys, built
on the message store's container ([MessageStore.md](MessageStore.md)) as its stage 3.

It is off for every account until an administrator switches it on for that account.


What it protects, and what it does not
--------------------------------------

**It protects the store and every copy of it** - a disk taken out of the machine, a
snapshot of the volume, a backup archive, the data directory copied somewhere, the
database dumped - once the account has signed in with its password after the switch.
Each then holds the messages as ciphertext and the key only in forms that need the
account's password or the domain's recovery passphrase to open.

**Until that first sign-in it does not protect the key from whoever has the machine's
own secret.** The switch has no password to wrap the key by, so it is held for the first
sign-in under this machine's secret store (*pending*, below): on Linux that is the key
file `.hmailserver-secret-key` **inside** the data directory, so a copy of the data
directory together with the database - or the disk itself - opens every pending key; on
Windows it is the machine's DPAPI key, on the same disk. An account that only receives
mail and never signs in with its password stays pending; the Deck's account card says
so.

**It does not protect the mail from the running server, nor from whoever runs it.**
While the key is open the server reads the mail - that is what lets IMAP, POP3, the
webmail, search and the rules work as they always have - and an administrator who
controls the server can capture a password as somebody signs in. A person who needs the
server itself not to be able to read their mail wants a [zero-access
mailbox](ZeroAccess.md), which is a different promise:

| | Encryption at rest (this) | Zero access ([ZeroAccess.md](ZeroAccess.md)) |
|---|---|---|
| Who switches it on | The administrator, per account | The person, from the webmail, when allowed |
| The key | An X25519 key this server makes and holds while the person is signed in | The person's OpenPGP key, made in the browser; this server never holds it |
| Who can read the mail | The server, while the key is open | The person, in the browser or a client holding the key; never the server |
| IMAP and POP3 | Hand out the message as it arrived | Hand out the ciphertext |
| Search | Works, reading each message while the key is open | Finds the kept headers only |
| A lost password | The administrator opens the key with the domain's recovery key | The person's recovery key, or the mail is lost |
| What a thief of the disk or a backup gets | Ciphertext (and a pending key, on Linux, with the data directory) | Ciphertext, and the kept headers in the clear |

**The two are never on one mailbox.** Switching encryption at rest on is refused for a
zero-access mailbox (409), and switching zero access on is refused for a mailbox with a
mailbox key (409), each with the sentence that says why. A zero-access mailbox's mail is
already ciphertext to a key this server cannot open; a second layer to a key it can
would promise nothing more and cost a key to lose.


Switching it on
---------------

1. **The domain needs a recovery key first.** On the Control Deck's domain page, the
   *Mailbox recovery key* card; over REST, `PUT /api/v1/domains/{domain}/mailbox-recovery`
   with `{"passphrase": ..., "hint": ...}`. The passphrase is at least 12 characters,
   and it is the one thing between a stolen database and every encrypted mailbox of the
   domain: keep it where this server is not. The hint is kept, in the clear, for the
   person who has to remember which passphrase it was.
2. **Then the account.** On the account's page, the *Encryption at rest* card's
   *Switch encryption on*; over REST, `PUT /api/v1/accounts/{address}/mailbox-encryption`
   with `{"enabled": true}`. The account gets its key, and from that moment every
   message stored in its folders is encrypted to it. **The switch-on leaves the key open**
   in memory, as a sign-in would, and **ends every session the account had** - its IMAP
   and POP3 connections, its SMTP submission sessions and its webmail sessions, as SCIM's
   deactivation ends them: each was signed in before the mailbox was encrypted, by
   whatever credential it was - an app password, a token, a passkey, the master user -
   that could never have opened it. A mail client sending at that moment sees its
   connection drop and sends again. The person signs in again with the password, which
   wraps the key (below). The key closes as any key does. Two switch-ons of one account
   at once make one key: the second is answered 409, *made a moment ago, by another
   request*.
3. **Then what is already there.** Mail stored before the switch - and the account's
   deleted items kept before it - stays as it is until `hmailserver --encrypt-store
   --account <address>` rewrites it, with the server stopped (below).

Refused, with a sentence: an account whose domain has no recovery key; a zero-access
mailbox; an account that signs in against a Windows directory - whose password this
server never holds and whose changes it never sees, so its key could not be kept wrapped
by it; and **an account with a second factor (TOTP) enrolled**. With a secret enrolled
the account's password stops being a mailbox credential wherever no second factor is
asked - IMAP, POP3, SMTP, ManageSieve, DAV - and an app password becomes the only one,
while an encrypted mailbox opens only with its own password: every mail client would be
shut out. For the same reason a second factor cannot be enrolled on an account that has
a mailbox key (COM's `Account.EnrolTOTP` refuses it). The two are not put on one account.

An account made by single sign-on or SCIM with no password (OIDC, Entra) has none to
wrap its key by, and stays pending until an administrator sets one - which wraps the
pending key - or the person signs in with one.

**Switching it off** (`{"enabled": false}`, *Switch encryption off*) stores new mail as
it comes. What was stored encrypted stays encrypted, and the key stays to read it;
`hmailserver --decrypt-store` puts it back in the clear. Once nothing is encrypted to
the key, `DELETE /api/v1/accounts/{address}/mailbox-encryption?confirm=forfeit` removes
it - and with it, any message still encrypted to it, for ever, which is why the route
wants the word, and why it is **refused while a legal hold covers the account**
([Discovery.md](Discovery.md)): the key is the only way to the held mail.

A mailbox with a key **stays in its domain**: its key is sealed to its own domain's
recovery key, which the other domain's cannot stand in for, so a move to another domain
is refused (409) - by REST, COM's `Account.Move` and `Utilities.MoveMailbox`, SCIM's
rename and a saved `Account.Address` alike ([MailboxMove.md](MailboxMove.md)). A rename
inside the domain goes ahead. To move one, put its mail back in the clear
(`--decrypt-store`) and forfeit its key first.


The keys
--------

**The mailbox key** is an X25519 key pair (RFC 7748), one per account, in
`hm_accountkeys` (schema 6099). Its public half is stored as it is. Its private half is
kept in up to three forms: two that need the account's password or the domain's
passphrase, and one, until the first sign-in, that this server can open by itself.

* **Wrapped by the account's password** - a key-encryption key derived from the
  password with **Argon2id** (RFC 9106) at the server's own password-hash parameters,
  never below 19 MiB and two passes, and a random 16-byte salt; the private key under it
  with AES-256-GCM, bound to the key's public half. On a build whose OpenSSL has no
  Argon2 (3.0 and 3.1, the Linux distributions' before 3.2) it is PBKDF2-HMAC-SHA256 at
  600,000 iterations instead, and the wrap says which. Empty until the first sign-in.
* **Sealed to the domain's recovery key** - an ephemeral X25519 agreement with the
  recovery key's public half, put through HKDF-SHA256, the private key under AES-256-GCM,
  bound to both public keys and to the account's key.
* **Pending**, between the switch and the account's first sign-in with its password:
  under this machine's own secret store (DataProtector: DPAPI's machine key on Windows,
  the key file `.hmailserver-secret-key` **inside** the data directory on Linux). The
  switch has no password to wrap the key by; the first sign-in with it does, and the
  pending copy is removed in the same write. **Until that sign-in, the server - and
  whoever has the machine's secret with the database - can open the key without the
  password.** Mail that arrives meanwhile is encrypted all the same.

**The domain's recovery key** is an X25519 key pair in `hm_domainrecovery`: its public
half, and its private half wrapped by the administrator's passphrase (Argon2id, as the
password wrap). The passphrase is never stored; without it the recovery key opens
nothing. It cannot be removed while any account of the domain has a key.

**Replacing it** needs the current passphrase, and seals every account's key of the
domain to the new key. Every account's key is opened and sealed to the new key in memory
before anything is written - one that cannot be opened changes nothing - and then the
writes go in an order that loses nothing wherever it stops, since there is no
transaction across them to lean on: first the domain's row, in one statement, with the
new key **and the key it replaces beside it**, both wrapped by the new passphrase; then
each account; then the replaced key goes. Cut short anywhere, every account still opens
with the new passphrase - sealed to the new key or to the one beside it - the recovery
card says the replacement did not finish, and the next replacement (given the new
passphrase as the current one) finishes it first. **One replacement runs at a time**, and
a switch-on waits for it - from its read of the recovery key to its stored key - as does
the recovery key's removal and its restore from a backup: a switch-on in the middle of a
replacement would seal the new account's key to the key the replacement then removes, and
two replacements at once (a retry after a timeout) would leave accounts sealed to one
run's key while the row holds the other's. The lock is the server's own, so a second
server on the same database is not held by it.

The forms are text, in the database, and a form this build does not know or parameters
outside its bounds are refused rather than tried: a row asking for a trivial derivation,
or for a gigabyte and a minute, is damage or an attack, not a key.

**A row whose public half is not a key is damage**, and taken neither for "no key" nor
for "not known": nothing can be encrypted to it or opened with it, and waiting will not
change that. It is answered at once, with one sentence naming the way out - *...its key's
row in the database holds no key, so nothing can be stored in it or read from it until
an administrator removes the row - DELETE
/api/v1/accounts/{address}/mailbox-encryption?confirm=forfeit, after which the mail
encrypted to the key it held can never be read - or puts it back from a backup* - to the
account and to the organisation's own:

* a **delivery** to the account is bounced at once (4.3.0, this server's failure, so a
  list manager does not strike the address), with no copy made and no rule, forward or
  auto-reply run. A sender in a domain this server hosts is given the sentence; **any
  other sender is told only that the recipient's mailbox cannot receive mail at the
  moment** - nothing of how it is stored or how it is mended - and the error log carries
  the whole, with the message and the address (HM7365);
* **IMAP** answers `NO [CONTACTADMIN]` - RFC 5530's code for "the user should contact the
  system administrator or support desk", the one whose remedy is an administrator's act;
  `[UNAVAILABLE]`, which the temporary refusal above keeps, says a subsystem is down and
  tells a client to try again - and an `APPEND` the same; **POP3** `-ERR [SYS/PERM]`;
  **REST** 409 - the webmail's sign-in, a password change, the recovery, a draft, a
  message put in, switching it on or off;
* **replacing the domain's recovery key** is refused while the row is there (409),
  naming the account and the way out, and nothing is changed - so that a row put back
  from a backup still finds the recovery key it was sealed to.

The forfeit removes the row, as it removes any key, and mail is stored as it comes again.
Its messages are kept out of the index as a keyed mailbox's are. **Under a legal hold the
forfeit is refused** ([Discovery.md](Discovery.md)), so a custodian whose row is damaged
has every message to it bounced at once and none kept - the hold keeps what the mailbox
holds, and nothing reaches it - until the row is put back from a backup (which the
restore does, and the key then opens as it did) or the hold is released and the row
forfeited.


When the database cannot be read
--------------------------------

**Which accounts are keyed is a set in memory**, in the shape of the legal holds' set
(DiscoveryHolds): each account with a key row, whether its key is switched on, and the
key's public half. It is read whole from `hm_accountkeys` when the server starts,
changed by every write this server makes to the table as it makes it - the switch-on
and off, the forfeit, an account deleted or made - and read whole again at the next
question after a restore wrote the table. **A server with no keyed account has an empty
set and asks the database nothing about keys**, for no delivery, sign-in or request. A
key row written behind the server's back - by hand, or by a second server sharing the
database - is seen at the next start or restore.

**An account outside the set is not keyed, and nothing about it waits on the database.**
A read of the table that fails keeps the set the server had. So **while the database is
unreachable**, a server with keyed accounts decides what every message is stored as
from memory - a keyed account's mail encrypted to the public half the set holds, every
other account's as it comes - and whatever else a delivery needs of the database
(its row, above all) fails or succeeds as it does for any mailbox. An open key stays
open and a session is never ended for the keys. What needs a key's row waits for the
database: opening a closed key at a sign-in, which is refused with *...could not be read
from the database just now... Try again in a moment* (IMAP `NO [UNAVAILABLE]`, POP3
`-ERR [SYS/TEMP]`, the webmail 503) rather than taken for a wrong password, and wrapping
a closed key by a new password, refused 503 the same way.

**The answer is *unknown* only while the server has never read the table** since it
started - its read at the start failed, and none since. Unknown is always temporary, and
never a guess:

* a **delivery** to such an account waits in the queue: the recipient's row stays, and
  the message is tried again - at the queue's quick-retry interval for its first tries
  when the queue has one (`QuickRetries`, `QuickRetriesMinutes`), then on the SMTP retry
  schedule (a posting the list manager could not read waits the SMTP retry interval
  instead, as it always has); the sender is told -
  4.3.0, this server's own failure - only when the retries run out. It is never bounced
  while retries are left, never discarded, and never stored in the clear. An outside
  recipient of the same message whose own retries ran out meanwhile is reported once
  and taken off the message, so the hold does not offer it again. A hold whose counted
  reschedule the database will not take, twice, is rescheduled without the try counted
  and with the first-pass mark in the same statement - the next pass does not sign, seal
  or mirror it again - and said in the error log once; when the database takes neither,
  the message is unlocked all the same, taken again at once, and its first-pass work may
  run a second time, which the error log says. It is never left locked, unseen, until
  the next start. A posting the list manager could not read is held the same way;
* **IMAP** refuses the sign-in `NO [UNAVAILABLE]`, **POP3** `-ERR [SYS/TEMP]`, an `APPEND`
  `NO [UNAVAILABLE]`; **the webmail and REST** answer 503 - a session is neither let in
  nor ended - and so do a draft, a message put in over REST, a password change, the
  recovery and a move to another domain (and the switch and the forfeit whenever the
  key's row cannot be read);
* the **indexers** leave its messages unindexed for their next pass - the backfill goes
  past them and the reconciliation finds them once the table reads, and meanwhile
  reconciles nothing sooner than its hourly turn;
* nothing is ended, revoked, deleted or bounced for not knowing.

The set is read again at the next question five seconds on at most, and the error log
says it could not be read once a minute (HM7366).


Signing in: when the key is open
--------------------------------

A sign-in that carries the account's own password opens the key: `AccountLogon::Logon`
is the door every password sign-in passes - IMAP `LOGIN` and `AUTHENTICATE PLAIN`, POP3
`USER`/`PASS` and `AUTH PLAIN`, SMTP `AUTH`, ManageSieve, the CalDAV and CardDAV
services, the webmail's sign-in and the REST API's Basic - and at its success the key is
unwrapped and held in the server's memory, by account. An app password does not open
it, nor does another account's password signing in as this one.

While it is open the server reads the account's mail as it reads any other. It closes
**`MailboxKeyIdleMinutes`** (15; 1 to 1440; `mailbox_key_idle_minutes` in
`/api/v1/settings`) after **the owner's** last use once nothing holds it: an IMAP or POP3
session of the owner's holds it for as long as it is signed in, and a sign-in with the
password and every request of the person's own webmail session count as a use. Reads by
anybody else do **not** keep it open - a delegate's client reading a shared folder, a
push, a support session opened into the mailbox - so the key does close once its owner
has gone. A sweep every minute, the first thing that minute's task does, closes what is
idle, writes the private key over and forgets what was decoded with it. A restart
closes every key.

**A sign-in that cannot open the key is refused where it would read the mailbox:**

* **IMAP and POP3** sign a keyed mailbox in only with its own password and only with its
  key open after it. An app password, an OAuth2 token (`XOAUTH2`, `OAUTHBEARER`), a
  client certificate (`EXTERNAL`) or a Kerberos ticket (`GSSAPI`) is answered
  `NO [UNAVAILABLE]` (POP3: `-ERR [SYS/PERM]`; `[SYS/TEMP]` for the temporary refusal
  above, and IMAP `[CONTACTADMIN]` for a damaged row) with *This mailbox is encrypted at
  rest and opens only with its own
  password...*; **the IMAP master user** - `AUTHENTICATE PLAIN` with this mailbox as
  the authorization identity - is refused the same way, *whether or not* the owner's
  key happens to be open, so it neither reads the mailbox while the
  owner is online nor holds the owner's key open after the owner has gone; a password
  that did not open the key - SCRAM, which never hands the server the password, with the
  key closed, or a password whose wrap is an earlier one's - with *...its key did not
  open: sign in with the account's own password (LOGIN or AUTHENTICATE PLAIN)...*. The
  credentials were right, so none of these counts as a failed logon. An IMAP or POP3
  session signed in before the switch-on that the switch-on could not reach - one the
  server could not record in the account's device list - holds no key, and is ended at
  its next command (`* BYE [UNAVAILABLE]`, `-ERR [SYS/PERM]`).
* **The webmail and the REST API's account routes**: a session is made only on the
  account's password; a passkey, a single sign-on or an app password is refused (403,
  code `mailbox_key`) with the same sentence. **An app password reaches none of the
  `/api/v1/me` routes** of a keyed mailbox - over Basic, or holding a session started
  with one before the switch - whether the key happens to be open or not (403,
  `mailbox_key`). **A session is good while the opening of the key it was made under is
  the current one** - not merely while some key is open: one whose key has closed since
  - idle past the setting, or a restart - or that was made before the mailbox was
  encrypted is **ended** at its next request (401), with every other session of the
  account made under an earlier opening, so that it cannot come back to life when
  something else opens the key. A session made under the current opening is not ended
  with them: a stale session's request never ends one signed in a moment ago. The page
  asks for the password again.
* **SMTP, ManageSieve and DAV** are not refused: they do not read the mailbox. A password
  sign-in to any of them opens the key as a side effect.


Delivery without a key
----------------------

Mail arrives when nobody is signed in, so it is encrypted to the key's **public** half,
one file at a time: an ephemeral X25519 key pair made for that file, its agreement with
the account's public key put through HKDF-SHA256, and the message under AES-256-GCM with
a random nonce. Reading needs the account's private key; writing needs nothing secret.

The container, as [MessageStore.md](MessageStore.md) describes it byte by byte, with
stage 3 as its last stage:

| Offset in the stage's parameters | Length | What |
|---|---|---|
| 0 | 1 | The stage's version, 1 |
| 1 | 32 | The account's X25519 public key: which key opens the file |
| 33 | 32 | The ephemeral public key made for this file |
| 65 | 12 | The AES-256-GCM nonce |
| 77 | 16 | The AES-256-GCM tag |

The associated data the tag covers is the container's magic, version, flags and
original size, the stages under this one with their parameters, and both public keys - so
none of them can be changed without the file being refused. It does **not** bind which
message the file is: integrity is per file, as in Dovecot's mail-crypt, and somebody who
can write the store could swap two of one account's encrypted files, or put back an
older version of one, and each would still open. **Compression runs first**, when
`MessageStoreCompression` is on: ciphertext does not compress. The header's SHA-256
field is 32 zeros: a digest of the message in the clear would confirm a guess at what the
file holds, and the tag refuses every changed byte anyway. Its original size stays in the
clear, as it is in `hm_messages.messagesize`, so sizes, quotas and `RFC822.SIZE` are read
without the key.

**What is encrypted: every message stored in the account's own folders from the switch
on, by every writer.** Delivered; appended over IMAP; collected by the IMAP mirror;
brought in by a migration or an import - read first, so an import with the key closed
still gets its row; a webmail draft; a message or an `.eml` put into a folder over REST; a
message COM or an event script adds to a folder (`Messages.Add`, then `Save`); a copy
from another mailbox or a public folder; the webmail's Sent copy; a message a mailbox
restore puts back and a deleted item put back from the recover-deleted store, each
encrypted on the way in when it was kept before the switch. A draft, a message put in
over REST, and one COM or a script adds are encrypted **in memory** and never written in
the clear at all - a draft's attachments included, which the webmail's upload makes into
their parts in memory, with no file in the temporary directory. A delivery, an APPEND,
the mirror and an import write the message as it comes first - they cannot do otherwise
- and when it is stored encrypted **the plaintext file is overwritten with zeros** before
it is removed. Between the two renames that put the encrypted file in its place the
plaintext is moved aside (`<name>.plaintext-<GUID>`) and the container is beside it
(`<name>.encrypting-<GUID>`); a crash there leaves both, and **the next start puts it
right** before anything is delivered: the container is renamed into the name when the
name is empty, the plaintext is overwritten and removed once the name holds the message,
and when nothing else holds it the plaintext is put back under its name, never
overwritten. The sweep walks the keyed mailboxes' own directories and reads **file names
only**: a leftover is a file whose name, split at the last mark, has a name part that is
not empty and a GUID after the mark exactly as the server writes one (`{8-4-4-4-12}`); any
other file is left alone, and so is everything above the file - a mailbox named
`x.plaintext-y@`, a domain or a data directory may carry either mark. It runs at every
start, a reinitialise's included, before the listeners open; a file whose name the file
system's conversion refuses is passed, and the leftovers already found beside it are
still put right; a directory it cannot walk is reported (HM7367) and passed, and the
server starts all the same. A COM rewrite under
way while a reinitialise sweeps can lose its staged container, and its `Save` then fails
with the stored message as it was. When the keys cannot be read at the start, the sweep
waits for a later start.

A message that cannot be stored encrypted is **refused** rather than kept in the clear:
larger than a container holds (64 MB), or a write that failed. A delivery bounces -
**5.6.0** for a message too large, which sending again will not change; **4.3.0** for
this server's own failed write, as a failed save is told, so that a list manager does
not strike the address - an `APPEND` is refused, and the error log carries HM7365; the
refused plaintext is overwritten with zeros before it goes. Which of the two a delivery
says is what the store said the refusal was. A mailbox whose encryption is not known is
not a refusal: its delivery waits in the queue (*When the database cannot be read*,
above). A delivery file is never shared with other recipients under `DeliveryHardLinks`,
since this copy is under this account's key alone.

A stored message **rewritten through the server** - COM's `Message.Save`, a script's, a
rule's - is read from the container in memory, written again in memory, encrypted to
the key it was encrypted to, written beside the stored file, read back, and only then
renamed over it: the plaintext never reaches the disk, and a rewrite that cannot be
encrypted is not made - HM7362, and the stored message is as it was. A message stored
**before** the switch and rewritten after it stays as it was stored, in the clear, until
`--encrypt-store`.


Parts held once: none
---------------------

The message store holds a large MIME part once, as a blob every message carrying the
same bytes shares ([MessageStore.md](MessageStore.md)). **A blob shared between accounts
cannot be encrypted under one account's key**, and encrypting blobs under a key that is
not an account's would be a key the server can always use - the thing this is meant not
to have. So an encrypted mailbox holds no part once: its containers name no references,
every part stays inside the message's own container, compressed and encrypted with it,
and a part this mailbox receives is never the same file as another mailbox's copy of it.
What that costs is the de-duplication, for these mailboxes only.


The index
---------

The full-text index (`hm_messageindexterms`) stores the words of every message, per
account, in the clear. For an encrypted mailbox that would be the leak the files were
encrypted to prevent, so **it holds none of its words**: the indexer marks every message
of a keyed account as one to be read at every search (the overflow marker, which the
index has always given a message it could not read) and stores no term of it, whether
its key happens to be open or not. The sort's copy of the headers (`hm_message_metadata`:
From, To, Cc, Subject, Date) is not written for it either. Both are deleted for the
account at the switch-on, since what they held was built from the text of mail stored
before it - after the switch-on puts the account in the keyed set, and the indexers take
a second look at the set after each batch they write, on every way out of it, dropping
what they wrote of an account that was switched on while they read it. The look costs
nothing where no account is keyed. A message whose mailbox's encryption is not known is
left unindexed - neither its words nor the marker - for the next pass. So the indexed
count (`TotalIndexedCount` in COM, `total_indexed_count` in `GET
/api/v1/settings/indexing`) stays short of the message count by the messages in
encrypted mailboxes: they are not waiting to be indexed, and never will be.

**Search still works, while the key is open**: IMAP `SEARCH`, `SORT` and the webmail's
search read each message, as the server did before it had an index - parsing it in
memory, never writing a copy of it anywhere. It costs what that cost - a search over a
large encrypted mailbox reads every message - and it is available only while the person
is signed in, because only then is the key open.


Copies and moves
----------------

A copy within the mailbox keeps the file as it is, encrypted to the same key. A copy
between a mailbox encrypted at rest and any other - into a folder delegated to another
account, into a public folder, out of an unencrypted mailbox or a public folder into an
encrypted one - is re-encoded for the mailbox it goes into: decrypted with the source's
key, which must be open (a delegate copying out of a mailbox whose owner is not signed in
is refused), and stored as the destination stores mail - encrypted to its key, or as it
arrived. So a message is never left under a key its new owner cannot open, and never in
the clear in an encrypted mailbox. A public folder is nobody's mailbox: a copy into one
is stored as the message arrived.

A folder shared with a delegate is readable by the delegate only while the owner's key is
open - and the delegate's reading does not keep it open. Over IMAP, while the key is
closed, a `FETCH` of anything that reads the message - `ENVELOPE`, `BODYSTRUCTURE`, a
section, the preview - is answered `NO [UNAVAILABLE]` with nothing of the message sent,
and sets no `\Seen`; its UID, flags and size are still answered. A damaged message
(`HM7350`) is left out the same way, to its owner as to anyone, but the rest of the
range is still fetched and the command ends `NO`, so that one damaged message does not
stop a client's sync of the whole folder at itself.


Passwords
---------

**The person's own change** from the webmail (`POST /api/v1/me/password`) opens the key
with the current password and wraps it by the new one, stored right after the account's
row is saved, as its own write: if that write fails (HM7364) the password has changed and
the key is still wrapped by the old one, and the next sign-in says so (HM7360) until the
recovery.

**An administrator setting a password** - the Deck's and the API's account update, the
help desk's reset (`POST /api/v1/accounts/{address}/password`), COM's `Account.Password`
and `Application.ResetPassword`, which is also the Control Panel, and SCIM - wraps the key
by the new password when the key is open (the person is signed in) or still pending. **When
it is closed the change is refused**, with a sentence naming the recovery: a password set
then would open nothing, and the mail would stay unreadable. A **reset link** from the
person's recovery address is refused the same way before the link is spent - somebody who
has forgotten their password has, by definition, a closed key - and sends them to their
administrator. A SCIM change that fails part way and is put back puts back the key's wrap
with the password. **An administrator's password set is not a use of the key**: it wraps
the key and does not keep it open for the person - the key still closes
`MailboxKeyIdleMinutes` after the owner's own last use.

**Recovery** - `POST /api/v1/accounts/{address}/mailbox-encryption/recover` with
`{"passphrase": the domain's, "password": the new one}`, or the account card's
*Recover the mailbox* - opens the domain's recovery key with the passphrase, the
account's key with it, and wraps it by the new password, which becomes the account's
through the password policy, the reuse history and the breach check as every password
does. It is recorded on the audit trail. A wrong passphrase is 403 and counts towards the
auto-ban as a wrong password does.

A password that was set anyway by a path none of the above covers - an event script, a
database edited by hand - leaves the key wrapped by the earlier one: the next sign-in
with the new password is refused at IMAP and POP3, and the error log says so (HM7360),
once an hour. While the key is open, a sign-in with the account's own password, proved
against its stored hash, wraps it by that password again.

### What you lose

**A lost password and a lost recovery passphrase together are the mail lost.** Nobody -
not the administrator, not this project - can open it: that is what the passphrase not
being stored means. The way out is `DELETE .../mailbox-encryption?confirm=forfeit`, which
removes the key and makes every message still encrypted to it unreadable, for ever, after
which a password can be set again and new mail is stored as it arrives.


The command line
----------------

```
hmailserver --encrypt-store [--account <address> | --domain <name>]
hmailserver --decrypt-store (--account <address> | --domain <name>)
```

Run as the user the server runs as, with the server **stopped** (refused with exit 4
while it runs, as `--compress-store` is: [MessageStore.md](MessageStore.md)).

* **`--encrypt-store`** rewrites every stored file of a mailbox with a key that is not
  encrypted yet - mail stored before the switch, and the copies its deleted items keep,
  whose rows' subjects and senders it empties - encrypted to the account's key, which
  needs only its public half. Each file is written
  beside itself, read back and compared byte for byte with what was encrypted, and only
  then renamed over its name. Each **name** is rewritten on its own: a file several
  accounts share under `DeliveryHardLinks` cannot be under one account's key, so this
  account's name gets a file of its own and the others keep theirs. What the old file
  held once is let go of for this name.
* **`--decrypt-store`** is the reverse, for one account or one domain, deleted items
  included: the key is opened with the domain's recovery passphrase, read from the
  environment variable **`HMAILSERVER_RECOVERY_PASSPHRASE`** - never from the command
  line, which every user of the machine can read - and every file encrypted to it is put
  back as plain RFC 822, read back and compared. Switch the mailbox's encryption off
  before starting the server again, or new mail is encrypted once more.

`--compress-store` and `--decompress` leave a keyed mailbox's files as they are and say
how many: encrypting and decrypting are these two verbs' business. `--measure-store` reads
an encrypted file's header and counts it as it is.


What stays in the clear
-----------------------

This is the whole list, each with the reason. Nothing else of a keyed mailbox's messages
is written to the disk by this server.

* **The queue**, and the account's copy while a delivery works on it. A message is
  accepted, scanned, filtered, run through the rules, Sieve and the event scripts in the
  clear - they cannot work on ciphertext - and encrypted when it is placed in the mailbox;
  the plaintext copy is then overwritten with zeros and removed. A copy the rules, Sieve
  or the spam threshold drop before it is placed is removed without being overwritten.
  What a filesystem, a snapshot or an SSD keeps of blocks that were overwritten or freed
  is theirs; the queue file's blocks are freed, as for any mailbox. A delivery copy the
  MIME parser cannot read is copied to the log directory's `Problematic messages`, as for
  any mailbox.
* **An APPEND's literal, the IMAP mirror's message and an import's file** while they are
  written and until they are placed, as the delivery copy: overwritten with zeros once
  stored encrypted, or once refused - except an import's file that is refused, which is
  the only copy there is and stays for the administrator; and an import whose row could
  not be saved after its file was stored, which is put back as it was, in the clear, for
  the next run to import. And the plaintext a crash left beside its encrypted copy
  (above) until the next start.
* **The calendar and the address book.** An invitation delivered to a keyed mailbox is
  filed into its calendar as it arrived (`Imip`: its summary, description, location and
  attendees, in the database), and a reply or a cancellation into the event it answers,
  so that the calendar keeps working. The calendars and address books - CalDAV, CardDAV
  and the webmail's calendar and contacts - are not encrypted by this row, and neither is
  what the server remembers of the account's correspondents (the senders it has heard
  from, for the external-sender note).
* **The quarantine**, the **archive** and **journal** copies, a **forward**, and a
  **confidential message's original** that a keyed account sends (with its subject in
  `hm_confidential`): each is made before the mailbox, of the message as it arrived, and
  belongs to the organisation's records or to the recipients rather than to the mailbox -
  a confidential message's recipients read it when the sender's key is closed. The Sent
  copy of it is encrypted.
* **The database**: every message's row (its size, its flags, its folder, its dates),
  the folder names, the recall rows, the message trace, the domain's recovery hint, and
  a confidential message's subject. **Not** the subjects and senders of the mailbox's
  messages: the sort's copy of the headers is not written for it, and its deleted items
  are listed with no subject or sender in `hm_recoverable` - the list reads them from the
  kept copy, the newest 200, while the key is open. Deleted items kept before the switch
  keep theirs until `--encrypt-store`, which encrypts their copies and empties those
  rows' subjects and senders.
* **Logs** and the **trace**, as for any mailbox.
* **What a reader takes out while the key is open**: an export of the account or of the
  person's data, an eDiscovery search's export, the webmail's download of a message, a
  message sent on from the mailbox (a draft sent later, a redirect), which goes to the
  queue - each is the message, as it is for any mailbox, and each reads it only while the
  key is open (a closed key makes them skip the message, or fail for it, without calling
  it damage). A push notification's subject goes to the phone. Reading - a search, a
  preview, the webmail's views, eDiscovery, the rules and scripts, COM's `Message` -
  parses the message in memory and writes nothing. **SpamAssassin is not taught** by a
  move to or from Junk in a keyed mailbox: teaching it would hand spamd a plain copy of
  the message.
* **Memory**: an open key, and a message decoded a moment ago (kept for half a minute, so
  that a client reading a message in pieces decrypts it once, and dropped when the key
  closes - a decode already under way then is not kept); a message a reader has open
  stays in its memory until it lets go. Memory is freed, not overwritten, apart from the
  key.
* **A backup of the data directory** holds the encrypted files as they are - and, on
  Linux, the key file a pending key opens with; see below.

Until 25 September 2026 this list left out the calendar copy of an invitation, and a
draft's attachments passed through the temporary directory in the clear; both are now as
above.


Backups and restores
--------------------

**A backup with messages** carries the encrypted files as they are; `hm_accountkeys` is
carried with the account ([Backup.md](Backup.md)), and the domain's recovery key with the
domain - with the key a cut-short replacement kept beside it. A restore brings the files,
the key and the recovery key back together, so the mail opens exactly as it did - to the
account's password, or the domain's passphrase. The **pending** copy is under the
machine's own key: on Windows it opens only on the machine that wrote it, and a mailbox
restored on another machine before its first sign-in is opened with the recovery (HM7361
says so at that sign-in); on Linux it opens wherever the data directory's key file goes
(the backup archive does not carry that file).

**A mailbox restore** puts the archived files back as the mailbox stores mail now: a file
encrypted to the account's key as it was stored, and one archived before the account's
mailbox was encrypted at rest encrypted to its key on the way in. One encrypted to a key
the account no longer has - forfeited and made again since - cannot be opened, and is
counted as failed.


Settings and surfaces
---------------------

| Where | What |
|---|---|
| `mailbox_key_idle_minutes` in `/api/v1/settings` | How long an unheld key stays open after its owner's last use: `MailboxKeyIdleMinutes` in `hm_settings`, 15. On REST, the Deck's server settings and the Control Panel's Maintenance > Performance page, Message store tab, which reaches it over the REST API - there is no COM property for it |
| `GET` / `PUT` / `DELETE /api/v1/accounts/{address}/mailbox-encryption` | The account's state (`enabled`, `keyed`, `open`, `damaged` - its row holds no key, and only the forfeit removes it - `pending`, `fingerprint`, `created`, `changed`, `zero_access`, `domain_recovery`, `recovery_current` - whether the domain's passphrase opens the key's recovery copy); the switch; the forfeit (`?confirm=forfeit`). *view accounts* reads, *manage accounts* writes - and every write is refused (403) for an account whose authority the credential does not hold, as a password reset is: a domain administrator does not switch a server administrator's mailbox |
| `POST /api/v1/accounts/{address}/mailbox-encryption/recover` | The recovery: the domain's passphrase and the new password. *manage accounts*, and the same refusal |
| `GET` / `PUT` / `DELETE /api/v1/domains/{domain}/mailbox-recovery` | The domain's recovery key: its state (`exists`, `fingerprint`, `hint`, `created`, `accounts`, `replacement_unfinished`); made or replaced; removed. *view accounts* reads, *manage accounts* writes |
| `GET /api/v1/me/mailbox-encryption` | The person's own view: `enabled`, `keyed`, `open` |
| The Control Deck | The account page's *Encryption at rest* card - the state, the switch, the recovery, and a note when the domain's recovery key does not open the key - and the domain page's *Mailbox recovery key* card, with a note when a replacement did not finish |

Every write is recorded on the audit trail as an event naming the account or the domain
- never the key: the two tables are key material a sign-in writes with no actor on the
thread, and are not audited row by row.


Errors
------

| Code | What |
|---|---|
| `HM7360` | A sign-in with the account's own password did not open its key: it is wrapped by an earlier password. The mail stays unreadable until the recovery. Once an hour an account |
| `HM7361` | A pending key could not be opened at the first sign-in - a database carried to another machine, or a machine key that changed. Recover it |
| `HM7362` | A stored message rewritten through the server could not be encrypted again, so the rewrite was not made: the stored message is as it was |
| `HM7363` | A copy between mailboxes could not be re-encoded for the mailbox it goes into, or whether that mailbox is encrypted could not be read, and was not made |
| `HM7364` | A key row could not be written, restored or put back, or a domain's recovery key in a backup was not a key and was not restored |
| `HM7365` | A message for an encrypted mailbox could not be stored encrypted - or whether its mailbox is encrypted is not known - and was refused, or held for a later attempt, rather than kept in the clear |
| `HM7366` | The accounts' keys (`hm_accountkeys`) could not be read into memory: the server answers from what it read last, or - when it has not read them since it started - treats whether a mailbox is encrypted as unknown. Once a minute at most. Also a sign-in whose opening of its mailbox key stopped at an unexpected error: the sign-in stands, the key stays as it was, and the next sign-in tries again. Once an hour an account |
| `HM7367` | The start's sweep of a keyed mailbox's directory failed - a name the file system's conversion refused, a directory gone meanwhile: that directory is passed, and the next start sweeps it again |

A read of an encrypted message while its key is closed is refused, and said only in the
debug log: it is not damage (`HM7350` is for damage), and a delegate's client or a push
can ask for such a message often. None of these is raised by a server on which no
account has been switched on.


What is thinner than the row
----------------------------

* **No webmail card.** The person's own view is `GET /api/v1/me/mailbox-encryption`;
  the webmail's Security page does not show it yet, and has no *device key* that would let
  a passkey sign-in open the mailbox - a keyed mailbox signs in to the webmail with its
  password.
* **No Control Panel page and no COM** for the switch or the recovery: the Deck and the
  REST API are where they are. `MailboxKeyIdleMinutes` has no COM property either; the
  Control Panel has it on Maintenance > Performance, the Message store tab (the
  *Mailboxes encrypted at rest* card), which reaches it over the REST API, so the REST
  API has to be on, and its settings search finds it there. COM refuses to set a
  password while the key is closed, and a second factor on a keyed account, with the
  sentence, and says nothing else about it.
* **An account's key is never rotated.** Rotating it would mean re-encrypting every file;
  the recovery key can be replaced, and re-seals every account's key.
* **Search reads every message** of an encrypted mailbox, while its key is open.
* **Integrity is per file**, not per message (above).
* **The plaintext's blocks** that a delivery, an APPEND or an import wrote are
  overwritten with zeros in place, which a copy-on-write filesystem, a snapshot or an SSD
  may not honour; a copy dropped before it is placed, and memory, are freed, not
  overwritten.
* **Run on Windows, not on Linux.** The fixture
  `RegressionTests/Infrastructure/EncryptionAtRest.cs` first ran on 25 September 2026 on
  the folded batch35, with the inbound webhook's fixtures: 113 of 115 passed, and the two
  failures were the tests' own - a bounce's quoted-printable text read raw, and IMAP SASL
  never switched on - fixed in 2e19cb336. It needs no lab, so it is in every full Windows
  gate since; batch37's, run on 25 September 2026 across the laptop and the second bench,
  had no failure of it. It is not in the Linux suite.


Tests
-----

`MailboxCrypto::Test` (run by `ClassTester::DoTests`, no server): X25519 against RFC
7748's vector, HKDF-SHA256 against RFC 5869's first case, base64 both ways, and every seal
and wrap round-tripped with its refusals - another key, a wrong secret, a changed byte of
a seal and of a wrap, a copy made for another purpose, parameters out of bounds.
`MessageStoreFormat::Test`: the encrypted container, deflated and not, holding no byte of
the message in the clear, naming no reference and no digest, given back whole and its
start only to the account's key, and refused to another key, to no key, for a changed
byte of its body or of its size, and - deflated - for a changed deflate level, which only
the tag's associated data protects; and `ParseHeader`'s refusals of containers this
server never writes - a stage after the encryption, the encryption twice, deflate twice,
an encrypted one naming a part held once, one carrying a digest - beside the same header
rebuilt unchanged, which opens.

`RegressionTests/Infrastructure/EncryptionAtRest.cs` (REST on 10140), with a negative
control wherever a behaviour has a refusal:

* a delivered message stored as an encrypted container with no plaintext on disk while
  the key is closed, read after a password `LOGIN` that opens the key, beside a sibling
  stored as it came; compression first and no part held once, beside a sibling whose
  attachment is a blob;
* the recovery key needed first and kept while an account is sealed to it; zero access
  refused for a keyed mailbox and encryption refused for a zero-access one and for a
  two-factor one, and a second factor refused for a keyed one, beside a plain account
  that enrols one;
* app passwords refused by IMAP, POP3, the webmail's session start and `/api/v1/me`
  (with the key open), a token refused by `AUTHENTICATE XOAUTH2`, and the IMAP master
  user refused with the owner's key open - each beside the password, or a plain mailbox,
  let in;
* the key shared by two sessions whose LOGINs opened it; with two idle minutes, still open
  seventy seconds after both have gone and, an administrator's password set then, closed
  by three minutes all the same; a webmail session ended when its key closed and
  not brought back when the key opens again, and a stale session's request ending neither
  the session signed in after it nor anything but sessions of an earlier opening; pending
  until the first sign-in, closed by a restart and opened by the password's wrap;
* the switch-on ending an IMAP session idling with an app password - which sends nothing,
  so only the switch-on's drop can end it - an IMAP and a POP3 session, and a webmail
  session signed in before it;
* the keys' table made unreadable with `SimulateDatabaseFailureFor` while the set is in
  memory: a plain mailbox and a keyed one still delivered to, the keyed one encrypted;
  and made unreadable across a restart, so that nothing is known: a delivery held in the
  queue - no bounce, nothing stored - and delivered once the table reads again, IMAP
  `NO [UNAVAILABLE]`, POP3 `-ERR [SYS/TEMP]` and the webmail's sign-in 503 meanwhile;
* a key row that holds no key refused at once for its own account alone - its recipient
  bounced, to a sender this server hosts, with the sentence naming the forfeit, while the
  other is delivered, nothing held, IMAP `[CONTACTADMIN]`, POP3 `[SYS/PERM]`, the
  webmail's sign-in and the switch-off refused naming it - and
  the forfeit removing the row, after which mail is stored as it comes;
* keyed mailboxes named `x.plaintext-y` and `x.encrypting-y`, beside a mailbox `x`,
  keeping every message across a restart, byte for byte;
* the plaintext and the container a crash leaves beside an empty name put right at the
  next start, a leftover plaintext beside a stored message overwritten and removed, and
  one nothing else holds put back under its name as it was;
* a draft with an attachment saved to a keyed mailbox, stored encrypted, with no file
  created in the temporary directory (the watcher counts what the server names its own
  temporary files by, a braced GUID);
* `SEARCH` finding a word while signed in, with no file created in the temporary directory,
  the index holding no term of the mailbox (a search the index would answer for its
  sibling refused on the examine ceiling for it) and the sort's copy of its headers not
  written;
* a COM add, a COM rewrite and an `APPEND` stored encrypted, with nothing left in the
  temporary directory, beside a COM add to a plain mailbox; a copy to a public folder
  stored as it arrived and a copy back encrypted; an import with the key closed given its
  row and stored encrypted;
* deleted items kept encrypted with no subject in the database, beside a plain sibling's
  that keeps it; an item kept before the switch put back encrypted;
* an administrator's reset refused while the key is closed, the recovery with a wrong
  passphrase refused and with the right one opening the mail to the new password, on the
  audit trail; the person's own change carrying the key, the old password refused after
  it; a replaced recovery key opening both accounts of the domain and the old passphrase
  refused; a domain administrator refused a server administrator's mailbox and another
  domain's recovery, beside switching an ordinary account on;
* a keyed mailbox refused a move to another domain while a rename and a plain mailbox's
  move go ahead; the forfeit and its word, refused under a legal hold and let through when
  it is released; the idle setting's bounds;
* `--encrypt-store` encrypting mail stored before the switch and `--decrypt-store`
  refused without the passphrase and putting every file back byte for byte.

`BackupAccountStores.TheMailboxKeyComesBack` carries the key, the domain's recovery key
and an encrypted message through a backup and a restore, and reads the message with the
password after it. `build/deck-script-test.js` holds the two Deck cards and their notes to
their routes.

**Not tested** - code with no fixture behind it yet: a mailbox restore's encryption on
the way in; the IMAP mirror's and a REST message put's encryption; a delegate's copy out
of a mailbox whose key is closed, and the reads of others not keeping a key open;
`DeliveryHardLinks` not sharing a keyed copy; `EXTERNAL`, `GSSAPI` and SCRAM refused, and
the POP3 session's hold; a stale wrap (HM7360); the HM7362, HM7363 and HM7365 refusals of
a write that failed, and so the 4.3.0 and 5.6.0 bounces and the scrub of a refused
plaintext file; a delivery held for an unknown mailbox until its retries run out (the
4.3.0 then), its quick retries, an outside recipient's spent row taken off a held
message, a reschedule that could not be written (the second try, the uncounted one with
the first-pass mark, and neither), and a list posting's quick retries; the at-once
bounce's wording to a sender outside the server (the fixture's sender is local, and is
given the sentence); IMAP's `[CONTACTADMIN]` on an `APPEND`; a replacement of the
recovery key refused for a damaged row; a file name the sweep cannot convert; a
replacement of the recovery key
cut short, and two at once or a switch-on during one (the lock), and two switch-ons at
once (the 409); a restore that fails part way reading the set again; a copy into a
mailbox switched off whose key the set holds; the sweep's HM7367; SCIM's put-back of the
wrap; a directory account refused the switch; the 12-character minimum and the auto-ban count of a wrong passphrase over REST
(the Deck harness checks the minimum); the COM, SCIM and reset-link password refusals and
their 503s; an import whose row save fails put back as it was; the index dropped at the
switch-on and the indexers' second look; an IMAP or POP3 session ended at its next
command because the switch-on could not reach it; exit 4 of the two verbs beside a
running server; and `GET /api/v1/me/mailbox-encryption`.
