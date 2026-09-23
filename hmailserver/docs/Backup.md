What the backup carries, and what it does not
=============================================

The built-in backup writes one 7-Zip archive per run: an XML index, and — when
**Back up messages** is on — the whole message store beside it. A restore is
destructive and has no transaction: it deletes every domain, every account and
every public folder before it puts the archive's back. So the only question that
matters about an archive is what is *in* it, because everything an account has
that is not in it is deleted by the restore and not replaced.

This document answers that question, item by item, and is checked rather than
remembered: `build/check-account-stores.py` reads the `CREATE TABLE` scripts
against the server's own declaration and fails the build when a table that holds
one account's data is in neither list. It runs on every pull request.

Before 15 September 2026 seven of the tables below were in neither, and
restoring an archive destroyed them. If you are running 6.3.3 or earlier, read
the last section.


In the archive
--------------

### The server

| What | Notes |
|---|---|
| Domains, and their aliases, distribution lists and signatures | With each domain its allow and block list, its spam traps, its quarantine digest switch and hour, and its own values of the settings with levels (`hm_settingoverrides`, scope 1; see [SettingLevels.md](SettingLevels.md)) |
| Accounts | Address, password hash, forwarding, vacation, signature, quota, admin level, TOTP secret, anti-spam thresholds |
| Aliases and distribution lists, with their members | |
| Routes, IP ranges, TCP/IP ports, SSL certificate *records*, incoming relays, groups | Only with **Back up settings** on |
| Anti-spam and anti-virus lists | DNS black lists, SURBL servers, white list, blocked senders, the server's allow and block list, blocked attachments, greylisting white list |
| Public folders, and the permissions on them | |
| The server settings that were the `[Settings]` section of `hMailServer.ini` | The rows of `hm_inisettings`, which is the settings store since schema 6042; the section in the file is the server's copy of them, and is not what the archive reads. Carried as name and value with **Back up settings** on, so a row this build does not know still comes back. **Not `PasswordPepper`** - see *Not in the archive* |
| Custom administrator roles | The rows of `hm_roles` that are not built in, with their permissions and descriptions, whenever **Back up domains** or **Back up settings** is on. Restored first, before any domain is removed, so that the assignments the accounts carry find their role on a server that has never seen it. A role the restoring server already has is kept as it is there, never overwritten with the archive's, and the difference is named in the backup log. See [Roles.md](Roles.md) |

### Everything an account has

| What | Table | Notes |
|---|---|---|
| Folders and messages | `hm_imapfolders`, `hm_messages` | Only with **Back up messages** on. A message keeps its UID, its flags **and its keywords** — the webmail's labels, the second and third stars, follow-up flags, mute and pin |
| Rules | `hm_rules`, and its criteria and actions | |
| Fetch accounts, and the UIDs already downloaded | `hm_fetchaccounts`, `hm_fetchaccounts_uids` | |
| Application passwords | `hm_apppasswords` | The hashes, so a restore does not silently revoke every client the account holder set up |
| The address book | `hm_contacts` | Including the vCard a CardDAV client stored, under the resource name it chose |
| The webmail's remembered choices | `hm_accountprefs` | Theme, density, reading pane, notifications, language, labels and their colours, saved searches, swipe actions — the server attaches no meaning to a key and carries each verbatim |
| Scheduled sends and snoozes | `hm_scheduled` | See *References*, below |
| Files sent as expiring links | `hm_files` | The record: name, owner, size, expiry, download count and the password hash. The bytes are not messages: they are files under `<data directory>\Files\<token>`, and an archive holds them only because **Back up messages** copies the whole data directory. An archive taken without it, or with `BackupMessagesDBOnly`, has the records and not the files; restored onto a data directory that does not hold them, each link answers *No such file* |
| S/MIME certificates and keys | `hm_smimekeys` | Including the wrapped private key. This server cannot open it, so if a restore lost it nothing would fail — the account would simply never read its own encrypted mail again |
| The password reuse history | `hm_passwordhistory` | So a restore does not let every account go back to a password it was made to change |
| Who has written to the account before | `hm_knownsenders` | The memory the first-contact note is decided against, as of the backup. Without it a restore would announce every correspondent of every account as a first contact again — see [DomainTransforms.md](DomainTransforms.md) |
| Calendars and their events and tasks | `hm_calendars`, `hm_calendarobjects` | Tombstones included, so a CalDAV client's next sync is answered correctly rather than being told nothing was ever deleted |
| Safe senders and blocked senders | `hm_senderlist`, the account's own entries | With their reasons and expiries. The table also holds each domain's list and the server's, which travel with the domain and with the settings; see [SenderListsAndDigests.md](SenderListsAndDigests.md) |
| Its own values of the settings with levels | `hm_settingoverrides`, the account's own rows | Written back under the restored account's id through the same checks a write makes, less the limit's ceiling, which caps the value when it is read; a row this build refuses is left out and named as `HM7312`. See [SettingLevels.md](SettingLevels.md) |
| Whether the quarantine digest was stopped | `hm_digeststate` | A person who asked for the digest to stop is still opted out after a restore. The digest's position is carried too, and an entry held after the last digest went is still new if a restore onto a fresh database starts the quarantine's numbering again |
| The administrator role it was given, and the domains it reaches | `hm_roleassignments` | The role by its name, found again on the restoring server. Put back first thing after the account's row, before its folders, and the account's admin level - which the archive also carries, and which alone would give back authority the role had narrowed away - is withheld until the assignment is written and given back only behind it, so a restore that stops part-way leaves no account wider than its role. An assignment naming a role that neither the archive nor that server has is dropped, the level stays withheld, and the backup log and the error log (`HM6129`) name the account and the missing role |

The per-account stores are carried as rows under their account, in the archive's
own XML. A value is written as an attribute where every character of it is safe
in one, and as base64 of its UTF-8 under the same name with `.base64` appended
where it is not — so most of the index stays legible, and a vCard, an iCalendar
object or a PEM block cannot make the archive unreadable.


Not in the archive, deliberately
--------------------------------

| What | Why, and what to do |
|---|---|
| `[Database]` and `[Directories]` in `hMailServer.ini` | Per-machine. They are how this installation finds its database and its data folder, and they would be wrong on the machine you restore onto. Keep a copy of the file |
| The administrator password and its second-factor secret | In `hMailServer.ini`, for the same reason: they are what lets you in when there is no database |
| `PasswordPepper` | The one `[Settings]` key the settings store never holds and the archive never carries. Its whole purpose is to be somewhere the password hashes are not, and the archive holds the hashes: every Argon2id and scrypt hash was made with it, and without it each verifies false. It lives in `hMailServer.ini` only - the server takes it back out of the database if an earlier start put it there, and says so in the application log. **A restore onto new hardware needs the same pepper in that machine's `hMailServer.ini` before an account whose password is an Argon2id or scrypt hash can sign in**; PBKDF2, SHA-256 and MD5 hashes, and the administrator password, do not use it. Keep a copy of the value with the backups, apart from the archive |
| TLS certificate and DKIM key **files** | The database stores the *paths*, not the files. A restore onto a machine where those paths hold nothing gives you a server that starts, serves no TLS and signs nothing. Keep them inside the data directory (where the message-store backup carries them) or copy them yourself |
| Mail still in the delivery queue, and its recipients | A queued message is in flight rather than in a mailbox. Let the queue drain before a migration |
| The full-text index | Derived from the messages, and rebuilt by the indexer. Carrying it would roughly double the archive to save work a background task does anyway |
| Per-message and per-mailbox annotations (RFC 5257, RFC 5464) | Keyed on ids a restore reassigns. Recorded as a gap in `Roadmap2.md` rather than silently dropped |
| The expunge log a QRESYNC client is answered from | Restoring it would tell a client that messages the restore has just put back are gone |
| What the last reputation check found (`hm_reputationchecks`) | An operational record about this machine's own addresses, rewritten by the next check, which runs within a day of the restore. What it checks - the three `ReputationWatch` settings - is in `hm_settings`, which the archive does carry. |
| Quarantine, message trace, metric history, the archive index | Server-wide operational records rather than an account's data. Back the database up if you need them |
| The journal of mailbox moves (`hm_mailboxmoves`) | A record of what this installation did to its own store, not part of a mailbox. A restore is not a moment in a move: the account comes back at the address the archive holds, so a carried row would describe a move this store has no trace of. The audit trail is the record a person reads - see [MailboxMove.md](MailboxMove.md) |
| The links in quarantine digests already sent (`hm_digesttokens`) | Each is good once and expires within `QuarantineDigestLinkHours`, and names a quarantine entry the archive does not hold. The next digest issues new ones |


References: how a scheduled send survives a restore
---------------------------------------------------

A restore inserts every row afresh and the database hands out new identities, so
nothing in the archive can be an id. That is what makes the archive portable
between database backends at all — see
[MigratingDatabaseBackend.md](MigratingDatabaseBackend.md).

One row type names another: a scheduled send names the draft to send and the
folder to return a snoozed message to. Writing those numbers into the archive
would, after a restore, point a scheduled send at whatever message happened to be
given that id — so they are carried as what survives a restore instead: the
folder's path, segment by segment, and the message's UID. On the way back in the
path and the UID are resolved against the folders and messages this restore has
just put back.

A reference that cannot be resolved — the draft was deleted, or the archive was
restored without its messages — drops that one row, counts it, and says so in the
backup log by account. Nothing else in the account is affected. Restoring a row
that points at nothing is the one outcome worse than losing it.


What the verification proves
----------------------------

Every run reads its own archive back before it is called a backup, and before
retention is allowed to consider deleting an older one:

1. **The index opens**, through the same code a restore uses to open one, and
   describes what this run put in it.
2. **The per-account rows are all there.** What the run built is counted against
   what the archive reads back, per table. A difference means the archive does not
   contain what was backed up, so it is discarded and the backup fails.
3. **The message store extracts** to exactly the files and bytes that were staged
   for compression. Fewer of either is a truncated archive, whatever 7-Zip's exit
   code said.
4. **Every message row has a file.** This one is *reported*, never failed: the
   rows were read before the copy started and mail flow does not stop for a
   backup, so a message deleted in between is normal. A large number means the
   backup was taken during a bulk deletion and is worth taking again.

Step 3 costs disk: for the length of the check the message store exists twice, in
the temp directory. A temp volume that cannot hold it skips the step with an
explanation rather than failing a good backup. `BackupVerifyRestore=0` turns
steps 3 and 4 off and says so in the log.


An archive that is encrypted
----------------------------

Off until a passphrase is set, and then every archive is encrypted as it is
written: **7-Zip AES-256, with the archive's headers encrypted too** (`-mhe=on`),
so that an archive without its passphrase cannot even be *listed*. Whoever finds
the file learns neither the mailboxes in it nor that it is an hMailServer backup.

Set it in the Control Panel (**Backup & restore → Encryption**), in the Control
Deck's Backup view, over REST (`encryption_passphrase` in
`PUT /api/v1/settings/backup`), or with `hmctl backup passphrase set`. Clearing
it — an empty passphrase — goes back to unencrypted archives.

### What the passphrase is, and where it is not

| | |
|---|---|
| Kept | In the database, as the `backupencryptionpassphrase` row of `hm_settings`, protected by the machine's secret store: a DPAPI envelope on Windows while `ProtectStoredSecretsWithDPAPI` is 1, the key file on Linux — the way the route and relay passwords are kept |
| Never | On a command line (see below), in a log, in a COM or REST read, or **in the archive itself** |
| Written to the archive | Nothing. The settings section the archive carries skips this one setting, as the stored `[Settings]` skip `PasswordPepper`. A key kept inside the lock it opens is not a key — and it also means a restore never overwrites the passphrase this server is using with one out of an archive |
| Read back | Never. There is no getter over COM and none over REST. What can be asked is whether one is **set**: `Settings.Backup.EncryptionPassphraseSet`, `encryption_passphrase_set` |
| If it is lost | **Everything in every archive written with it is lost.** Not recoverable by this server, by its author, or by anybody. Keep it somewhere other than beside the backups — a restore asks for it, and the day it is needed is the day the server it was stored on is gone |

A passphrase is 12 to 256 **printable ASCII** characters and may not begin or end
with a space. The ASCII rule is not fussiness: the passphrase reaches the
archiver through its standard input and is decoded by whatever code page or
locale that archiver runs under, which is not the same on the Windows machine
that wrote the archive and the Linux machine somebody restores it on. Within
ASCII there is no such difference, so the archive opens with the same passphrase
in any 7-Zip anywhere.

### Not on the command line

Everything after a program's name is in the process list, which every user of the
machine can read. So the passphrase is never an argument: 7-Zip is given `-p`
with **no value**, which is its "ask for the password", and the server writes the
passphrase into the process's standard input before the process starts
(`ProcessLauncher::SetStandardInput`, `Compression::LaunchCommand_`). The same
change also means an archive that asks for a passphrase nobody gave now reads the
end of its input and fails, where before it would have waited for ever at a
console prompt nobody could see.

The archiver's own **output** is left where it was: on Linux it is the server's,
so 7-Zip's account of a failure — an unsupported switch, a full disk, a refused
path — still reaches the journal, which on that platform is the only explanation
anybody gets beyond an exit code; on Windows it goes to the null device, because
a service has no console for a child to write to either way.

`hmctl` holds the same line: `hmctl backup passphrase set` and
`hmctl backup restore` take `--passphrase-file FILE` (or `-` for standard input)
or `--ask`, and never a passphrase as an argument.

### The restore

Every way in asks for it, and every one of them refuses **before anything on the
server is touched** — the archive is opened, its version checked and what was
asked for checked against what the archive holds, and only then is the
destructive part begun:

| Where | How |
|---|---|
| Control Panel | The **Restore** card's passphrase box |
| COM | `BackupManager.LoadEncryptedBackup(file, passphrase)`, beside the old `LoadBackup(file)` |
| REST | `POST /api/v1/backup/restore` with `file`, `passphrase` and the three parts — **the only restore a Linux server has**, since COM is Windows-only |
| Control Deck | The Backup view's Restore card |
| hmctl | `hmctl backup restore FILE --domains --messages --passphrase-file FILE` |

The two failures are told apart, because the answers are different: *this archive
is encrypted and you gave no passphrase* (find the passphrase) and *it could not
be opened with the passphrase given: either it is not this archive's passphrase,
or the archive is damaged* (find another archive). Which one it is comes from the
archive's own header — `Compression::GetArchiveEncryption` reads the 7z signature
header and the coder ids of the block it points at — so it costs nothing and
needs no passphrase. `Utilities.BackupArchiveIsEncrypted` answers the same
question over COM.

The question that header answers is narrow, and worth stating exactly: **whether
the archive's headers are encrypted**, which is how this server writes every
encrypted archive (`-mhe=on`) and is what hides the file names. 7-Zip can also
encrypt an archive's *contents* while leaving its names readable. This server
never writes one, but an archive made elsewhere may be one, and it fails where a
truncated archive fails rather than where an encrypted one does — so when an
archive with readable names will not open and no passphrase was given, the
refusal names both possibilities instead of calling it corrupt. The header is
deliberately not searched for encryption of that kind: the AES method id sits
among the coders of an unencrypted header too, and hunting for those four bytes
in a header whose file names are arbitrary text would eventually call a good
archive encrypted and send somebody looking for a passphrase that never existed.

**An archive taken without a passphrase still restores without one**, and offering
one for it is not an error either: whoever restores does not have to know which
kind of archive they are holding.

**A restore is recorded** — the request, not what it replaces. One audit row is
written the moment one is asked for, by whoever asked, over COM or over REST:
object type `backup`, the archive's path, action `restore started`, and which
parts are being put back and whether the archive is an encrypted one. What the
restore then does is not recorded, because it runs on the maintenance work queue
where no actor exists to attribute it to; `docs/AlertsAndAuditTrail.md` says
which half is which and why.

One difference between the ways in, and it is about threads rather than archives.
`POST /api/v1/backup/restore` does its checking on one of four HTTP worker
threads, so the archiver run that reads the index is bounded at **two minutes**;
past that the route answers **504**, starts nothing and says that the archive was
not judged. Without the bound, an archive whose path leads to a drive that has
stopped answering would hold that thread for as long as the drive stayed that
way, and four such requests are the whole API. Nothing else has the bound or
needs it — the Control Panel over COM and the backup's own verification each run
on a thread whose only job is that one backup — so an archive too slow for the
route still restores from the Control Panel, or after being copied to a local
disk.

### What it changes about a backup run

* The verification extracts the archive with the passphrase, so what is verified
  is that the archive opens **with the passphrase this server holds** — not with
  one it used a moment ago.
* After the archive is written it is read back and its header checked: an archive
  that was meant to be encrypted and is not is discarded, not reported as a
  successful backup.
* A stored passphrase this machine cannot open — a database carried from another
  installation, a key file that has gone — **fails the backup**. An unencrypted
  copy of every mailbox is never written in place of an encrypted one.
* **Compression must be on** when messages are backed up. Without it the message
  files are copied into a `DataBackup` folder *beside* the archive, where nothing
  encrypts them, so that combination is refused before anything is written.
* **The staging moves off the destination.** Both halves of a backup are written
  to a file before they are archived — the index (`hMailServerBackup.xml`, which
  carries every account and the server's stored secrets) and a whole copy of the
  message files in a `DataBackup` folder, because 7-Zip is handed a directory.
  An unencrypted backup stages both in the destination; an encrypted one stages
  them in a directory of its own under `TempDirectory` and removes it on every
  way out, including the failures. Otherwise a plaintext copy of every mailbox
  would sit in the destination — the share, the cloud mount whose sync client
  uploads a file as it appears — for as long as the compression took, which is
  the exposure the encryption exists to prevent. The archive's contents are
  unchanged by this: 7-Zip names members from the last component of the path it
  is given, so they are `DataBackup\…` either way and a restore cannot tell.
* **So an encrypted backup needs room in the temp directory**, as much as the
  message store plus a little. It is checked before the copy starts and the run
  is refused with both numbers if it is not there. Point `TempDirectory` at a
  disk with room, or take the backup unencrypted deliberately. (The verified
  restore already needed the same room, and skipped itself without it; this
  refuses, because staging in the destination instead would be writing the mail
  where it was not to be written.)
* Not every archiver can encrypt: `7zr`, the reduced p7zip build, cannot, and
  fails the run. Install 7-Zip (`7zz`) or p7zip-full. The failure says so.
* Changing the passphrase does not touch the archives already written. Each
  archive needs the passphrase it was written with, which is a reason to write
  the passphrase and its date down together.

One thing encryption does **not** hide: the archive's own file name
(`HMBackup <date> <time>.7z`), its size, and the fact that a backup ran. That is
what a folder listing shows, and no archive format hides it.

And one place the passphrase still exists in the clear, said plainly because
"never in any read" is otherwise easy to over-read: **the server's own memory
while it is using it**, and so a process dump taken at that moment. The copies
are overwritten as soon as they are finished with — the bytes handed to 7-Zip go
the moment they are in the pipe, and the passphrase a backup or a restore was
given goes with the object that held it — but the settings store keeps the
value for the life of the process, as it does the smart host's password, and a
minidump written while a backup is running can contain it. Whoever can take a
dump of a service running as `LocalSystem` can read that server's secrets by
easier routes as well; this is a statement of what the feature does not claim,
not a way in.

And one thing it protects that is easy to miss: the settings section inside the
archive carries the server's own stored secrets in the clear — the smart host's
password among them — because a restore has to be able to put them back. An
unencrypted archive hands those to whoever can read the destination folder.


Restoring one mailbox, or one message
-------------------------------------

The whole restore above replaces the server. Since 22 September 2026 an archive
in the backup destination can also give back **one account's mailbox** — every
folder and message it held when the backup was taken — or **one message** of
it, and touch nothing else: not the account's settings, rules, contacts or
calendars (those are the account, and the whole restore is what brings them
back), and not any other account. It is the same operation in every interface,
`MailboxRestore`, and it runs the way the whole restore runs: checked on the
caller's thread and refused with the reason before anything is done, then
queued on the maintenance work queue under the backup manager's one run slot,
so that a mailbox restore, a whole restore and a backup never overlap, and
reported to the backup log.

| Where | How |
|---|---|
| REST | `GET /api/v1/backup/archives` (the archives), `GET /api/v1/backup/{archive}/mailboxes/{address}` (the account as the archive holds it), `POST .../restore` with `mode`, or `message` and `folder` (never both), and `GET /api/v1/backup/restore-mailbox/{job}` |
| Control Deck | The Backup view's **Restore a mailbox** and **Restore one message** cards |
| COM | `BackupManager.RestoreMailbox(file, address, mode, passphrase)`, `RestoreMessage(file, address, id, folder, passphrase)`, `DescribeMailbox(file, address, passphrase)` and `MailboxRestoreStatus(job)` |
| Control Panel | The Backup page's **Restore a mailbox** and **Restore one message** cards |
| hmctl | `hmctl backup archives`, `hmctl backup mailbox ARCHIVE ADDRESS`, `hmctl backup restore-mailbox ARCHIVE ADDRESS --merge\|--replace [--message ID [--folder F]]`, `hmctl backup restore-status JOB` |

**Which archives.** Only one inside the backup destination, and over REST,
the Deck and `hmctl` only by the **name** the listing answers
(`HMBackup YYYY-MM-DD HHMMSS.7z`) — a path cannot be given there at all, since
the route's path segment is not decoded for separators, and one that is tried
answers 404 as a name the destination lacks. Over COM the archive may also be
named by its path inside the destination; a path anywhere else is refused
there with the reason, whatever it points at: the whole restore takes any path
from an administrator who has decided to replace the server, but this is
reached from a mailbox picker, and a picker must not be a way to read files
the server was never told about. The archive must hold domains and messages
(`BackupDomains` and `BackupMessages` on when it was taken), its version is
checked as the whole restore checks it, and an encrypted one needs its
passphrase — in the request body, or in the `X-hMailServer-Backup-Passphrase`
header on the listing, which has no body. One archive at a time: there is no
picker across several archives for the version of a mailbox at a chosen moment;
the listing says when each archive was taken, and that is the choice.

**What the index may say.** The one thing read out of an archive that chooses a
path is a row's file name, and the index is a file in a directory more hands
write to than the server has administrators. So a file name is taken only in
the shape the executer writes — `{` + 36 hexadecimal characters and hyphens +
`}.eml`, no separator, no colon, no leading dot — and a row with any other
name is never listed, never staged and never written under the account's
directory: it is counted (`unrestorable` in the listing, `failed` in a whole
mailbox's outcome) and named in the backup log, the first ten.

**Merge and replace.** *Merge* creates each archived folder that is missing and
adds each archived message whose file is not in the live mailbox; nothing there
is removed. *Replace* empties every live folder of the account first and then
fills as merge would, so that the mailbox is the archive's — mail that arrived
since the backup is gone. The folders themselves stay, with their subscriptions
and rights; what replace removes is kept **only for `RecoverDeletedDays`, which
is off by default** (0 keeps nothing), and both confirm sentences say so.
Replace is refused up front, with 400, when the archive holds no messages for
the account — it would only empty the mailbox — and a replace whose fill could
not put every archived message back ends `failed`, with the counts in `error`
and the same report a failed backup makes, because the mailbox then holds less
than either the archive or the live mailbox did. A single message is merge of
one row, into the folder it was in or a named one, which is created if it is
missing; `mode` given together with `message` is refused with 400, since a mode
does not apply to one message.

**The identity rule.** The executer names every message file once, with a GUID,
and the same name in the live mailbox — in whatever folder — is the same
message. That is what "not in the live mailbox" means: a message the person has
moved since the backup is not brought back to its old folder, a message they
deleted is, and merging the same archive twice adds nothing the second time. A
restored message comes back with its flags, keywords and dates as the archive
holds them, a fresh UID in its folder (the archive's UIDs are not reused), its
size charged to the account as a delivery's is, and a session with the folder
selected is told the way delivery tells it. The full-text index picks the new
rows up on its own pass, nudged by the restore so that a search finds them in
seconds.

**What is refused, and what is counted.** The account must exist on this server
(404 if not — a mailbox is restored into the account it belongs to, and a
deleted account is the whole restore's job or an administrator's to make first;
restoring into a different account is not offered), the archive must hold it
(404), the message named must be in it (404), and a second restore while a
backup or restore runs is 409 — the job the request registered is then marked
`failed` with that reason, so that no status ever reads `queued` for something
that will not run. A message whose file the archive does not hold — deleted
from the store while the backup ran — is counted as *failed* and named in the
backup log (the first ten), and a merge goes on: one missing file is not a
reason to leave the other thousand out. The outcome — `folders`, `added`,
`skipped` (the same file name was still there) and `failed` — is read from the
job's status route, from `MailboxRestoreStatus` over COM, and from the backup
log; the last fifty jobs are kept for the life of the process. A job that
cannot complete at all, and a replace with anything in `failed`, fails the way
a whole restore fails: a `BACKUP ERROR` line, a critical error, the
backup-failed alert and the `OnBackupFailed` event.

**Who.** The roles that may restore the server may restore a mailbox from one
of its archives (`manage_server`); reading an account's messages out of an
archive — senders, dates and subjects — is reading the mailbox as it was
(`view_mailbox`), which an auditor may do and a help desk may not. All four
routes are server-wide and refused to a credential confined to named domains.
The request is recorded on the audit trail as the whole restore's is — object
type `backup`, the archive's path, action `mailbox restore started`, and a
sentence naming the account and the mode — by whoever asked, over COM or REST;
what the restore then does runs with no actor on its thread and is not.

**What the listing costs.** The archive index carries a message's file name,
sender, date, size and flags, and not its subject. The listing reads subjects
from the message files, staging them out of the archive to do it — the listed
messages' files, named to the archiver one by one, and never the account's
directory or the store — for a listing of at most 500 messages; a larger
mailbox is listed without subjects, the answer says so, and `?folder=` narrows
both the listing and what is staged to one folder. A restore stages the same
way: the files of the messages it will put back, and nothing beside them. On
the REST route the archiver runs under the same two-minute bound as the whole
restore's check.

**Not in this.** An archive taken with `BackupMessagesDBOnly` holds rows and no
files, and a mailbox cannot be restored from rows alone; the job says so and
fails. A public folder is not an account's mailbox and is not restored here.
Nothing here recreates an account, a domain, or an account's settings, rules,
contacts or calendars: that is the whole restore.

Versions
--------

An archive from an **older** hMailServer restores. What that version did not
write is simply absent, and the restore does not complain about it: an archive
from before this change carries no per-account stores and no keywords, and
restores exactly as it always did.

An archive from a **newer** hMailServer is refused, with a sentence naming both
versions and telling you to upgrade first. There is deliberately no override: a
newer archive can carry properties this build has no column for, and those would
be dropped in silence while the restore reported success.

An archive that names a table this build does not have — which can only be a
hand-edited one, or one from a build whose version string cannot be parsed — has
those rows counted and named in the backup log, and the rest of the restore goes
ahead. Refusing a whole restore over rows nothing here could use would cost you
your mail to protect something you cannot have.


Adding a per-account table
--------------------------

For anyone working on the server. Everything that holds one account's data is
declared in
`hmailserver/source/Server/Common/Application/AccountStores.cpp`, in one of two
arrays:

* `stores_` and `columns_` — the table, and every one of its columns with the
  role it plays. The backup reads it through that declaration and the restore
  writes it back through the same one.
* `elsewhere_` — the table, and the reason it is not carried there: a business
  object already writes it, or it is derived, or it is in flight.

`build/check-account-stores.py` fails the build when a table that reaches
`hm_accounts` through `ON DELETE CASCADE`, or that has an account-id column, is
in neither — and when a column of a declared store is missing, when the three
`CREATE TABLE` scripts disagree about one, or when a store has no test in
`hmailserver/test/RegressionTests/Infrastructure/BackupAccountStores.cs`.

The test shape is the same for every store: make the data through the surface
that really writes it, take a backup, delete the whole domain the way a disaster
would, restore, and find the data exactly as it was.


If you are running 6.3.3 or earlier
-----------------------------------

The seven per-account tables above — the address book, the webmail's settings,
scheduled sends, file links, S/MIME keys, calendars and the password history —
and a message's keywords were in no archive those releases wrote, and a restore
deletes them. Until you are on a release that carries them:

1. **Back up the database as well**, with your database server's own tool:
   `pg_dump`, `mysqldump`, SQL Server's backup, or a copy of `hMailServer.sdf`
   with the service stopped.
2. **Restore from that** when what you are recovering is a whole server, and use
   the built-in restore for what it was good at: moving domains, accounts and the
   message store between installations.
3. If you restore from an archive those releases wrote, expect users to find
   their address book, labels and webmail settings gone, and tell them before
   they find out themselves. An archive written by an older release does not gain
   what it never held by being restored onto a newer server.
