# eDiscovery

A lawyer, a regulator or an internal investigation asks for the mail of some
people over some months that mentions some words: found, kept from deletion
while the matter lasts, and handed over in a form another tool can read, with a
record of who looked for what. That is what Google Vault and Microsoft Purview
do, and what this server does with **cases**, **searches**, **holds** and
**exports**.

The archive's own legal hold ([Journaling.md](Journaling.md) names the archive;
`/api/v1/archive/{id}/hold`) keeps one archived copy from the archive's retention
sweep. A case is the rest of it: it finds the mail across every mailbox and the
archive, holds whole mailboxes against every way this server deletes a message
that is in them - and against the account's own filters dropping one as it is
delivered - and exports what it found. What a hold does not reach is listed
below: the server's and a domain's rules, spam and virus deletion before a
message is an account's, the delivery queue, and the stores beside the mailbox.

| Route | What it does |
|---|---|
| `GET /api/v1/discovery/cases` | Every case, newest first, summarised |
| `POST /api/v1/discovery/cases` | A case opened: `{name, description}` |
| `GET /api/v1/discovery/cases/{id}` | One case with its searches, holds and exports |
| `POST /api/v1/discovery/cases/{id}/close` | The case closed, and every hold of it released |
| `POST /api/v1/discovery/cases/{id}/searches` | A search queued |
| `GET /api/v1/discovery/cases/{id}/searches/{search}` | Its state, its counts and a page of its hits |
| `POST /api/v1/discovery/cases/{id}/searches/{search}/export` | An export of its hits queued |
| `POST /api/v1/discovery/cases/{id}/holds` | An account, a domain, or the archived copies a search matched, put on hold |
| `DELETE /api/v1/discovery/cases/{id}/holds/{hold}` | A hold released |
| `GET /api/v1/discovery/cases/{id}/exports/{export}` | The export's zip, downloaded |

Times are seconds since 1970, UTC, and `0` for "not yet".

## Cases

A case has a name (one line, up to 255 characters), a description (up to 1,000),
who opened it and when, and a state: **open** or **closed**. Everything else
belongs to one: the searches run for it, the holds it placed, the exports it
made. Closing a case releases every hold of it that is still live - each
release on the audit trail - and a closed case runs no search, places no hold
and makes no export; its searches, holds and exports stay readable, and its
exports downloadable. A case is never deleted: the record of what was looked
for is the point. Nor does an address's erasure reach it: the custodians its
queries name, what its holds hold and what its results and exports list are
that record ([PersonalData.md](PersonalData.md)).

The files a case makes are under the data directory, in
`Discovery\case-<id>\`: `search-<id>.json` for each search's results and
`export-<id>.zip` for each export. They are not in the backup: a message backup
leaves the `Discovery` folder out of its copy of the data directory, and a
restore of the data directory leaves it where it is, because the four tables
that name those files are not in the backup either and a restore does not
change them ([Backup.md](Backup.md)). So a server rebuilt from a backup has no
cases, no searches, no exports and **no holds**: place the holds again before
its mail can be deleted. Nothing removes an export: when one is no longer
needed, delete its zip from the case's folder by hand. Its row stays, and its
download then answers `404` with the path it looked in.

## Searching

`POST /api/v1/discovery/cases/{id}/searches` takes:

```json
{
  "custodians": ["alice@example.com", "sales.example.com"],
  "from": "2026-01-01",
  "to": "2026-06-30",
  "words": "quarterly \"revised forecast\"",
  "attachment": { "name": "report", "type": "application/pdf", "min_kb": 10 },
  "scope": "both"
}
```

* **custodians** (required, at most 100): an address - one account, and its
  archived copies filed under that address - a domain - every account in it,
  and the archive's copies filed under the domain - or `*`, every account on the
  server and the whole archive, the copies of mail from outside included. An
  address with no account now is still searched in the archive under that
  address, and listed as a custodian whose mailbox could not be searched.
* **from**, **to** (optional, `YYYY-MM-DD`, inclusive): the day a message is
  dated - its `Date` field, read as UTC; a message with no `Date` that reads as
  one is dated by the day the server stored it (delivered it, archived it, or
  for a deleted item, deleted it). A message whose date cannot be read at all is
  listed as not searched rather than guessed about.
* **words** (optional): every word must be found; `"a quoted phrase"` is one.
  A word is found when it is a case-insensitive substring of what **IMAP SEARCH
  TEXT** reads: the decoded header, the text body, the HTML body, and the text of
  the attachments whose media type is `text/*`. A search and an IMAP `SEARCH
  TEXT` of the same mailbox for the same word find the same messages.
* **attachment** (optional): at least one attachment whose file name contains
  `name`, whose media type is `type` - or, for a type ending in `/` or `/*`, of
  that family, `image/` for every image - and whose decoded size is at least
  `min_kb` kilobytes. Each part is optional; `{}` is any attachment at all.
* **scope**: `mailboxes`, `archive` or `both`, the default. A search that reads
  the archive needs `view_archive` as well (see *Who*), so a role without it
  names `mailboxes`.

There is no sender or recipient criterion: a word is looked for in the whole
decoded header, the From and To fields among it, and a person is named as a
custodian.

The query is refused with the reason (`400`) for no custodian, a custodian that
is neither an address, a domain nor `*`, a date that is not `YYYY-MM-DD`, `from`
after `to`, a scope that is none of the three, a member the query does not
have, more than 50 words, or a query longer than 1,800 characters once written
down - the audit trail records the query whole, and its detail is 2,000
characters. `202` answers with the search's id; the search is queued.

### Where it looks

For each custodian account, three places:

1. **Its folders** - every message of `hm_messages` the account owns, read a
   page at a time by id, whatever folder it is in.
2. **Its deleted items** - what a hold kept, and what `RecoverDeletedDays` keeps
   ([RecoverDeleted.md](RecoverDeleted.md)), listed under the folder
   `Recoverable Items/<the folder it was deleted from>`.
3. **The archive** (`scope` archive or both) - the rows of `hm_archiveindex`
   filed under the address or the domain, read a page at a time by id, because
   the archive index's own search stops at 1,000 rows. The index holds no body,
   so each archived file is read.

A search **copies nothing**: it lists. Its results go to
`Discovery\case-<id>\search-<id>.json`, and `GET .../searches/{search}` answers
them a page at a time (`offset`, `limit` up to 1,000). A hit names its `kind`
(`mailbox`, `recoverable` or `archive`), its custodian, the account's id, its
own id (the message's, the deleted item's or the archive entry's), its folder,
its `Message-ID`, date, From, To, subject and size, the names of its attachments
(or `null` when its body was not read), whether only its headers could be read
(`headers_only`: a zero-access mailbox, or a message encrypted end to end),
whether it came from a zero-access mailbox (`zero_access`) and whether it is held
now. What the search could not read is paged on its own: `unsearched_list`, from
`unsearched_offset` (default 0), at most `unsearched_limit` (default 100, at most
1,000), of the `unsearched_listed` entries the results file keeps.

### What it cannot see

* **100,000 hits.** A search stops at the 100,000th hit and says `truncated`.
  Narrow it and run it again.
* **80 MB a message.** A message is read whole, up to the 80 MB `MessageData`
  reads. A larger one is listed as not searched, with that reason, when the
  query asks about its body (words, an attachment); otherwise it is a hit by its
  header and date, with its attachments unread. It is **never** dropped as a
  non-match.
* **Text in attachments: `text/*` only, up to 1 MB of characters a message.**
  That is what IMAP SEARCH reads (`MessageData::GetAttachmentText`). The text of
  a PDF, a Word document or a spreadsheet is not read: a word that is only
  inside one is not found. An attachment criterion still finds the PDF by its
  name, type and size.
* **Zero-access mailboxes: headers alone.** A zero-access mailbox's messages are
  encrypted to a key this server does not have ([ZeroAccess.md](ZeroAccess.md)).
  Its messages are matched on their headers - words in the subject, the From,
  To and Cc, the rest of the header - and marked `headers_only` and
  `zero_access`; one whose headers lack a word is not a hit, as decided for such
  a mailbox, and an attachment criterion cannot be judged, so such a message is
  listed as not searched. The archive's copy of one, which the archiver wrote as
  it was received, is plain.
* **Messages encrypted end to end: headers alone.** An S/MIME or PGP/MIME
  message - one whose top-level type is `multipart/encrypted`,
  `application/pkcs7-mime` or `application/pgp-encrypted` and whose body really
  is ciphertext ([SealedMail.md](SealedMail.md): the label alone is not enough,
  so a readable message under one of those labels is searched like any other) -
  has no body this server can read, in any mailbox, the deleted items or the
  archive. It is matched
  on its headers and marked `headers_only`; one whose headers lack a word, or
  asked about by an attachment criterion, is listed as not searched with that
  reason, since its body might hold what was asked. It is not withheld from an
  export: it is exported as it is stored.
* **What could not be read** - a file that is gone, a message that does not
  parse - is listed in `unsearched_list` with the reason, never dropped. The
  results file lists the first **10,000** such entries; the search's
  `unsearched` counts every one.
* **The stores beside the mailbox.** A message sent in confidential mode or
  protected by a password is kept for its recipients under
  `<data directory>\Confidential` and `\Protected`; a search reads neither.
* **Archived copies with no index row.** The archiver files a copy whose envelope
  sender is null or not an address - a bounce, a delivery report - under the
  archive's `Error` folder with no row in `hm_archiveindex`, so a search does not
  find it.

**The full-text index is a filter, never the answer.** It is off by default
(`IndexerFullText`). When it is on, a search rules out a mailbox message the
index proves cannot contain a word - a message indexed completely, without
overflow, holding none of the word's terms - exactly as IMAP SEARCH does; every
other message is read. The index holds what the search reads, so what it rules
out is what the search would not have found. It covers mailbox messages only:
deleted items and archived copies are always read.

**One at a time.** Searches and exports are queued, and one runner works
through the queue on the maintenance work queue, the oldest first: a search
reads every message it is asked about, and the queue's five threads have other
work. A search or an export the server stopped in the middle of - a service
stop, or a reinitialise, which also ends one between two messages - is failed
with the sentence when the runner next starts, and is asked for again. The
runner is this server's: two servers sharing one database would each work
through the one queue, so run discovery on one of them.

## Holds

`POST /api/v1/discovery/cases/{id}/holds` takes one of:

| Body | Holds |
|---|---|
| `{"scope":"account","target":"alice@example.com"}` | Every message of the account, and its archived copies |
| `{"scope":"domain","target":"example.com"}` | Every message of every account in the domain, and their archived copies |
| `{"scope":"archive","search":12}` | The archived copies search 12 of this case matched, at most 10,000 at once |

A hold is a row of `hm_discoveryholds`, live until it is released
(`DELETE .../holds/{hold}`, or the case closed). A case holds an account or a
domain once (`409` for the second); a hold on the archive's copies skips those
the case holds already. What a hold keeps is kept by **any** live hold of any
case: releasing one case's hold leaves another case's standing.

An archived copy is held without touching the archive's own `archivehold`
flag. That flag has no owner - the first case or administrator to clear it would
release everyone's - so the archive asks for a live hold of a case beside it. A
domain hold covers the copies filed under the name the domain had when the hold
was placed and under the name it has now.

### What a hold stops

Every way this server deletes a message that is in a mailbox, or the mailbox, or
drops a message the account's own filters were given, found by reading the code:

| Path | Under a hold |
|---|---|
| **The person's delete**: IMAP `EXPUNGE`, `UID EXPUNGE` and `CLOSE`, POP3 `DELE`, the webmail's delete and its emptying of a folder, a folder deleted over IMAP, over the webmail or over COM, `APPEND` with `REPLACE`, a recall of a message the sender sent | The message leaves the folder, and a copy is kept among the account's deleted items **whatever `RecoverDeletedDays` says** (0 by default). If the copy cannot be made the delete is refused and the message stays (HM7333), and a folder is not deleted over a message left so |
| **An IMAP `MOVE` out of the account**: into a public folder, or into another account's folder shared under `#Users` - by the account's own person, or by a delegate moving from the held account's shared folder into their own | The original is kept among the held account's deleted items, as a delete's is: the copy the move made belongs to somebody else now, who may expunge it |
| **An IMAP `COPY` or `MOVE` that fails part way** - the quota met after its first copies, say - takes the copies it made out of the destination again, and expunges no original | Each copy taken out is kept among the deleted items of the account whose folder it was in, as a delete's is; one whose copy cannot be made stays in the destination (HM7333), and the command is still answered `NO`. The originals stay where they were |
| **An administrator's delete**: `DELETE /api/v1/accounts/{address}/messages/{id}` and the other message deletes of the account's resources, `Messages.DeleteByDBID` over COM | The same: deleted from the folder, kept among the deleted items |
| **Every message at once**: `DELETE /api/v1/accounts/{address}/messages`, `Account.DeleteMessages` | Refused, `409` over REST and an error over COM, with the sentence |
| **The account's own filters at delivery**: an account rule's Delete or Reject action, a Sieve `discard`, `reject` or `redirect` that keeps no local copy, a forward that keeps no copy, and the account's own spam delete threshold when there is no quarantine | The copy is not delivered to the mailbox - the filter's decision stands - and is kept among the deleted items instead of being dropped; the log line says the hold kept it. For a **zero-access mailbox** the kept copy is sealed to the mailbox's key first, as the mailbox would have held it, and is never kept in the clear. When the copy cannot be kept, the message is **delivered** instead - a hold outranks the filter - and the log says so; a Sieve reject is then not sent |
| **Retention**: the account's message retention policy and the person's own Trash, Junk and Archive choices | The account is left alone: nothing of it is deleted or archived while the hold stands, and the log says so |
| **The recover-deleted sweep**: `RecoverableStore::Sweep` | A held account's deleted items are not ended, however old |
| **Deleting a deleted item for good**: `DELETE /api/v1/me/recoverable/{id}` | Refused, `409`; the listing answers `held: true` |
| **A mailbox restore's replace**: `POST /api/v1/backup/{archive}/mailboxes/{address}/restore` with `mode` replace, `BackupManager.RestoreMailbox` | Refused: over REST `409` with the sentence, before the archive is opened; over COM, and for a hold placed after the request was taken, when the job runs, which fails with the sentence and changes nothing. Merge and one message delete nothing and go ahead |
| **A full restore with the domains option**: `POST /api/v1/backup/restore`, `Backup.StartRestore` over COM, which deletes every domain before it puts the archive's back - with `BackupMessagesDBOnly` too | Refused whole while any account or domain is held, before anything is changed, naming what is held (HM5014, *Nothing has been changed on this server*). A hold on archived copies alone does not refuse it: the archive is not in the data directory the restore replaces - unless `ArchiveDir` was put inside it, when a restore with the messages option replaces the archive too, held copies included, so keep the archive outside the data directory |
| **Moving the account to another domain**: `POST /api/v1/accounts/{address}/move`, `Account.Move` and `Utilities.MoveMailbox` over COM, SCIM's rename, `Account.Address` saved | Refused while the account is held, on itself or through its domain: `409` over REST and SCIM, an error over COM, with the sentence. A domain hold is kept by the domain, so a move out of it would take the account out of the hold; a rename inside the domain goes ahead, and so does a move of an account under no hold into a held domain, which the hold then covers |
| **Removing a mailbox key**: `DELETE /api/v1/accounts/{address}/mailbox-encryption?confirm=forfeit` ([EncryptionAtRest.md](EncryptionAtRest.md)) | Refused, `409` with the sentence: the key is the only way to the mail encrypted to it, and removing it would be deleting that mail. A custodian whose key row holds no key is refused the same, and every message to it is bounced at once and none kept until the row is put back from a backup or the hold is released |
| **The account's deletion**: `DELETE /api/v1/accounts/{address}`, SCIM's `DELETE /scim/v2/Users/{id}`, `Account.Delete`, `Accounts.Delete` and `DeleteByDBID` | Refused, `409` (SCIM: in its error shape) or an error over COM, with the sentence; `PersistentAccount::DeleteObject` refuses whatever path reaches it. SCIM's removal of a held account also **suspends** it and ends its sessions and connections, since the person the identity provider removed has left: the account and its mail stay, and the identity provider hears `409` until the hold is released. The one deletion let by is the undoing of an account the same request made a moment before - an inbox that could not be made, a SCIM create whose alias was refused - which nothing was ever in |
| **The domain's deletion**: `DELETE /api/v1/domains/{domain}`, `Domain.Delete`, `Domains.DeleteByDBID` | Refused while the domain or any account in it is held, before any account of it is deleted |
| **The archive's retention sweep** (`ArchiveRetentionDays`) and **an address's erasure** (`POST /api/v1/addresses/erase`) | A held copy - by a hold on it, on its account or on its domain - keeps its file and its index row. A file the index has no row for is judged by the directories it lies in. The erasure also leaves a held account's messages held for review, messages that expire and password-protected messages as they are, and its answer counts what a hold kept (`held`, [PersonalData.md](PersonalData.md)) |

A hold does not stop what is not a deletion: a message moved to another folder
**of the same account** - over IMAP `MOVE`, the webmail's move, or to Trash by
the webmail's delete, none of which may leave the account - is still in the
mailbox, and the hold keeps no second copy of it. A scheduled draft sent is
kept, as a delete of a held draft is.

What a hold does **not** reach:

* **The server's and a domain's rules.** They decide about a message before it is
  split between its recipients; a Delete there drops the message for everybody,
  held accounts included.
* **Spam and viruses before a message is the account's.** A message the virus
  scanner deletes, and one refused or deleted at the conversation by the spam
  filter, never reaches the account's copy. The quarantine keeps what it holds in
  its own store - the account's spam delete threshold and block list send the
  account's copy there when it is on - and what the quarantine discards is
  deleted from there.
* **The queue.** A message still in the delivery queue is not in a mailbox yet.
  An address's erasure is the one exception: it leaves a held account's
  messages held for review where they are, as it leaves its archived copies.
* **The stores beside the mailbox.** A message the custodian sent in confidential
  mode or with a password is kept for its recipients under
  `<data directory>\Confidential` and `\Protected`, and the sender's revoke and
  those stores' own expiry delete it whatever a hold says. Neither is searched.
  An address's erasure does not delete it: a held account's kept copies stay.
* **Archived copies with no index row** in the archive's `Error` folder - a bounce
  or a report with a null sender: the archive's sweep judges them by the directory
  they lie in, which names no account, so a hold on an account or a domain does
  not keep them.
* **The full-text index and the message trace**, which are not mail.
* **A second server** reading the same database learns of a hold within five
  seconds.

**The hold is not hidden from the custodian.** The account's own
`GET /api/v1/me/recoverable` answers `held: true` and lists what the hold kept,
and a delete of one of those for good answers `409` naming the legal hold; the
webmail's *Recover deleted items* page then lists them, and offers no delete for
good. Vault and Purview keep a hold silent; a silent hold here would need the
items the hold alone keeps hidden from the person, which this server does not do.

The questions are answered from the live holds read into memory, read again at
once when a hold is placed or released and at most five seconds after a change
made elsewhere. A read that fails is answered **held for everything** and said
in the error log once a minute (HM7332): a hold that could not be read is not a
hold that was released. The same goes for an account whose domain cannot be read
while a domain hold stands, and the retention sweep says in one line that the
holds could not be read rather than naming a hold on every account. An address's
erasure that meets holds it could not read fails, naming them as a store to retry,
rather than count what it left as kept by a hold ([PersonalData.md](PersonalData.md)).

## Exports

`POST .../searches/{search}/export` queues an export of a finished search's
hits; `GET /api/v1/discovery/cases/{id}` shows its state, and once it is `done`,
`GET .../exports/{export}` downloads the zip. The zip is
`Discovery\case-<id>\export-<id>.zip`:

```
alice@example.com/INBOX/1-CAB12345@mail.example.com.eml
alice@example.com/INBOX/7-<message id>.eml
alice@example.com/INBOX.mbox
alice@example.com/Recoverable Items/INBOX/3-<message id>.eml
alice@example.com/Recoverable Items/INBOX.mbox
alice@example.com/Archive/Received/9-<message id>.eml
alice@example.com/Archive/Received.mbox
SHA256SUMS
manifest.json
```

* **Each hit as EML**, the message's own bytes as the server stores them, byte
  for byte, under `<custodian>/<folder>/<n>-<message id>.eml`: `n` is the hit's
  number in the search, and the `Message-ID` is written with everything but
  letters, digits and `.-_@+=` as `_`. An Inbound archive copy is under
  `inbound/`.
* **An mbox per custodian folder** beside them, for a tool that wants one: a
  From_ line of the sender's address and the day, the message, a body line that
  begins `From ` quoted - `PersonExport`'s form. One per folder, not one per
  custodian, because the zip writer takes an entry whole; and a folder's mbox is
  written in parts of at most 256 MB - `INBOX.mbox`, then `INBOX-part2.mbox` and
  on - so that a large folder is not held in the server's memory at once.
* **SHA256SUMS**: the `sha256sum` line of every file written, so that
  `sha256sum -c SHA256SUMS` checks the unpacked export.
* **manifest.json**: the format, the case (id, name, description), the search
  (id, the query as it was run, who ran it, when it was asked for and when it
  finished, its counts and whether it was truncated), the export (id, who asked,
  when), the number of files and of items withheld, what the search could not
  read (`unsearched_list`, with each reason, up to the 10,000 the results file
  keeps), and every item - its path, SHA-256 and size, kind, custodian, folder,
  id, `Message-ID`, date, From, To, subject and whether it is **held** now - or,
  for an item not written, `withheld: true` and the reason. `sums_sha256` is the
  SHA-256 of `SHA256SUMS`: the manifest's own digest over the lines that name
  every file.
* **The zip's own SHA-256** is kept with the export, and the case answers it
  (`exports[].sha256`), so the download can be checked against what the server
  wrote.

**Withheld, with the reason:** a zero-access mailbox's messages - its
administrative export is refused by design, so its hits are listed and not
written (exporting the ciphertext is not done either) - and so are those of a
mailbox whose owner switched zero access on after the search ran, since from
then its administrative export is refused; a message deleted or moved since the
search ran; a deleted item put back or ended since; an archived copy swept or
erased since. Search again to find where a message is now.

**4 GB.** The zip writer writes the 32-bit zip format, which cannot be larger
than 4 GB. An export whose messages would pass that, written twice over as EML
and in an mbox, fails before a byte is written, with the sentence: narrow the
search and export each part. ZIP64 sizes and offsets and split archives are not
written. The count of entries is: an export of more than 65,534 entries - one
.eml a message, the mbox parts, SHA256SUMS and the manifest - ends with the ZIP64
end record and its locator, which count them in 64 bits, where the classic end
record alone would have counted them modulo 65,536 and a reader that trusts it,
.NET's among them, would have refused the archive or listed part of it. Every
unpacker of the last twenty years reads that record. An export that would leave
the data directory's volume - where the mail is delivered - with less than a
gigabyte free fails the same way as one past 4 GB.

**The download** is sent from the case's directory a piece at a time, as the
person export's is, never read into memory whole; it counts against the
listener's cap on large downloads at once (sixteen; one more answers `503` with
`Retry-After: 30`). An export past 500 MB answers `413` with where it is on the
server's disk and its SHA-256, for an administrator to take from there. It is sent as
`application/zip` with `Content-Disposition: attachment`, `nosniff`, `sandbox`
and `no-store`, and it is on the audit trail as a **discovery export
downloaded**, with the export's id, the search's, the size and the SHA-256.

### Why EML and mbox, and not PST

PST is Outlook's own file format (MS-PST): a database of pages, blocks and
B-trees, published but large, and writing it correctly is a project of its own.
This server reads PST, to bring a file's mail, contacts and calendars into an account
(a migration of kind `pst`, [Migration.md](Migration.md), *Outlook data files*), but
it does not write it, and a discovery export is never a PST. An EML file is
the message as it travelled, which every review tool reads, and an mbox is the
same messages in one file. An examiner who needs PST converts the EML files with
a third-party EML-to-PST converter; the manifest's SHA-256s let them show that
what they converted is what this server exported.

## Who

Two permissions ([Roles.md](Roles.md)), appended to the vocabulary:

* **`view_discovery`** reads the cases, their searches and hits, their holds and
  their exports. The **auditor** holds it: an auditor reads everything and
  changes nothing.
* **`manage_discovery`** opens and closes a case, runs a search, places and
  releases a hold, and makes an export. The auditor does not hold it.

The **server administrator** holds both, as it holds everything; the auditor
holds `view_discovery` alone; no other built-in role holds either, and a custom
role may be given them.

**Downloading an export needs `view_mailbox` as well**: it hands over the mail
itself, byte for byte, and reading the cases is not taking the mail away. The
auditor holds `view_mailbox`. What the two grant together is more than
`view_mailbox` grants alone: an export carries the custodians' **deleted
items** - what the person deleted under a hold, and what a hold kept from a rule,
a Sieve script or a forward that dropped it before it reached a folder - which
no other administrative route hands over. `build/check-role-choke-point.py` pins
that second permission.

**A search that reads the archive needs `view_archive` as well** - to run it,
to read its hits and to download its export - since its hits are archived
copies, which a role without `view_archive` reads nowhere else. The auditor and
the server administrator hold it; a custom role without it searches with
`scope` mailboxes.

**A credential confined to named domains** is confined here too, since no path
names a domain: it names no custodian outside its domains - and never `*`,
which is every mailbox - holds no account or domain outside them, and a case
with a search or a hold that reaches outside them is the same `404` to it as a
case that is not there. A case with neither a search nor a hold - just opened -
it sees only when it opened the case itself, since a case's name and
description are not every tenant's to read, nor its close every tenant's to
make. A hold on an archived copy that has since gone is judged by the address
the hold names.

An API key with a role that holds them uses the routes as an account does.

## The audit trail

"Every search logged" is the audit trail ([AlertsAndAuditTrail.md](AlertsAndAuditTrail.md)),
while it is on - `audit_trail_enabled`, on by default; switching it off is
itself recorded. The four tables are audited, so a request's write is recorded
under whoever asked, each row named by its case (`case 3: Smith v. Jones`):

| Object | Action | When |
|---|---|---|
| `discovery case` | `created`, `updated` | A case opened (named by its name, as the trail keeps it: 120 characters and `...`) and closed |
| `discovery search` | `created` | A search run - **with its whole query** and who ran it |
| `discovery search` | `read` | A page of a search's hits, or of what it could not search, read - by whoever read it, with how many hits it has; the same search read again by the same credential from the same address within a minute, whatever the page, is the row already written |
| `discovery hold` | `created`, `updated` | A hold placed (its scope, what it holds, by whom) and released |
| `discovery export` | `created` | An export asked for |
| `discovery export` | `downloaded` | An export downloaded, with its SHA-256 |

The case keeps its own record as well, whatever the switch says: every search's
query, who ran it and when; every hold, who placed and who released it and
when; every export, who asked for it and when. A download is on the audit trail
alone, so with the trail off a download is recorded nowhere.

The runner's own writes - a search's progress and result, an export's - carry
no actor and are not recorded, as a restore's are not; the request that started
them is. A search's hits are the custodians' mail as the search found it - who
wrote to whom, when, about what, and what was attached - and whoever holds
`view_discovery` reads them, who need not be whoever ran the search: from 6.3.6
a page of them read is on the trail, as the other reads of somebody's mail
through an administrator's route are (*Mail read through an administrator's
route* in [AlertsAndAuditTrail.md](AlertsAndAuditTrail.md)), and like a
download it is on the trail alone. A search still running answers no hit and
records nothing. Reading a case - its searches' queries, counts and states,
its holds and its exports - is not recorded: none of it is the mail.

## Where it is used

* **The Control Deck**'s **Discovery** view: the cases, a case's searches with
  their progress and hit counts, the search form, the holds - their scope in the
  reader's language - with **Lift the hold**, the exports with Download, and
  Open and Close. The form asks for custodians, words, dates, an attachment's
  name and type and the scope; an attachment's least size (`min_kb`) is asked
  over REST.
* **The Control Panel**'s **eDiscovery** page reads the cases and their lines on
  the audit trail over the REST listener - `GET /api/v1/discovery/cases` and
  `GET /api/v1/audit` - with the name and password it signed in with, and
  changes nothing; the work is the Deck's. It needs the listener on
  (`RestApiPort`), and finds it as the server starts it: with no certificate of
  its own the listener takes the ACME certificate when one has been issued, which
  the page looks for on the server's disk, and from another machine, where it
  cannot look, it asks HTTPS of a listener bound beyond loopback. It says so where
  the table would be when the listener is off or cannot be running, when it is
  bound to a loopback address of another machine, when its certificate does not
  name the address asked (the password is not sent past a certificate that cannot
  be checked - the ACME certificate on a loopback bind is one), and when the
  administrator has a second factor, whose one-time code the REST API asks for
  with every request made with the password. A case's lines need `view_audit` as
  well as `view_discovery`; without it the cases are listed and the page says why
  the lines are not.
* **No COM** and **no `hmctl`**: neither has an archive group to add this
  beside. The REST routes are in the OpenAPI document.

## What it does not do

* **No PST export** (above).
* **No text from attachments but `text/*`**, and at most 1 MB of it a message;
  nothing past 80 MB a message is read.
* **No sender or recipient criterion**: words over the whole header, and people
  as custodians.
* **No export past 4 GB**, and no download past 500 MB (the file is on the
  server's disk); nothing removes an export.
* **One search or export at a time**, on one server.
* **Zero-access mailboxes, and messages encrypted end to end, on their headers**,
  and a zero-access mailbox's messages not exported.
* **100,000 hits** a search, 10,000 unsearched entries listed, and at most 10,000
  archived copies held by one request.
* **No hold on the server's or a domain's rules**, on spam and virus deletion
  before a message is the account's, on what the quarantine discards, on the
  queue, on the confidential and password-protected stores (but for an
  address's erasure, which leaves a held account's), on archived copies
  with no index row, or - when `ArchiveDir` is inside the data directory - on
  held archived copies against a full restore.
* **No silent hold**: the custodian can see what a hold kept (above).
* **No search of the mail a hold kept in another way**: a search reads the
  deleted items, and the archive; it does not read backups, and a backup does not
  carry the cases.
* **No saved query to run again**, no review tags, no redaction and no
  de-duplication across custodians: a message sent to two custodians is two
  hits, as it is two copies.

## Proving it works

`RegressionTests.Infrastructure.Discovery` (REST port 10060): a search by
custodian, date and words over two accounts finds exactly the messages made to
match - a word only in the body of a message over 1 MB among them - and not one
dated before the range, one without the word nor one of an account that is not
a custodian; the archived copy is a hit with `scope` both and not with
mailboxes; IMAP `SEARCH TEXT` finds the same two of a mailbox's three messages by
the same word, compared by their `Message-ID`s; the audit trail carries the
query; an archive hold holds the copy without its flag - the custodian's address
erased with the archive leaves the held copy's row and file and takes the unheld
copy's, and a search run again says it is held. An attachment criterion matches
only the message carrying it. A hold keeps what an IMAP `EXPUNGE` deleted with
`RecoverDeletedDays` at 0, refuses deleting it for good, survives the recoverable
sweep, keeps the account's old mail from its own retention policy where the
control account's goes, keeps a rule's Delete where the control account keeps
nothing, refuses a restore's replace and the account's deletion over REST and
COM, is found by a search, and its placing and release are on the audit trail;
released, the deletion proceeds. An IMAP `MOVE` of a held account's message into
a public folder keeps its original, where a move within the account and the same
move out of an unheld account keep nothing. A `MOVE` and a `COPY` that fail part
way over a held account's quota each keep the copy they took out again, with the
originals where they were. `API/RestApiErasureStores.cs`: an
address's erasure leaves a held account's message held for review, message that
expires and password-protected message, counts them in `held`, and ends them once
the case is closed. A domain hold keeps an expunge in
the domain where another domain's account keeps nothing, refuses the domain's
deletion over REST and COM and a move of its account to another domain, and lets
an unheld account move in and holds it there. A Sieve discard under a hold is
kept where the control's is not. A zero-access mailbox under a hold keeps a
rule's Delete sealed - PGP/MIME, no plaintext body - where an ordinary one's is
kept as it came; its search hits are header-only, a word only in its bodies is
not found where the ordinary mailbox's is, and its export withholds its messages
with the reason. A full restore with the domains option under a hold is refused
before a domain that sorts first is deleted. An export downloads with its EML
files byte for byte, its manifest's and `SHA256SUMS`'s digests verifying, an mbox
that parses, and the SHA-256 the case answers; asking for it and downloading it
are on the audit trail. A key with neither permission is refused, an auditor
reads and is refused every write it tries - a search, a hold, a release, an
export, a close, a case - a role without `view_mailbox` is refused the download,
a role without `view_archive` is refused a search of the archive and the hits of
one while it searches the mailboxes, and a key confined to a domain names no
custodian outside it, is listed the empty case it opened and not one somebody
else opened, and is answered `404` for a case that searched outside it.
`API/Roles.cs` counts the permissions (twenty-six when this row landed; the inbound
webhooks' `manage_inbound_webhooks` made twenty-seven). The schema probes for 6097 succeed,
and the negative control fails as a missing column.

Not proved by a test, and read in the code instead: a copy that cannot be kept
at delivery leading to delivery; the account's spam delete threshold, a Sieve
`reject` or `redirect` and a forward under a hold; the `BackupMessagesDBOnly`
restore; SCIM's removal and rename of a held account and the undoing of an
account a request made; COM's `Domains.DeleteByDBID`, `Accounts.Delete`,
`DeleteByDBID`, `Account.DeleteMessages` and `Account.Move`; POP3 `DELE`, the
webmail's deletes, a folder's deletion, `APPEND` with `REPLACE` and a recall
under a hold; `IsHeld` failing closed (HM7332, HM7333); a message encrypted end to
end in an ordinary mailbox; zero access switched on between a search and its
export; the 4 GB and free-space refusals, the 413, the mbox parts and the
unsearched paging; the refusals past 100 custodians, 50 words and 1,800
characters; `attachment.min_kb`; a domain renamed under a hold; and the runner
taken up again after a reinitialise.

The Control Panel page's reading is pinned without a server by
`DiscoveryReportTests` in `ControlPanel.Tests`: the listing's shape as
`RestApiDiscovery` writes it, which rows of the audit trail are a case's (its
opening row named by the case's name as the trail cut it, the rest by
`case N:`, and never another case's of the same name), and where the listener
is by the rules it starts by, the ACME certificate's among them. The Deck's view
is driven by `build/deck-script-test.js`: the listing, opening a case, a
search's body, holds on an address and a domain with their scope in the reader's
words, a release, an archive hold, closing with its question, an export, and the
download link; an auditor is offered no write, and a domain administrator is
not shown the view.
