Object storage for the message store
====================================

The messages in the mailboxes can be kept in an object store instead of under the data
directory: an **S3-compatible** store - AWS S3, MinIO, Wasabi, Backblaze B2's S3 API,
Ceph RGW - signed with AWS Signature Version 4, path-style or virtual-hosted, or **Azure
Blob Storage**, with Shared Key or a SAS, in block blobs. The database stays the index:
every folder, flag, size and UID is where it always was, and a message's object is named
by the same path its file had. It is how a store grows past one disk, and what the
active-active row will share.

**Nothing changes until a store is named.** The shipped default is the local file
system, and a server that never names a store reads and writes exactly as it did: it
asks one setting every five seconds and nothing else.

**Built 25 September 2026, and run against the lab the same day.** The requests were
proved against MinIO and Azurite (below, *How it was proved*). The server was compiled
and gated in batch37, and its fixture run from the laptop against the lab on `linix`:
the batch's first fixture run found one defect of the server's and three of the
tests', and after their fixes (93806de9b) the ObjectStorage, SecretsAtRest and EnvironmentConfiguration
fixtures passed 42 of 42; with the teardown fixed for a bench with no lab (f81eb664e),
ObjectStorage and EnvironmentConfiguration passed 28 of 28 against the lab, and the
eight ObjectStorage tests were skipped, naming what was missing, without it.


What goes to the store, and what stays
--------------------------------------

| Goes to the store | Stays on the local disk |
|---|---|
| Every message in an account's folders and in the public folders | **The delivery queue**: a queued message, and the copy a delivery makes while the rules, Sieve, the forward, the scanners and the seal work on it by path |
| The parts held once (`Blobs`, [MessageStore.md](MessageStore.md)) | Every name staged beside another (`.writing-`, `.encrypting-`, `.plaintext-`, `.linking-`, `.store-`): a working file of the name it is beside, never an object. There is no staging area in the store |
| The recover-deleted store's kept copies (`Recoverable`) | The temporary files, the logs, Sieve scripts, the confidential and protected messages' own stores, the list archive, eDiscovery's cases, the index, and the committer's journal (`_ObjectStore`) |

**Every write is a local one.** A message is placed on the local disk exactly as the file
system backend places it - a delivery, an `APPEND`, a collection by the IMAP mirror, a
migration, an import, a message written by path by COM's `Message.Save`, a script or the
webmail - and **the committer** puts it into the store afterwards, and only then removes
it from the disk:

* a message when its row is saved (`PersistentMessage::SaveObject`), handed over at once;
* anything else written through the store's seam - a rewrite by `--compress-store` or
  `--encrypt-store`, a blob, a kept copy - half a minute after it was written, when its
  writer is done with it. A name being made for a row that is not saved yet (a placing,
  an IMAP `COPY`) is handed over with its row, never before, so that a placing that is
  refused, or a row that is never saved, leaves nothing in the store.

The committer runs every minute on the scheduler, and puts what is due into the store on
the maintenance queue, fifty seconds of work at a time. **No name is ever put by two
passes at once**: a pass holds the names it took until it is done with them, and a pass
that has run ten minutes - a large message over a slow link - has another started beside
it for the other names only (the application log says so). A name the store does not take
is tried again after thirty seconds, then twice as long each time, fifteen minutes at most;
HM7473 says once an hour how many wait and why. What it has been handed is written to a
journal, `<data directory>\_ObjectStore\pending.txt`, and taken up again after a restart;
the journal is removed when nothing waits.

So **a delivery never waits for the store and never fails for it**, and **every read looks
at the local disk first, then at the store**: a file on the disk is the newest bytes under
its name, and one the store has not taken yet is still read. **The committer never deletes
a file where it is.** Under a lock it shares with every write, move and removal through
the seam it looks at the file once more and renames it to a name of its own, and only
then deletes it: a file written again while it was put is kept, and put again later; one
removed or moved away meanwhile has what was put removed again, so that the name holds
nothing, as its writer meant; a writer that renames its own file over the name without
the lock - a rule's or a script's rewrite (`MessageData::Write`) - does so either before
that rename, and the file set aside is told from the one that was put and goes back under
the name, or after it, and its file is a new one the committer never touches. A file a
reader holds open (Windows) is taken off the disk on a later pass, without another put; a
message deleted while its put held it open is removed with what was put when the put
ends. File calls made under these locks try once and say nothing, so that a scanner
holding a file costs no four-second wait and no error report under a lock. A file a crash
left set aside (`name.store-{GUID}`) with its name empty is put back under the name at the
next start and handed over again, and set-aside files a day old are removed.


Turning it on
-------------

1. **Make the bucket or container.** This server makes none. For S3, a bucket and an
   access key allowed `s3:GetObject`, `s3:PutObject`, `s3:DeleteObject`,
   `s3:ListBucket` and the multipart upload calls on it; add a lifecycle rule that
   aborts incomplete multipart uploads after a day. For Azure, a container and the
   account key, or a SAS with the blob service, the container and object resource
   types, and read, add, create, write, delete and list.
2. **Name it** (the settings below), with `message_store_backend` still `file`. From
   now on a message that is not on the local disk is looked for in the store.
3. **Copy the store into it**: `hmailserver --copy-store-to-object-storage`. It runs
   beside the server, verifies every object, and goes on where it stopped when run
   again.
4. **Switch new mail to it**: `message_store_backend` to `object`. Taken within five
   seconds; the committer puts each message placed from then on into the store.
5. **Remove the local copies** when you are ready: stop the server and run the copy
   again with `--remove-source`, which removes each local file whose object it has
   verified. Until then the local files are read first, and are the same bytes.

**Going back** is the same the other way: `message_store_backend` to `file` (new mail
stays on the disk again, and nothing more is put into the store; the store is still read
for what is only there), then `hmailserver --copy-store-from-object-storage`, then, once
it says nothing failed, `message_store_object_service` to empty. Turning the backend back
to `file` hides nothing: the store is read for as long as it is named.


The settings
------------

Rows of `hm_settings`, never `hMailServer.ini`; `/api/v1/settings` over REST, and the
Control Deck's *Settings > Server* form, which is built from the same table. There is
no COM member and no Control Panel page for them, as for single sign-on.

| Property | REST key | Default | What |
|---|---|---|---|
| `MessageStoreBackend` | `message_store_backend` | `file` | Whether the committer puts placed messages into the store: `file` or `object` |
| `MessageStoreObjectService` | `message_store_object_service` | empty | `s3`, `azure`, or empty for no store |
| `MessageStoreObjectEndpoint` | `message_store_object_endpoint` | empty | `https://host[:port]`; for Azurite `https://host:10000/devstoreaccount1`. Empty is the service's own: `https://s3.<region>.amazonaws.com`, or `https://<account>.blob.core.windows.net`. Printable ASCII only - no space, no control character, no `#` |
| `MessageStoreObjectRegion` | `message_store_object_region` | `us-east-1` | The region an S3 request is signed for |
| `MessageStoreObjectBucket` | `message_store_object_bucket` | empty | The bucket, or the Azure container |
| `MessageStoreObjectAddressing` | `message_store_object_addressing` | `auto` | S3: `path` (`https://endpoint/bucket/key`), `virtual` (`https://bucket.endpoint/key`), or `auto` - virtual for AWS's own endpoint, path for one you named |
| `MessageStoreObjectPrefix` | `message_store_object_prefix` | empty | Written before every key, so that one bucket can hold more than one server's store |
| `MessageStoreObjectAccessKey` | `message_store_object_access_key` | empty | S3's access key id; Azure's storage account name |
| `MessageStoreObjectSecret` | `message_store_object_secret` | empty | S3's secret access key; Azure's account key. **Protected** as the relayer password is, never emitted, and left out of a backup |
| `MessageStoreObjectSasToken` | `message_store_object_sas_token` | empty | Azure: a SAS (`sv=...&sig=...`), used instead of the account key. Protected, never emitted, left out of a backup; printable ASCII only |
| `MessageStoreObjectCaFile` | `message_store_object_ca_file` | empty | A PEM file of the CA that issued the store's certificate (below) |
| `MessageStoreObjectCacheMB` | `message_store_object_cache_mb` | `32` | What was read a moment ago, kept half a minute (below); 0 keeps nothing, 4096 at most |

The secret and the SAS are recorded on the audit trail as changed, never by value. A
backup does not carry them - each reads and writes every message in the bucket, this
server's future mail among them, and an archive would hold them in the clear - so after a
restore onto another server they are entered again; a restore leaves the ones a server
holds as they are.

**TLS verification is always on, and there is no setting that turns it off.** The
certificate is verified against the machine's roots and the host name checked; with
`MessageStoreObjectCaFile` set it is verified against the certificates in that file
instead - beside the system's bundle on Linux, and alone on Windows, where OpenSSL's
default directory is a path on the machine it was built on - for a MinIO or a Ceph
gateway behind a CA of the organisation's own, with the host name, or the address when
the endpoint is written as one, checked all the same. Plain `http` is taken only to this
machine (`127.0.0.1`, `::1`, `localhost`), as everywhere else in this server.
`HttpProxy` carries the requests as it carries every other client's.

Every setting is read again within five seconds of a change; a changed endpoint,
bucket, credential or CA makes a new client, and what the old one read a moment ago is
forgotten. The settings of a store that cannot be used - no bucket, an endpoint that is
not a URL or holds a space or a control character, an Azure key that is not base64 - are
said once in the error log (HM7472), and meanwhile nothing that is only in the store can
be read and nothing is put into it: new mail stays on the local disk, where it is read
from.


Names and keys
--------------

A key is the message's path under the data directory, `/` between the parts, **letters
made small** as a directory's are on Linux, in UTF-8, after the prefix:

    <data>\example.com\alice\4C\{4C0A...}.eml  ->  mail/example.com/alice/4c/{4c0a...}.eml
    <data>\Blobs\9f\9f86d0...                  ->  mail/blobs/9f/9f86d0...
    <data>\Recoverable\12\5B1E....eml          ->  mail/recoverable/12/5b1e....eml

so a store is a copy of the tree, and the same tree copied from a Windows server and a
Linux one gives the same keys. A name directly in the data directory is a queue file and
has no key, and neither has a name staged beside another: those stay on the local disk.


What each operation is
----------------------

The seam's backend (`MessageStoreBackend`, [MessageStore.md](MessageStore.md)) and what
each store does for it. Every operation reads the local disk first where it reads.

| Operation | S3 | Azure Blob Storage | When it fails |
|---|---|---|---|
| **Exists**, **Size** | `HEAD` the object | Get Blob Properties (`HEAD`) | Only a `404` while the bucket is there is "not there": a 404 is taken for an absent object after `HEAD` on the bucket (or the container) has answered within the last minute, since S3 answers a `HEAD` in a missing bucket with the same 404. **Anything else** - a redirect, a `400`, a `405` from a proxy, a `409`, a `412`, a `421`, no answer - is *unavailable* |
| **OpenRead** | `GET` with `Range`: the first megabyte, then 8 MB at a time, every window after the first held to the first one's ETag with `If-Match` | `GET` with `x-ms-range`, the same, `If-Match` signed with the rest | The read fails and the thread is marked unavailable: a message the row names that cannot be read now is the store's condition, never a missing message. A `412` - the object rewritten in the middle of the read - fails the read rather than splicing two versions |
| **ReadAll** | `GET` in the same windows, to the end | the same | as above |
| **WriteAtomically** | On the local disk, under the name; the committer puts it into the store | the same | False when the local disk does not take it |
| **Copy** | A local file on the disk, where it is; an object onto a new name by `CopyObject` (`PUT` with `x-amz-copy-source`) - a 200 whose body is an `<Error>` is a failure; out of the store onto a local name by a read | With Shared Key and a name that holds nothing, Copy Blob, waited for up to five seconds and aborted when it has not finished; onto a name that holds an object, or with a SAS, read and put again (Put Blob and Put Block List replace a blob whole or not at all) | False |
| **Move** | A local file moved on the disk, and an older object under the old name removed; an object only in the store copied, then `DELETE` the old key | the same | A copy that fails leaves the old key as it was. A crash, or a `DELETE` that fails, **between the two** leaves the object under both names, never under neither; the old one is named by no row (HM7475 says which when the `DELETE` failed) and costs space only (below) |
| **Link** | false while new mail goes to the store | the same | Every caller copies instead (below) |
| **Remove** | The local file, then `DELETE`; a key that is not there is gone | the same with Delete Blob; 404 is gone | False while the store does not answer: the name may still be there |
| **Identity** | `object:<key>:<ETag>:<size>`, one name; an answer kept without an ETag is asked again | the same with Azure's ETag | False when it cannot be asked |
| **List** | `ListObjectsV2` by prefix, every page | List Blobs by prefix, every page | False |
| **Commit** (the committer) | The local file put whole: one `PUT` up to 16 MB, above it a multipart upload - `POST ?uploads`, `PUT ?partNumber&uploadId` for each 16 MB, `POST ?uploadId` with the list - **aborted** (`DELETE ?uploadId`) when a part or the completion fails; then the file removed from the disk | Put Blob (`x-ms-blob-type: BlockBlob`, `x-ms-blob-content-md5`) up to 16 MB; above it Put Block for each 16 MB and Put Block List with the MD5 of the whole | The file stays on the local disk, read from there, and is tried again (HM7473 once an hour) |

A `PUT` is atomic per object, and a multipart upload and a block list are committed by
their last request: a key holds the old bytes or the new ones, never a part. The
completion of a multipart upload has the whole upload's deadline, since S3 assembles the
parts before it answers; a completion tried again after an answer that was lost gets
`NoSuchUpload`, and the object is then taken for this upload when its ETag is the one these
parts make. Blocks never committed are Azure's to expire (a week).

Every request is tried **three times at most**, 200 ms and then 800 ms apart, on a
connection that was refused or dropped, a timeout, `408`, `429` or `5xx`; each try is
bounded as a whole - the lookup, the connection, the handshake and the transfer - by 15
seconds and 2 more per MB it moves, 120 at most: a read window's by the window, a put's by
what it puts. A request that ends without an answer leaves the store alone for ten
seconds, during which every request is *unavailable* at once without asking, and the
committer stops its pass. `401` and `403` are the store refusing this server's
credentials: not retried, *unavailable*, and HM7471 says so; they are never taken for an
absent message. A `404` to a put, or to the bucket's own `HEAD`, is a bucket or container
that is not there: *unavailable* too, and HM7474 says so. Any other `4xx` is a failure of
that request (HM7474), which the doors answer as *unavailable* as well. An answer whose
body ends before the `Content-Length` the store gave - a connection cut in the middle - is
tried again like one that never came, and never taken for the object; a `200` to a ranged
`GET` - the range ignored by the store or a proxy - is cut to the window that was asked
for, never taken for bytes from the offset.


When the store cannot be reached
--------------------------------

**Mail is never lost to a store that is down.** What is written is written to the local
disk, and what cannot be read is *unavailable* - temporary - at every door:

* **A delivery** is placed on the local disk and done, as with the file system: nothing
  waits for the store, nothing is held in the queue for it, and the rules, the forward
  and the auto-reply run once. The committer puts the message into the store when it
  answers again.
* **IMAP** `FETCH` answers `NO [UNAVAILABLE]` (RFC 5530) for a message only in the store -
  before a byte of that message's response is sent, and so also when the store stops
  answering between the existence check and the read of its header or structure, which
  would otherwise go out as an empty `ENVELOPE` or `BODYSTRUCTURE` the client would keep.
  `APPEND` is placed on the local disk like a delivery.
* **POP3** `RETR` and `TOP` answer `-ERR [SYS/TEMP]` (RFC 3206).
* **Every reader of a message is a reader of the disk or the store, whichever holds it at
  that moment.** While a store is named a plain message is parsed from memory, read
  through the seam, never by path; `LoadMime`, the header and the body loads fall back to
  the store when the committer took the file off the disk between the question and the
  open; a file handed to SpamAssassin is a temporary copy; and a message the store has is
  never taken for one being composed because its file is not on the disk.
* **The webmail and REST** answer 503 for a message that cannot be read now; a message
  put into a folder is written to the local disk, like any other.
* **A message the store cannot say anything about is never taken for a missing one.** The
  consistency check does not list it, the recover-deleted store does not purge its kept
  copy, and a message a rule or a script is rewriting is not taken for one being composed.
* **No placeholder is ever written while a store is named.** For a message whose file is
  gone the file system backend writes a placeholder saying so; with a store named, a 404
  is not that word - a move under way, a prefix or a bucket changed and changed back, a
  store that answered 404 for a moment all look the same - and a placeholder saved would
  be put over the message's own key. HM7476 names it, once a minute at most. IMAP's
  `FETCH` leaves that one message out of its answer, fetches the rest of the range, and
  ends `NO`, so a message lost from the store never stops a client's sync at itself; POP3
  answers that message `-ERR` without `[SYS/TEMP]`, since it is not coming back.
* **A deletion** that keeps a copy for recovery is refused while the store cannot give the
  copy, as a legal hold refuses one: the message stays, flagged deleted, and goes - with
  its copy kept - once the store answers.
* **A recount of the parts held once** (`--recount-store`) ends with nothing changed when
  the store does not answer for any name: a count made without every name is too low,
  and an exact one would then lower and remove the parts of the names it did not see.
  **The consistency check** lists no part as missing on the store's silence, and lists
  none as unreferenced when it could not read every name.

The queue itself is on the local disk, and never in the store.


Link and Identity: who asks, and what they do without them
------------------------------------------------------------

An object store has no second name for an object, so `Link` answers false while new mail
goes to one. Every caller was already written for a volume that cannot link:

| Caller | What it does when Link answers false |
|---|---|
| `PersistentMessage::CopyFromQueueToInbox` (a recipient's copy linked to the queue file, `DeliveryHardLinks`) | Copies the queue file, which is "exactly the behaviour without the setting" - the fallback it always had |
| `LocalDelivery`'s shared template (`DeliveryHardLinks`) | Every recipient's copy is then a copy of its own, so `SameFile` never finds two names of one file and each copy is placed on its own, as with the setting off |
| `MessageStore::Link` (counts the blobs, then links) | Lets go of the counts it took and answers false |
| `MessageStore::Relink` (`LocalDelivery::PlaceCopy_`'s next recipient; `--compress-store`'s rewrite of a file with several names) | Answers false with the name as it was: the delivery then places the recipient's own copy, and the walk leaves the file and counts it; neither arises on a store, where no file has two names |

`Identity` is asked by the decoded-container cache (which object a name is, with the
ETag telling a rewrite from the file it replaced - a local file's last write is asked
only of a local file), by `SameFile` (two names of one file: never, for two keys), by
`Scrub` (overwrite only a file with one name: an object has one, and is not a local
file, so nothing is overwritten - what `Scrub` exists for is always the local working
file), and by the store's walks (`--measure-store` counts a file with several names once;
on a store every name is one object). An answer the read cache kept without an ETag - what
this server wrote or read without being told it - is asked of the store again, so that a
rewrite of the same size is never taken for the object it replaced.


The copy of the tree
--------------------

```
hmailserver --copy-store-to-object-storage   [--account <address> | --domain <name>] [--remove-source]
hmailserver --copy-store-from-object-storage [--account <address> | --domain <name>] [--remove-source]
```

Run as the user the server runs as, with the same `hMailServer.ini`; on Windows
`hMailServer.exe` takes the same switches. The names are the database's - every
message's file, every kept copy, and every blob: on a run over the whole store all of
them, and on a run over an account or a domain the ones its messages and kept copies
reference, which they cannot be read without - so a working file the server is writing is
never touched. A kept copy the store could not be asked about is listed with the rest and
copied or counted as failed by name, never left out; a list of kept copies or of blobs that
could not be read fails the run. For each name:

* **To the store**: a name only in the store is *there already*; a local file whose
  object is there and verifies is *there already*; any other is put - multipart above
  16 MB - and verified. **Verified** is the object's size, and its MD5: Azure's
  `Content-MD5`, or S3's ETag where it is one (a single `PUT`'s ETag is the MD5, a
  multipart upload's the MD5 of its parts' MD5s and their number, which is worked out
  here from the file); where neither holds - a store that encrypts with its own keys -
  the object is read back and its MD5 taken. With `--remove-source` Azure's `Content-MD5`
  is not taken for one - it is the MD5 this server gave the store, which the store keeps
  without checking - and the object is read back before the local file goes.
* **From the store**: a name only on the disk is left; a local file and an object that
  do not verify against each other are both left, and the run fails; any other object is
  read, written beside its name and **linked** to it, which never replaces a file - one
  the running server wrote under the name while the object was read is newer, and is
  left and counted as there already - and verified.
* **`--remove-source`** removes what was copied from - the local file, or the object -
  once its copy is verified, and only with the server stopped (exit 4 beside it).

A run that stopped goes on where it stopped: what is there and verifies is left. It
prints what it copied, found there already, removed, found missing on both sides, and
could not copy; the exit code is 0 when nothing failed, 1 when a name could not be
copied or verified or a list could not be read, 2 when no store is configured or there is
no such account or domain.


What else meets the store
-------------------------

* **Backups.** A backup with messages copies the data directory as always, and then
  reads every message and blob the database names that is only in the store into the
  staged tree under its relative name, so the archive holds every message whichever
  backend holds it; a store that cannot be read fails the backup rather than let it be
  taken without them. A restore puts the files back on the local disk, where they are
  read first; the copy verb moves them into the store when you like.
* **An account renamed, or moved to another domain, or a domain renamed.** The directory
  on the disk is moved as it always was, and the objects under the old name are copied
  under the new one and removed; the files on the disk under the new directory are then
  handed to the committer under their new names. **A name the new directory already holds
  is never written over**: the old object goes only when the new one is the same object -
  the same size, and the same ETag or MD5, or the same bytes read back - and otherwise
  both are left and HM7475 says how many and which first. A move that fails part way is
  refused and put back like the rest of it - the objects moved so far, then the
  directories - so the row, the directory and the objects stay together at the old name;
  what the store will not give back (it stopped answering in the middle) stays under the
  new name, never under neither, the error says how many, and the mailbox move is
  journaled as a rollback that failed. The move made again carries the rest across. A
  crash in the middle of one object's move leaves it under both names, and the move made
  again removes the old one once it finds the new one the same object.
* **Late arrivals.** Mail delivered under the old name while the move runs is swept
  across afterwards, once the row and the cache carry the new name - a mailbox move's by
  its own sweep, a domain rename's by one of its own. The committer is held off the old
  directory for the sweep, once no name under it is being put (two minutes at most); the
  files still under the old directory on the disk are moved under the new first, one whose
  name is taken is left, and then the objects under the old prefix are moved as above - so
  no late arrival is put under its old name after those objects are listed. What cannot be carried across is left
  under the old name and said (HM7478 for a domain).
* **The recover-deleted store** keeps its copies in the store, puts them back from it, and
  its sweep of a deleted account's copies lists them there.
* **The consistency check** asks the store whether each message is there, and lists the
  blobs nothing references there too - unless the store did not answer for every name.
* **SpamAssassin's learning**, which needs a file, is handed a temporary copy of a plain
  message, as it is of a compressed one.
* **Compression, parts held once and encryption at rest** work unchanged: a container
  is an object, a blob is an object, and an encrypted container is written on the local
  disk already encrypted and put into the store so - the plaintext of a delivery is the
  local working file it always was, overwritten and removed, and a rewrite by
  `--encrypt-store` stages its plaintext and its container on the local disk, where the
  start's sweep of encryption leftovers finds what a crash left, as on the file system.
* **The Import Tool** walks the local disk. An import whose row cannot be saved puts the
  file back on the local disk as it was, never through the committer, so that the next
  run finds it.

### What was read a moment ago

An IMAP `FETCH` asks whether a message is there, its size, its header, its structure and
its bytes, and each would be a request. `MessageStoreObjectCacheMB` (32 MB) keeps, for
half a minute, what the store answered about a key - its size and ETag - and the bytes of
an object up to a quarter of that; a message the committer has just put is kept the same
way. Everything this server writes under a key is forgotten as it is written, and
everything is forgotten when the store's settings change. Another server writing the same
store is seen within the half minute; set it to 0 where that matters.


What is thinner than it could be
--------------------------------

* **One connection per request**, HTTP/1.0 over TLS - the tree's own client - so every
  request pays its handshake. The read cache takes most of that off a `FETCH`.
* **One committer**, a pass at a time: a server that places mail faster than one
  connection puts it keeps more of it on the local disk for longer, read from there.
* **A message is read whole into memory** to be written, as the file system backend
  reads it, and a large message is uploaded from the local file 16 MB at a time.
* **The parts held once are checked under their lock**: placing a message whose part is
  already in the store asks the store for it, and a store that does not answer holds
  that placing - and others with parts held once - for the bounded wait of three tries
  before the part is written again on the local disk from the message's own bytes.
* **Credentials are the two the settings hold**: no instance role, no workload identity,
  no Azure AD token; an S3 store that encrypts with its own keys is verified by reading
  back.
* **`Message.Filename`** over COM, a script's, and the REST message resource's `file`
  still answer the path the message would have: once committed there is no file there.
  Read the message through the API.
* **The Import Tool and the data directory synchronizer** walk the local disk; run the
  copy from the store first when they must see the whole store.
* **A prefix changed** is not a move: what is under the old one is not looked for.
* **No scan for objects no row names.** The consistency check lists the blobs nothing
  references, but not a message object left under its old name by a crash between a
  move's copy and its delete, or by a move whose new name held a different object; it
  costs space, never mail.
* **A file written into in place** - opened and written, not renamed over its name - on
  Linux while the committer sets it aside would go with it. The tree has no such writer of
  a name the committer holds: every rewrite of a stored message renames a new file over the
  name, and the writers that write into a file in place write working files and new names,
  which are handed over only once their rows are saved.
* **The fixture runs only where the lab is.** The second Windows bench has no
  `HMAILSERVER_OBJECTSTORE_LAB`, so a gate there skips it; it has run from the laptop
  against the lab on `linix` (25 September 2026, above), and not in the Linux suite.


Errors
------

| Code | What |
|---|---|
| `HM7470` | The store could not be reached: three tries, none answered (a refused connection, a timeout, `408`, `429`, `5xx`). Once a minute at most |
| `HM7471` | The store refused this server's credentials (`401`, `403`). Once a minute at most |
| `HM7472` | The store's settings do not describe a store this server can use. Once for each change of them |
| `HM7473` | Message files wait on the local disk for the store to take them: how many, and the last one it did not take and why. They are read from the disk meanwhile, and nothing is lost. Once an hour at most |
| `HM7474` | The store refused a request for a reason of its own (another `4xx`, an answer that could not be read), or the bucket or container is not there (a `404` to a put or to the bucket's `HEAD`). Once a minute at most |
| `HM7475` | An object was copied under its new name and the old one could not be removed afterwards, or a move found the new name holding a different object and left both: named by no row |
| `HM7476` | A message is neither on the local disk nor in the store; no placeholder is written for it while a store is named. Once a minute at most |
| `HM7477` | The request signatures' own tester failed (ClassTester) |
| `HM7478` | Mail delivered under a domain's old name while it was renamed could not all be carried across |

None is raised by a server with no store named.


How it was proved
-----------------

**The signatures**: `ObjectStoreSignatureTester` (in `ClassTester` on Windows, and
`object-store-signature-tests`, run by the `linux-build` job of `.gitlab-ci.yml`, on Linux) holds SigV4 to AWS's
own Signature Version 4 test suite - `get-vanilla`, `get-vanilla-query`,
`get-vanilla-empty-query-key`, `get-vanilla-query-order-key`,
`get-vanilla-query-order-key-case`, `get-vanilla-query-order-value`,
`get-vanilla-query-unreserved`, `get-vanilla-utf8-query` and `get-unreserved`, for the
service `service` in `us-east-1` at `20150830T123600Z`, with get-vanilla's canonical
request, string to sign and `Authorization` header compared whole - and to the four S3
examples of the SigV4 documentation (`GET` with a `Range`, `PUT` of `test$file.text`,
`GET ?lifecycle`, `GET ?max-keys=2&prefix=J`), each with a negative control. Shared Key
is held to a string to sign written out from Microsoft's specification and to signatures
computed apart from this code.

**The requests**: on 25 September 2026, MinIO (`pgsty/minio`, RELEASE.2026-08-04, from
Docker Hub - `minio/minio` is no longer there) and Azurite
(`mcr.microsoft.com/azure-storage/azurite`) ran on the lab VM behind a certificate from
a lab CA, and a harness sent them every request this client makes, byte for byte in
HttpsClient's shape - HTTP/1.0, the same headers, signed the same way - 53 checks, none
failed: `PutObject` (its ETag the MD5), `HeadObject`, a ranged `GetObject` (206,
`Content-Range`), the same held to its ETag with `If-Match` (206) and to another (412), a
range past the end (416 - MinIO sends no `Content-Range` with it, and the first window of
an empty object is the one read that asks for it), a whole one, a virtual-hosted
`GetObject`, `CopyObject`, `DeleteObject` (204, and 204 again for a key not there),
`ListObjectsV2`, a three-part multipart upload read back whole, an aborted one that leaves
nothing, and a wrong secret answered 403, never 404; Put Blob with its `Content-MD5` kept,
Get Blob Properties, a ranged Get Blob, the same with `If-Match` signed in Shared Key's own
place for it (206) and another ETag (412), a range past the end (Azurite: 206, no bytes and
the total; 416 for an empty blob), Copy Blob, Delete Blob (404 for a blob not there), List
Blobs, four Put Blocks and a Put Block List read back whole, the same with an account SAS,
and a wrong key answered 403. Azurite answers a Copy Blob whose source carries a SAS with a
500, which is why a copy with a SAS is read and put again.

**The server**: `RegressionTests/Infrastructure/ObjectStorage.cs` runs the server against
both, through a relay on the bench's loopback address (10244, 10245; REST on 10246): every
door on each store - delivered and put into the store by the committer, `FETCH`, `RETR`,
`COPY`, `MOVE`, `EXPUNGE`, and read again after a restart - with the objects looked at by a
SigV4 and Shared Key client of the fixture's own; an outage, waited out on the local disk -
delivered at once, read while the store is down, never queued or bounced, and put into the
store once it answers; IMAP `NO [UNAVAILABLE]` and POP3 `-ERR [SYS/TEMP]` while the store is
down and the message intact after; a wrong secret; a message the store has lost, never
replaced by a placeholder; the copy verb both ways and its resume; and the default touching
no store. It needs `HMAILSERVER_OBJECTSTORE_LAB` (the lab host) and
`HMAILSERVER_OBJECTSTORE_CA` (the lab CA's PEM, where the service can read it), and skips
naming what is missing without them; with `HMAILSERVER_OBJECTSTORE_SSH` it stops the
MinIO container itself for the outage instead of the relay.

### The lab

On `linix` (192.168.11.154), under `~/row111-objectstore-lab`: `certs/` (the lab CA,
`ca.crt`, and the servers' certificate for `192.168.11.154`, `127.0.0.1`, `linix` and
`*.s3.lab.test`), `row111-minio` on 10240 (`MINIO_ROOT_USER=hmslab-access`,
`MINIO_ROOT_PASSWORD=hmslab-secret-row111`, `MINIO_DOMAIN=s3.lab.test`, bucket `hmslab`)
and `row111-azurite` on 10242 (`devstoreaccount1` and the emulator's published key,
container `hmslab`), both `--restart unless-stopped`.
