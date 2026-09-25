The message store: compression and parts held once
===================================================

A message this server keeps in a mailbox is a file under the data directory. Until
24 September 2026 that file was always the message exactly as it arrived, RFC 822
byte for byte. With **MessageStoreCompression** on, a message delivered to a mailbox,
or put there by IMAP APPEND, is stored instead as a *container*: the message
compressed with deflate, and each large MIME part - an attachment of at least
**MessageStoreDedupMinKB** once decoded, 256 KB unless changed - cut out and held once
as a *blob* that every message carrying the same bytes shares. A mail sent to forty
people on this server with a 10 MB presentation attached keeps the presentation once.

It is off by default. A server upgraded to schema 6098 stores every message as it
always has until somebody turns it on, and reads a store of either form whatever the
setting says - per file, by the file's first eight bytes.

Only the server's own writer makes a container. Bytes from outside - what a client
appends, what a remote server sends to a mirrored account, what a migration or an
importer brings in - are the message, whatever they begin with: a message that begins
with the magic is *wrapped* (a container with no stage, which reads back as exactly
those bytes) rather than taken for one, and one that cannot be wrapped is refused, not
kept. So a container the store meets is always one it wrote, copied or linked, and the
blobs a container names are always ones the store counted. A queue file is never a
container at all - a file directly in the data directory is read as the bytes it
holds, whatever they are.

**Measured on a real mailbox: to come.** The integrator runs `--measure-store` over a
real mailbox before any saving is claimed, and the figures replace this sentence.


What a client sees: nothing different
-------------------------------------

Everything that hands a message out reads the original bytes back through one door
(below), so a compressed message is the same message to every reader:

* **IMAP**: `FETCH BODY[]`, `RFC822`, any section or partial range, `RFC822.SIZE`,
  `BODYSTRUCTURE`, `SEARCH` on the body or the text, `SORT` and `THREAD` on the
  headers. The size is the original's, in `hm_messages.messagesize` as it always was.
* **POP3**: `RETR`, `TOP`, `LIST` and `STAT`.
* **The webmail and the REST API**: the message, its source, its attachments, the
  account export and the person's data export.
* **The full-text index**, the rules, Sieve, the quota (which counts the original
  sizes), a DKIM signature checked against the stored copy (it verifies, because the
  bytes are the bytes that were signed), the recover-deleted store and the mailbox
  restore.
* **SpamAssassin learning**, which needs a file by path, is handed a plain temporary
  copy that is deleted afterwards.

What does change is the file itself:

* **`Message.Filename` over COM still answers the stored file's path.** With
  compression on, a COM client or a script that opens that path and reads it reads a
  container, not RFC 822. Read the message through the API instead, or turn
  compression off and run `hmailserver --compress-store --decompress` (below) if
  something outside the server must read the files. `Message.Save` still works: it
  writes the message back plain, and lets go of what the container held once.
* **The queue is never compressed.** A queue file, and a copy of a message while it
  is being delivered, is plain RFC 822 by construction - the store only turns a
  working file into a stored one after the rules, Sieve and the filters have had it.
  So the event scripts (`OnAcceptMessage`, `OnDeliveryStart`, `OnDeliverMessage` and
  the rest), the virus scanners, SpamAssassin, the sandbox, gateway encryption and
  the filter hook all read what they always read.
* **A backup of the data directory** carries the containers and the `Blobs`
  directory beside them; see *Backups*, below.


Turning it on, and off
----------------------

| Where | What |
|---|---|
| The Control Panel | *Settings > Maintenance > Performance*, the **Message store** tab: the switch, the smallest part held once, and what the store holds and saves as the server last measured it |
| The Control Deck | The two settings on the server settings page, and a **Message store** section on the dashboard: the messages, what their files take, what the parts held once take, and the saving |
| REST | `message_store_compression` and `message_store_dedup_min_kb` in `/api/v1/settings`; `GET /api/v1/store` for the measurement (permission *view configuration*), `POST /api/v1/store/measure` to take one now (permission *manage server*) |
| COM | `Settings.MessageStoreCompression`, `Settings.MessageStoreDedupMinKB`, and `Settings.MessageStoreStatus` (the same JSON as `GET /api/v1/store`) |

Both settings are rows of `hm_settings`, not `hMailServer.ini` keys, and take effect
for the next message stored. `MessageStoreDedupMinKB` is 0 to 65536; 0 holds nothing
once and compresses only.

**Turning it off** stores new mail as it comes and leaves every stored message
readable - the reader never asks the setting what a file is. The files already
compressed stay compressed until `hmailserver --compress-store --decompress` puts
them back, byte for byte.

### What `GET /api/v1/store` answers

```json
{"measured": 1790000000, "measuring": false, "messages": 120,
 "logical_bytes": 52428800, "stored_bytes": 20971520, "compressed_files": 100,
 "blobs": 3, "blob_bytes": 5242880, "blob_references": 9,
 "saved_bytes": 26214400, "saved_percent": 50.0,
 "compression": true, "dedup_min_kb": 256}
```

`logical_bytes` is what IMAP and POP3 say the messages are; `stored_bytes` is what
their files take on disk, a file with several names (`DeliveryHardLinks`) counted
once; `blob_bytes` is what the parts held once take. `messages` and `logical_bytes`
count only the messages whose files could be sized: a row whose file is missing or
was expunged mid-walk is on neither side of the saving, so a missing file never reads
as a saving (the consistency check is what reports it). The saving is
`logical_bytes - stored_bytes - blob_bytes`.

The server measures on its own work queue, never on the request. The answer is always
the last measurement; a `GET` starts the next one when the last is over a quarter of
an hour old **and the store is in use** - compression on, a part held once, or a
container found by the last measurement - so on a server that never turned compression
on nothing is walked and the answer stays `"measured": 0, "measuring": true` until a
measurement is asked for outright. `POST /api/v1/store/measure` asks for one now, and
answers `202` with the last totals and `"measuring": true`; the walk reads the start of
every stored file, so it needs *manage server*, and the Deck's dashboard reads
`GET` (which walks nothing on the default) rather than posting.


The store's walk, from the command line
---------------------------------------

```
hmailserver --measure-store [--account <address> | --domain <name>]
hmailserver --compress-store [--account <address> | --domain <name>] [--decompress]
hmailserver --recount-store
```

Run as the user the server runs as, with the same `hMailServer.ini`. On Windows
`hMailServer.exe` takes the same switches. `--account` covers one mailbox, `--domain`
one domain's mailboxes, neither the whole store; the account and the domain are found
the way a sign-in finds them, without regard to case on every database.

* **`--measure-store`** changes nothing. It reads every stored file, or one account's,
  or one domain's, compresses each in memory, finds its large parts and counts a part
  met twice once, and prints what the store takes now, what it would take, and how much
  more it would save. A store already compressed shows a further saving of nothing, not
  a loss: a container's parts are counted on both sides. It is the one verb that runs
  beside a running server.
* **`--compress-store`** rewrites every stored file in place, compressed with its large
  parts held once; with `--decompress`, back to plain RFC 822. Each file is written
  beside itself, read back and compared with the message byte for byte, and only then
  renamed over its name, so an interrupted run leaves every file whole in one form or
  the other. The message's row is not touched: the name does not change, and neither
  does the size. A file with several names (`DeliveryHardLinks`) is rewritten once and
  every name linked to the new file; when some of its names are outside the walk
  (another account's, under `--account`) it is left as it is and counted, never split
  into a copy per name. `--compress-store` leaves a zero-access mailbox's files as they
  are - ciphertext does not compress - but `--decompress` walks them like any other and
  puts back any container among them (an `APPEND` to such a mailbox is stored as the
  client sent it, so a container can be there). A message that compression would not
  make smaller and that has no large part stays as it is.
* **`--recount-store`** sets every blob's count to what the stored files reference,
  lowering it where it is too high and removing a row nothing references. It is the one
  recount that lowers a count, which is why it runs only with the server stopped; the
  recount a restore ends with only raises. It is also how a store upgraded from before
  this row has every file that begins with the magic counted.

The rewrite and the recount are refused while the server runs (exit 4): stop it first.
On Windows the service controller is asked; on Linux the server holds a lock in the log
directory for as long as it runs, and a walk is refused while it does (and the server
will not start while a walk holds it). The exit code is 0 when the walk finished, 1
when a file or the store could not be read or written (each one is named, and left as
it was), 2 when there is no database configured or no such account or domain, and 4
when the server is running.


The seam
--------

Every place that reads, sizes, copies, moves or deletes a stored message goes through
`MessageStore` (`Common/Persistence/MessageStore.h`). Before this row the stored
file's path was asked for at about 180 places in eighty files, and each read it its
own way; a file that is not the message's bytes would have been read as garbage by
every one of them. The callers are of three kinds:

* **Working files** - the queue file, the copy being delivered, the file written for a
  row that does not exist yet - are plain by construction and are read and written by
  path, as before, through `MessageStore::WorkingFile`.
* **Stored files** are read through the store's doors: its reader (IMAP `FETCH` and
  POP3 read through it), `ReadAll`, `ReadRange`, `LoadMime`, `FileSize`, and the two
  loaders that go through them, `PersistentMessage::LoadHeader`/`LoadBody` and
  `MessageData::LoadFromMessage` - which parses a container from memory, never from a
  plain copy on the disk. They are copied (`CopyStored`, `CopyOriginal`),
  renamed, linked and removed through the store, which is what keeps the counts of
  the blobs right.
* **Listed** sites must have the stored file's name itself - `Message.Filename`, the
  indexers that carry the name to the loaders, the recover-deleted store. Each is
  named, with its reason, in `build/check-message-store-seam.py`, which refuses a call
  anywhere else, a second call in a listed file, and a message's size taken from its
  file. `MessageStore::WorkingFile(account, message)`, the working-file path that names
  an account's folder - where a stored message will be - is held to the same rule and
  listed the same way; the object-storage and encryption-at-rest rows audit that list,
  since each such path is a place a key-based backend hands a temporary file to instead.
  The checker runs in GitLab CI's checkers job on every push and merge request, and in
  `MessageStoreFormat.TheSeamCheckerPasses`, which runs it in the suite.

The store is a **chain of stages over a backend**. The backend, `MessageStoreBackend`,
is where the bytes live, and the local file system is the one there is: exists, size,
open, read, write atomically (beside the name, then renamed over it), copy, move,
link, remove, and which object a name is. The stages are what is done to a
message on the way to the backend and undone on the way back, and the container
names them, in order, by id.


The container, byte by byte
---------------------------

Every number is little-endian.

| Offset | Length | What |
|---|---|---|
| 0 | 8 | The magic, `89 48 4D 53 0D 0A 1A 0A` (`\x89HMS\r\n\x1a\n`). The first byte is above 0x7F, which no RFC 5322 header field begins with, so no message is taken for a container. A message that does begin with the magic is stored as a container with no stages, so the test stays exact |
| 8 | 1 | The version, 1. A reader refuses a later one |
| 9 | 1 | Flags: bit 0 set when references follow |
| 10 | 2 | Reserved, 0 |
| 12 | 4 | The header's length: where the body begins |
| 16 | 8 | The original message's size |
| 24 | 32 | The SHA-256 of the original message |
| 56 | 1 | The number of stages; then for each its id (1 byte), the length of its parameters (1 byte) and the parameters |
| . | 4 | The number of references; then for each the offset of the part in the original (8), its length (8) and the SHA-256 of its bytes (32), which is the blob's name. In offset order, never overlapping |
| . | . | Zeros to the header's length |

The body is the original with the referenced ranges cut out, put through each stage
in the order listed. The stages:

| Id | Stage | Parameters |
|---|---|---|
| 1 | deflate, zlib-wrapped so the Adler-32 is checked (the vendored zlib 1.3.1) | the level, one byte (6) |
| 2 | reserved for zstd | |
| 3 | encryption to the account's key, *Mailbox encryption at rest* ([EncryptionAtRest.md](EncryptionAtRest.md)), always the last stage, run after compression. A container with it names no reference, and its SHA-256 field is 32 zeros | 93 bytes: the version (1), the account's X25519 public key, the ephemeral public key made for the file, the AES-256-GCM nonce and tag |

The reader checks the magic, the version, that the header's lengths agree with each
other and with the file, every stage id, that the references lie in order inside the
original, that each blob is there with the length and the hash its reference names,
that the stages give back exactly the size the header says, and last the SHA-256 of
the whole. A container that fails any of it is refused - the read fails, as it would
for a missing file - and the error log says which file and why (`HM7350`). It is
never handed out as something else, in whole or in part: a body cut short, or one that
gives other bytes than its length, is caught before a byte is streamed.

A read of the header alone - `FETCH ENVELOPE`, `SORT`, `THREAD`, `SEARCH` on the
headers, the rules, the index, a push notification's subject - checks everything but
the blobs and the SHA-256 of the whole, which need every part: the deflate stream is
read to its end and its Adler-32, so damage anywhere in what the container holds
itself is refused there too, and a damaged message's header is not served while its
body is refused.

A container is decoded whole in memory, so a message larger than 64 MB is stored as
it came. A message read in pieces - `BODY[]<start.count>`, which some clients use to
download in chunks, or its structure and then its parts - is decoded once and the
decoded bytes are kept for a short while, keyed by the file and its size, so a chunked
read costs one decode and not one a chunk; a rewrite is a new file, never served from
what was kept.


Parts held once
---------------

A part is held once when it is a leaf of the message's MIME tree whose body is at
least `MessageStoreDedupMinKB` once decoded (base64 counted as what it decodes to,
anything else as it is). What is kept is the part's body **as it was on the wire**,
so that putting it back gives the message byte for byte, at
`<data directory>\Blobs\<first two hex digits>\<sha256>`, with a row of
`hm_storeblobs` (schema 6098): its SHA-256, its size, how many stored files name it,
and when it was first kept.

Two messages share a blob when the part's encoded bytes are identical: the same
message to several people on this server, a forward or a reply that carries the
attachment unchanged, a copy into another folder. The same file attached twice by a
client that wraps its base64 differently is two blobs.

**The count is of names, and is never fewer than there are.** A container placed in
a folder adds one to each blob it references; so does every other name for it - an
IMAP `COPY`, the recover-deleted store's kept copy, a mailbox restore, a hard link
`DeliveryHardLinks` makes. A count is added before the name that needs it is written,
and let go of after the name is gone; a working file that was never placed, and so
counted nothing, is discarded without letting go of anything. At none the row and then
the blob go, the file only once the row is gone, so a count raised between the two
keeps the part. A count too high leaves a blob nothing needs, which the consistency
check reports and `--recount-store` corrects. A count too low would delete a part a
message still needs, which is the one mistake the order above is there to rule out.

When a container is placed and a part it names is already on disk, what is there is
checked against the part's own hash before it is counted, and written again from the
message's own bytes when it does not match - so a blob damaged on disk, or one left by
an interrupted run, does not take down every message that shares the part.

**A restore holds the counts.** A full restore deletes every domain, and a
whole-mailbox *replace* empties the mailbox's live folders, before the archive's
messages are put back - either of which could take a part's count to none while the
archived copies of the same messages are about to need it. So a hold is kept from
before the deletion until the messages are back: while it is alive a count may reach
none but the blob and its row are kept, each restored copy counts the part again, and
when the hold ends a full restore's counts are rebuilt from the stored files (raised,
never lowered - a name counted while the walk read keeps its count) before any blob
whose count is still none is deleted.

The consistency check (`MessageStoreConsistencyCheck`) counts a stored message whose
blob is missing as a message whose file is missing, and lists every blob nothing
references at the end of its report. It deletes nothing.


Backups
-------

* **A backup with messages** copies the whole data directory, so it carries the
  containers and the `Blobs` directory. It copies file by file while the server runs,
  so an archive taken while mail is delivered and deleted can hold a container whose
  blob was written after the `Blobs` directory was copied, or deleted before it was -
  the same kind of skew a running backup has always had between a message's row and
  its file, and one more reason to take the backup when the server is quiet.
  `BackupMessagesDBOnly` archives carry no files, and no blobs either.
* **A full restore** deletes every domain, which would let every count reach none.
  While it runs no blob is deleted; when it ends, with the hold still in force, every
  count is rebuilt from the stored files that are there, and only then is a blob whose
  count is still none removed. `hm_storeblobs` is not in the archive: it is rebuilt.
* **A mailbox restore** stages, with the messages it puts back, the blobs their
  containers reference that this server does not have, checks each against its hash,
  and keeps it. A whole-mailbox *replace* holds the counts across emptying the live
  folders and refilling them, so a part only the emptied copies named is not lost
  before the archived copies that need it are put back.
* **A restore onto a server that never turned compression on** reads the containers
  the archive brought: the reader never asks the setting.


What is thinner than it could be
--------------------------------

* **Only delivery and IMAP APPEND write the new form.** Everything else that puts a
  message in a mailbox writes it plain, as before: the webmail's drafts and its copy of
  what a person sends, a message saved through COM, the importers, the IMAP mirror, a
  scheduled send's draft. They are read exactly as well, and `--compress-store`
  rewrites them. A copy made through the store (IMAP `COPY`, a restore) keeps the form
  its source had.
* **zstd is not here.** Deflate on the zlib the tree already vendors is the one stage;
  zstd would compress mail somewhat better and faster, and its id is reserved so that
  adding it changes no stored file.
* **A container is decoded whole** to be read, and the largest is 64 MB, so a very
  large message is stored as it came. A message read in pieces is decoded once and kept
  for a short while, so a chunked read is not a decode a chunk; the header-only readers
  read the header's worth and check the rest of what the container holds without
  reading the blobs.
* **Not yet run against a bench**, like every row built in parallel on 24 September
  2026: the fixture `RegressionTests/Infrastructure/MessageStoreFormat.cs` runs when the
  branch is folded into a batch and gated.


What the next rows build on
---------------------------

**Mailbox encryption at rest** is stage 3, run after compression, and is built
([EncryptionAtRest.md](EncryptionAtRest.md)): an encrypted message is one more entry in
the list of stages and no new format. It answered the blob question this way: **a blob
shared between accounts cannot be encrypted under one account's key**, and a key that is
not an account's is one the server can always use, so an encrypted mailbox holds no part
once - every part stays inside the message's own container, compressed and encrypted with
it. The index holds none of its words. `--compress-store` and `--decompress` leave an
encrypted mailbox's files to that row's `--encrypt-store` and `--decrypt-store`.

**Object storage** is a second `MessageStoreBackend`. A name is a local path today and
becomes a key; `WriteAtomically` becomes a put; `Link` answers false where the store
has no second name for an object, and every caller already handles that by copying;
`Identity` is what a walk uses to find the names of one object. Nothing outside the
store and the listed sites holds a path, which is what the seam was built for - and the
sites the checker lists (`build/check-message-store-seam.py`, `GetFileName` and the
account-folder `WorkingFile`) are the audit that row starts from: each is a place that
holds a local path and hands the file to something by path, which a key-based backend
must instead hand a temporary file.


Errors
------

| Code | What |
|---|---|
| `HM7350` | A stored message could not be read: a damaged container, a blob that is missing or does not match its hash, a version or a stage this build does not know. The message is refused, never served as something else, and the file is left as it is |
| `HM7351` | A message could not be stored in the compressed form and is kept as it came (it is read as well as any other), or a plain copy for a program that needs one could not be written |
| `HM7352` | A blob could not be written, counted, or let go of, or the store could not be measured. Counting errs high: a blob is kept rather than lost, and `--recount-store` corrects the count |
| `HM7353` | A message whose first bytes are the container's magic could not be stored as the message it is - it could not be wrapped (larger than a container holds, or the write failed) - and is refused rather than kept, since kept it would be read as a container. A delivery of one bounces (5.6.0), an `APPEND` is refused, and `MessageData` (a saved or a scheduled draft) will not write it |
| `HM7354` | A file the importer or the synchronizer found begins with the container's magic and is not imported: a stored container is not adopted from its file, and an imported message is never taken for one |

None of `HM7350`, `HM7351` or `HM7352` is raised by a server with compression off,
unless a stored file begins with the container's magic; `HM7353` and `HM7354` guard
against exactly such bytes and can be raised whatever the setting.


Tests
-----

`RegressionTests/Infrastructure/MessageStoreFormat.cs` (REST port 10080) reads every
container it meets with a decoder of its own, written from the table above rather
than from the server's code, and holds each of these claims to its negative control:
compression on against off, byte for byte; IMAP `BODY[]`, `RFC822`, `RFC822.SIZE` and
`SEARCH`, POP3 `RETR`, COM's `Body` and a DKIM signature made in the test and checked
over what IMAP hands out, which also ends with the message as it was sent; an `APPEND`
fetched back exactly, the literal and the CRLF `APPEND` writes; a literal that begins
with the magic and names another account's blob stored as the client's own bytes,
fetched back exactly, and letting go of nothing when it is expunged, so the other
account keeps its part; one blob for two messages, kept through a COM rewrite of one,
a `COPY`, a mailbox move and the consistency check, and gone with the last;
`DeliveryHardLinks` keeping one container with three names, the blob kept until the
last of the three is expunged; `--measure-store`, `--compress-store` and `--decompress`
on one account, the shared file left whole, and a `--domain` walk that rewrites a file
shared across two accounts once and relinks both names; a container cut short and one
with the wrong SHA-256 refused with `HM7350` naming the file, its `BODY[]` and its
`ENVELOPE` alike; a backup and a full restore; `GET /api/v1/store`, refused to a
domain-restricted key; `POST /api/v1/store/measure` forcing a measurement whose stored
bytes are below the messages' and whose saving is positive; and the two COM settings
and `MessageStoreStatus`. `MessageStoreFormat::Test`, run by `ClassTester::DoTests`,
holds the encoder and the decoder to each other without a server - the round trip,
`DecodeStart` and its refusal of a damaged or cut-short stream - and
`build/check-message-store-seam.py` proves its own negative controls on every run.
