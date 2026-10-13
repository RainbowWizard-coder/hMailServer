Secrets at rest
===============

Every secret this server keeps, where it is kept and how. A secret the server has to
check a password against is kept as a salted, key-stretched hash. A secret it has to use
again - to sign, to sign in somewhere else, to verify a one-time code - is kept sealed:
the ones that live in one machine's `hMailServer.ini` or belong to that machine under
the machine's own secret store (DPAPI on Windows, `DPAPI:`; on Linux AES-256-GCM under
the key file `<data directory>/.hmailserver-secret-key`, `LINUX1:`), and the ones that
live in the shared database and must open on every node that uses it under the
installation's **stored-secrets key** (`HMK1:`, below) - once the installation has one.

Until 25 September 2026 three kinds of secret were kept as written, and the evidence pack
said so: the administrator password as the Windows installer wrote it, an account's second
factor, and the `[Settings]` keys that hold secrets. This page is what changed for each,
and a list of what the same sweep found still kept as written, each with its reason.


Where each secret is
--------------------

| Secret | Where | How it is kept |
|---|---|---|
| Account passwords, app passwords, recovery codes | `hm_accounts`, `hm_apppasswords`, `hm_recoverycodes` | A salted hash, PBKDF2 by default (`PreferredHashAlgorithm`), re-hashed at the next logon when it is weaker than the setting |
| **The administrator password** | `[Security] AdministratorPassword` in `hMailServer.ini` | PBKDF2. An older MD5 or SHA-256 value is written again as PBKDF2 at its next sign-in, and the Windows installer no longer writes MD5 (*The administrator password*, below) |
| The administrator's second factor | `[Security] AdministratorTotpSecret` in `hMailServer.ini` | The machine's secret store |
| **The stored-secrets key** | `[Security] StoredSecretsKey` in `hMailServer.ini`, and the files exported from it | The machine's secret store. Its id - never the key - is recorded in the database (`hm_settings`, `StoredSecretsKeyId`); the key is never in the database and never in a backup archive (*The stored-secrets key*, below) |
| **An account's second factor** | `hm_accounts.accounttotpsecret` | Sealed under the stored-secrets key, once the installation has one; as generated before that (*An account's second factor*, below) |
| **`SRSSecret`, `BATVSecret`, `OAuth2HmacSecret`, `OAuth2IntrospectionClientSecret`, `OutboundOAuth2ClientSecret`, `OutboundOAuth2FixedToken`, `MetricsServerAuthToken`, `MetricsServerAuthPassword`, `ServiceAccountPassword`, `GssapiServicePassword`** | `hm_inisettings` and the `[Settings]` section of `hMailServer.ini` | Sealed under the stored-secrets key, in the row and in the file's line, once the installation has one (*The `[Settings]` secrets*, below) |
| `PasswordPepper` | `[Settings]` in `hMailServer.ini`, and nowhere else | As written, on purpose (*What is still kept as written*) |
| The database password | `[Database] Password` in `hMailServer.ini` | The machine's secret store (`Passwordencryption` 6) when the server or the setup tool writes it; the Linux package's template reads a password typed with `Passwordencryption=0` as plain text |
| The smart host's password, the Web Push key, the backup passphrase, the import and single sign-on client secrets, the sandbox token | `hm_settings` | The machine's secret store (`PropertySet::IsCryptedProperty_`), so they do not open on a second machine ([WarmStandby.md](WarmStandby.md)) |
| Route, relaying-domain and fetch-account passwords and tokens, certificate key passphrases, webhook and inbound-webhook secrets, migration source credentials, an organisation CA's key | Their tables | The machine's secret store (`Crypt::ProtectSecret`), likewise |
| REST API keys | `hMailServerApiKeys.ini` | A SHA-256 digest of the token |
| The Control Panel's own second factor | `HKLM\SOFTWARE\hMailServer` `AdminTotpSecret` | DPAPI, machine scope |


A password outside Latin-1
--------------------------

**What a password hash is made over.** The password's UTF-8, every character of it, for
every scheme (MD5, salted SHA-256, PBKDF2, Argon2id, scrypt) and for the pepper
(`Crypt::EnCrypt`). Until 6.3.4 it was the password through `AnsiString(String)`, which
is not an encoding of it: on Windows it made the first character above U+00FF, and every
byte after it, a NUL byte, and on Linux it made each such character `?`. So a password
of Cyrillic, Greek or CJK letters was stored as the hash of its length - and any other
password of that length and shape, sent over SASL PLAIN (whose fields are UTF-8), opened
the account; on Windows so did the NUL bytes themselves, in a REST API Basic credential.
For ASCII the bytes are the ones they were, so an ASCII password's hash is unchanged, and
PBKDF2 over UTF-8 is also what a SCRAM-SHA-256 client derives its key from, so such a
password now serves SCRAM too. A password that reaches the server as its UTF-8 widened one
byte to a character - IMAP's `LOGIN`, the REST API's Basic credential - is hashed as those
bytes (`Unicode::ToUtf8Canonical`), which is what the old conversion hashed as well; so it
and the same password decoded (SASL PLAIN, COM, a REST body) are one password. The server
cannot tell that widened form from text that is the same characters, so the reverse holds
too: `pÃ¤sswÃ¶rd`, the UTF-8 of `pässwörd` read one byte to a character, given as text
opens the account whose password is `pässwörd`, and set as a password it sets that one. At a
sign-in that gives nobody anything: the form is made from the password's own bytes, so only
someone who knows the password can type it. The password policy and the breach check judge
the characters they are handed, not the bytes a hash is made over, so every place that sets
a password hands them the characters the client sent - a REST body, the help desk's reset
(`POST /api/v1/accounts/{address}/password`) among them, is read as UTF-8, and COM and the
Control Panel give text - and `pässwörd` sent to any of them is counted and looked up as
`pässwörd`. Typed as `pÃ¤sswÃ¶rd`, it is counted and looked up as those ten characters,
although what it sets is `pässwörd`.

**What a sign-in checks** (`Crypt::Validate`): the password's UTF-8 as above, then - for
Latin-1 text, every character below U+0100 and the bytes not UTF-8 - its characters as
bytes, which is what the old conversion made of such a password (`passwörd`) exactly, so
those hashes still open. For a password that arrives as its UTF-8 widened (IMAP `LOGIN`,
POP3 `PASS`, REST Basic) that second check is of the text it decodes to, so a client that
sends UTF-8 opens such a hash too, which it never did. A sign-in that matched only that
way is followed by a re-hash from the password decoded, under the scheme the hash has (or
the preferred one, if that is an upgrade), which moves the hash to UTF-8 - after which it
serves SCRAM-SHA-256 as well - and the application log says `Re-derived the stored password
hash for account <address> ... over the password's UTF-8`. A password holding U+0000 is
refused.

**What changes on upgrade.** A hash stored before 6.3.4 of a password with a character
above U+00FF is the hash of NUL bytes or question marks, which the server cannot tell
apart from the hash of another password: it is **refused**, to its own password too, and
the application log says `Authentication refused for account <address>: its stored
password hash was made before 6.3.4 from a password with a character above U+00FF...`
(for the administrator, `Administrator logon refused: the stored administrator password
hash was made before 6.3.4...`). An administrator sets such a password again - on the
Control Panel, over COM or the REST API, and with `hMailServer.exe --set-admin-password`
for the administrator's own; the webmail's password change asks for the old password
and is refused as a sign-in is - and it is hashed whole. On Linux one such hash still opens to the password with `?` in place of
each of those characters, which is an ASCII password the server cannot refuse. On both
platforms a PBKDF2 one still opens over SCRAM-SHA-256: a SCRAM client derives its key from
the password itself, so one that derives it from the NUL bytes (Windows) or question marks
(Linux) the hash was made over proves knowledge of it, and the server, which never sees the
password in SCRAM, cannot tell. Set those passwords again, soon after the upgrade; the log
line above names each account as its user tries to sign in, whether the client sends the
password as characters or as UTF-8. A pepper (`PasswordPepper`) outside ASCII is keyed as
UTF-8 for a new hash and, for a hash made before, also as it was then, so an Argon2id or
scrypt account signs in as before, and that sign-in makes its hash again under the
pepper's UTF-8.


The administrator password
--------------------------

**Where it lives.** `[Security] AdministratorPassword` in `hMailServer.ini` - beside
`hMailServer.exe` on Windows (`<install folder>\Bin\hMailServer.ini`), in
`/etc/hmailserver/hMailServer.ini` on Linux. Not in the database, because it is how an
administrator gets in when there is none, and not in a backup ([Backup.md](Backup.md)).
It stays there; nothing about an existing installation moves.

**How it is written.** As PBKDF2 (`$h1$<iterations>$<salt>$<key>`, the iterations
`PasswordHashIterations` asks for), by the Control Panel's *Administrative access* page,
COM's `Settings.SetAdministratorPassword`, `hmailserver --set-admin-password` on Linux,
and since this row `hMailServer.exe --set-admin-password` on Windows.

**What the Windows installer did, and does.** Until 25 September 2026 it wrote the
password its page or `/adminpassword` collected as `GetMD5OfString` of it: an unsalted MD5
digest, which the server accepted and nothing ever re-hashed. It no longer writes the key
at all. After the program files are in place, and before the database step - which signs
in with the password and starts the service - it runs `hMailServer.exe
--set-admin-password`, and the server writes PBKDF2. The password reaches the verb in the
installer's environment, `HM_ADMIN_PASSWORD`, which the verb inherits and which is emptied
as soon as it has finished; never on a command line, where anybody listing processes could
read it (Inno Setup can start a process but not write to its standard input). As before,
only when a password was collected and the file has none: an upgrade never changes the
administrator password. If the verb fails - or reports success and the file still has no
line - the installer says so, reports a failed installation, and the server is left with
no administrator password: set one on the Control Panel's *Administrative access* page
before the server is used.

The installer hands the password it collected to the database tools the same way: in
`HM_ADMIN_PASSWORD`, set for `DBSetupQuick.exe` (or `DBSetup.exe`, for an external
database) and emptied as soon as it has finished, and DBSetupQuick hands it on to
`DBUpdater.exe` in the same variable. Until 26 September 2026 it was
`DBSetupQuick.exe password:<password>` on their command lines, unquoted, so a password
with a space, a double quote or backslashes before one reached them changed and the
database step failed. An unattended install can give the installer the password in
`HM_ADMIN_PASSWORD` too, set in an elevated prompt, in place of `/adminpassword=`,
which is on the installer's own command line and cannot carry a double quote.
`password:<password>` still works for an administrator who runs `DBSetupQuick.exe` or
`DBUpdater.exe` by hand.

**`hMailServer.exe --set-admin-password`** takes the password from `HM_ADMIN_PASSWORD`, or,
when that is not set, from one line of standard input when that is a pipe or a file, and
never from an argument:

```powershell
# From an elevated prompt, beside hMailServer.exe:
$env:HM_ADMIN_PASSWORD = Read-Host -Prompt 'New administrator password'
.\hMailServer.exe --set-admin-password
Remove-Item Env:HM_ADMIN_PASSWORD
```

The environment variable carries the password as typed, whatever its characters. A pipe
does not in Windows PowerShell 5.1, which sends text to a program in ASCII and turns every
other character into `?` - the verb would then store a password nobody typed - so a pipe is
for a password in plain ASCII, or for PowerShell 7 with `$OutputEncoding` set to UTF-8.

It opens no database. A running service goes on with the password it loaded until it is
restarted or reinitialised. Exit 0 when the file holds a PBKDF2 hash of the password, 2 when
no password was given, 1 when the file could not be written.

**An installation that already has MD5.** The server reads three kinds of value: PBKDF2,
the salted SHA-256 of older versions (70 characters) and MD5 (any 32-character value). A
successful administrator sign-in - COM's `Authenticate` and `AuthenticateWithCode`, and the
REST API's Basic credential, which is also the Control Deck's sign-in - that verifies an
MD5 or SHA-256 value, or a PBKDF2 hash of fewer iterations than `PasswordHashIterations`
asks for now, writes the password again as PBKDF2, after the second factor has been
checked, and says so in the application log. `IniFileSettings::UpgradeAdministratorPasswordHash`.

It writes only when the value the password verified against is still the one in force
**and still the one in the file**, read from the file just before the write. The verb runs
beside the service, and so does an editor: a new password written into the file while the
service still verifies the old one must not be written over by the next sign-in with the old
one, or a password reset because the old one leaked would be undone by whoever holds it.

Two consequences, both once. The REST API stamps an administrator's browser session with
the hash in force (`RestApiServer.cpp`, *credential_stamp*), so a session started before the
re-hash - by a passkey or a single sign-on, which do not re-hash - ends at its next request
and signs in again. And a server that cannot write its own file, as the Linux package's is
not (root's, mode 0640), keeps the old value, says so once a process (`HM7494`) and does not
try again: run `sudo hmailserver --set-admin-password` there.


The stored-secrets key
----------------------

**Why a key of the installation's and not the machine's.** An account's second factor and
the `[Settings]` secrets live in the database, and more than one machine may use a database:
a warm standby shares it ([WarmStandby.md](WarmStandby.md),
[HighAvailabilityRunbook.md](HighAvailabilityRunbook.md)), a move to new hardware restores
it, a failback returns to it. Sealed under one machine's DPAPI, they would open on that
machine only, and every other node would refuse every second factor. So they are sealed
under a key every node holds - 32 random bytes - and each node keeps that key in its own
`hMailServer.ini`, `[Security] StoredSecretsKey`, under its own DPAPI (or the Linux key
file). The key is never in the database and never in a backup archive: a copy of the
database, or of an archive, holds the secrets sealed and not the key that opens them.

**The sealed form** is one line, `HMK1:<key id>:<base64>`: AES-256-GCM with a fresh 12-byte
nonce and the 16-byte tag, the associated data naming the format, the key id and what the
value is - the setting's name, or for a second factor the column and the account's id - so
that a value copied into another setting's place, or into another account's row, does not
open there. That binding keeps an envelope where it was made; it does not make the column
tamper-proof, since anybody who can write the database can still store a second factor as
generated, which is read as it is. The key id is the first 16 hex digits of SHA-256 over a
label and the key; it names the key without giving any of it away. A 32-character second
factor seals to about 100 characters. `Util/StoredSecrets.h`.

### Who makes it

Never the service. The service may run as an account with no write on `hMailServer.ini`
(`NT SERVICE\hMailServer`, the packaged Linux user), and a key made by whichever node
happened to start first is how two nodes came to seal one database under two keys. A key is
made by an elevated step only:

* **the Windows installer, on a new installation**, after the database step has created the
  database and before it starts the service (`hMailServer.exe --create-stored-secrets-key`),
  and it then says to export the key - and, when that database step did not complete,
  setup run again or `DBSetup.exe` run as an administrator, either of which finishes the
  new installation the same way ([ServiceAccount.md](ServiceAccount.md), "A new
  installation whose database step did not complete");
* **`hMailServer.exe --create-stored-secrets-key`** from an elevated prompt, or **`sudo
  hmailserver --create-stored-secrets-key`** on Linux - how an installation upgraded from
  before 25 September 2026 turns sealing on, and how the packaged Linux server gets its key;
* **`hmailserver --create-database`** on Linux, when it creates the schema and can write the
  configuration - the container's entrypoint, whose `/etc/hmailserver` is a volume the
  container's user owns - unless the configuration sets `ProtectStoredSecretsWithDPAPI=0`.
  Run by the package as the service's user, it cannot, and says what to run instead.

**An upgrade gets no key by itself.** Without one the server behaves as it did before this
row: second factors and `[Settings]` secrets are stored as written, one application-log line
at each start says sealing is off, and the setup checklist's `stored_secrets_key` item
offers the verb ([SetupChecklist.md](SetupChecklist.md)). To turn sealing on, make the key
on one node, export it, import it on every other node that shares the database, and restart
each; the next start seals what is stored as written.

### The database records it

Making the key records its id in the database in the same step - the `hm_settings` row
`StoredSecretsKeyId` - the key line written into the file and flushed to the disk first,
then the record, and the line taken out again if the record cannot be made. The database is
the arbiter from then on:

* a second key is refused where one is recorded, or where the database holds values sealed
  under one, with the import named - a node never makes a key of its own beside the
  database's;
* an import is checked against the record and refused when it carries another key;
* at every start (`StoredSecrets::EnsureAtStart`) a node judges its key against the record
  and **writes nothing into its file**: a node with no key on a database that records one, a
  key line that does not open here, or a key that is not the recorded one, seals nothing and
  says so at each start (`HM7495`), naming the import and - for a key that is lost - the way
  forward below. Until then every second factor sealed under the database's key refuses
  every code there (an account keeps it, and signs in with an app password or a recovery
  code), and those settings read as not set;
* a node holding a key, on a database that records none and holds nothing sealed under
  another - a dump restored from before the key was made - records its key there.

The record and the export record are the server's bookkeeping, as a profile's records are:
never a property the interfaces show, and never carried by a backup archive - restored
elsewhere, this installation's record would stop that one sealing anything. A verb that
seals in its own process - `--set-setting`, `--apply-environment`, `--apply-profile` -
checks its key against the record the first time it seals; where it is not the database's,
or the database cannot be read, the verb stores the value as written, which every node
reads, and says so once (`HM7496`) - on its standard error when it runs on Windows, or as root
on Linux, since neither writes the logs ([ServiceAccount.md](ServiceAccount.md), "What an
administrator's command writes") - and a start of the service seals it where the key is right.

### Keep a copy

The key exists on the machines that hold it and in the files exported from them, and
nowhere else. The product's own backup archive does not need it: it carries these secrets
opened, and a restore seals them under the restoring server's key ([Backup.md](Backup.md)).
**A database-level backup does** - a MySQL or PostgreSQL dump, a SQL Server backup, a copy
of the `.sdf`: restored anywhere without the key, it brings back every second factor and
`[Settings]` secret unopenable. A copy of `hMailServer.ini` is no copy of the key on Windows,
since its line is sealed under that machine's DPAPI. So export the key as soon as it is made,
on a single server as much as on a pair, and keep the file with the database backups, stored
apart from them as the password pepper is kept:

```powershell
# From an elevated prompt beside hMailServer.exe:
.\hMailServer.exe --export-stored-secrets-key D:\Escrow\hmailserver-stored-secrets.key
```

The export is recorded in the database (`StoredSecretsKeyExported`), and the setup checklist's
`stored_secrets_key` item stays open until it has been. On Linux the key is carried by the
exported file too, or by backing up the configuration together with
`<data directory>/.hmailserver-secret-key`.

### Carrying it to another node

A standby, a new machine, a second node after an upgrade:

```powershell
# On a node that holds it, from an elevated prompt beside hMailServer.exe:
.\hMailServer.exe --export-stored-secrets-key C:\Keys\stored-secrets.key

# On the other node, the same way, with the file carried across:
.\hMailServer.exe --import-stored-secrets-key C:\Keys\stored-secrets.key
Restart-Service hMailServer
```

On Linux, `sudo hmailserver --export-stored-secrets-key FILE` and `sudo hmailserver
--import-stored-secrets-key FILE`, then `sudo systemctl restart hmailserver`.

The export writes a new file - never over one that exists: the create itself refuses an
existing name, with nothing deleted first - readable by SYSTEM and the administrators only on
Windows and mode 0600 on Linux, and prints the key id, never the key. On Linux the file is
written through the descriptor it was made with and linked to its name, which never replaces
or follows anything there. The file is three lines: the format, the id and the key in
base64, and it opens every second factor and `[Settings]` secret the database holds. It is
refused when this node's key is not the one the database records. The import checks the id
against the key and against the database's record, refuses a file that is not one, and
refuses when the node already holds another key that opens - replacing that is removing its
`[Security] StoredSecretsKey` line first, deliberately, since what was sealed under it stops
opening there. Importing the key the node already holds changes nothing. Never copy the
`[Security] StoredSecretsKey` line from one machine's file to another's: it is sealed under
the first machine's own DPAPI and opens nowhere else.

The import takes the file as the export wrote it or as an editor saved it again: LF or CRLF
line ends, with or without a UTF-8 byte order mark, or UTF-16LE with its mark, which is what
Windows PowerShell 5's `Out-File` and `>` write. A file larger than 64 KiB is refused, since
no key file is that large, and one that cannot be read says so. Until 6.3.7 a file with a
UTF-8 mark was refused as not a key file, and so was a file that could not be read.

Neither verb leaves a copy of the key in the memory it gives back to the heap. The import
reads the file with the operating system's own reads into one buffer, and that buffer and
every copy made of the key on its way to the line under the machine's store - its base64,
its bytes, its hex, the line, and the line opened again to read the key back - are written
over (`OPENSSL_cleanse`) before they are freed, an error or an exception on the way included;
the export does the same with the file's text. Until 6.3.7 the import read the file through
the C runtime and the server's text-file reader, and their buffers and the copies of the
whole file made on the way to the one that was written over - the key's line in each - went
back to the heap as they stood. And on Windows, and in a build made with GCC, the line opened
by the machine's store was copied on its way out of `Crypt::DeCrypt`, and the copy's source -
the key in hex - freed as it stood: at every start, and whenever a verb read the key. On
Linux the scratch buffer in which a conversion of the line to or from UTF-8 is measured, on
the stack, is written over as the measure ends too.

A node set up on a new, empty database of its own before it was pointed at the shared one
holds a key of its own: the shared database refuses it at the next start (`HM7495`). Remove
its `[Security] StoredSecretsKey` line, then import the database's key.

### Who can open it

**On Windows** the key line is sealed with machine-scope DPAPI, as every other `DPAPI:` value
on this page is: any process on the machine that can read `hMailServer.ini` can open it, and
with read access to the database, every second factor and `[Settings]` secret. What guards
the key from the machine's other local accounts is the ACL of `hMailServer.ini` - which
under `Program Files` lets `BUILTIN\Users` read by inheritance. The key protects the
secrets from a copy of the database or of its backups that leaves without the file, not from
a local account on the server. Where local users are not trusted, take `BUILTIN\Users`' read
off `<install folder>\Bin`, leaving SYSTEM, the administrators and the service's account.

**On Linux** the line is sealed under the key file, which is mode 0600 and the service's: the
root-run verbs make that file for the owner of the data directory - the package's
`hmailserver` - through the descriptor they made it with, and never touch it by its path,
since the data directory is the service's and a symbolic link planted at that name would
otherwise send root's `chown`, and the key, wherever it pointed. A key file that exists must
be the service's already, a data directory root owns is refused, and the file is refused
unless it is a plain file with one name.

### When the key is lost

**Import it first.** A node that says `HM7495` because it lacks the key - a standby that has
not had it imported, a new machine, a node whose key file went missing - is mended by
carrying the key from a node that holds it, or from the file exported when it was made, or by
putting its key file back. Nothing is lost while any node or any export holds the key.

Only when the key is gone for good - the only machine that held it is gone and no export was
kept - is what it sealed beyond opening by anybody. The way forward, with the service
stopped, names the lost key by its id: the sixteen hex digits `HM7495`, `HM7492`, `HM7493`
and the setup checklist give.

```powershell
.\hMailServer.exe --forget-sealed-secrets 0123456789abcdef   # sudo hmailserver --forget-sealed-secrets <id> on Linux
.\hMailServer.exe --create-stored-secrets-key
.\hMailServer.exe --export-stored-secrets-key D:\Escrow\hmailserver-stored-secrets.key
```

`--forget-sealed-secrets KEYID` clears every envelope that names that key and nothing else:
each account's second factor sealed under it is removed, so that the account signs in with its
password and enrols again; each `[Settings]` secret sealed under it is emptied, in the row and
the file, so that it is entered again. The database's record, and the export's, are removed
only where they name that key; no other key is recorded by the verb (a node that holds one
records it at its next start, as on a database restored from before the key). It refuses
without an id; where this node's own key line does not open - its key file missing or of the
wrong mode, or a line copied from another machine - since it may be the very key named; and
where this node holds the key named, since nothing sealed under a key that is held is lost. It
refuses while the service runs (exit 4; on Linux by the lock the running server holds). It
prints one line for every item, and all of them again in one entry (`HM7497`): on its standard
error on Windows and as root on Linux, where a command an administrator runs writes nothing to
the logs, and into the error log when the service's own user runs it on Linux. Every value it
clears is gone.

An account's second factor
--------------------------

**How it is kept.** `hm_accounts.accounttotpsecret` holds the secret sealed under the
stored-secrets key, with the account's id in the associated data. The account object the
server works with holds it opened, so everything that verifies a code - COM's
`AuthenticateWithCode`, the webmail's sign-in, a recovery - is as it was. A new account's
secret, which has no id to be sealed with until the insert, goes in as generated and is
sealed by an update once it has one. Schema step 6102 to 6103 widens the column from 255
characters to 1024, the width the route, relay and fetch-account secrets have, so that a
longer secret or a longer envelope never meets it. `PersistentAccount::TotpSecretToStore_`
and `TotpSecretFromStore_`.

**The secrets already stored.** A row holding a secret as generated is read as it is. At
every start of a node that holds the database's key, before a listener is open, the server
seals the rows still in that state - all of them, the first start after the key is made, and
none after that (`PersistentAccount::ProtectStoredTotpSecrets`: one select, which finds no row
once they are done) - and an account saved for any reason is saved sealed. Nothing is sealed
on a node that does not hold the database's key, nor on an installation that has none.

**One that does not open here.** The account keeps its second factor and every code is
refused - never a second factor quietly switched off, which would let the password alone in
- and the error log says so once a process (`HM7493`), naming the first account met, the key
it was sealed under, the verbs that carry that key here and the one that clears it when the
key is lost. It still signs in with an app password or a recovery code.

**Backup and restore.** The archive carries the secret opened, as it carries the server's
other stored secrets, and the restore seals it again under the restoring server's key: an
archive restores onto any server, whatever key it holds. So an archive that is not encrypted
holds every account's second factor in the clear, beside its password hash: set a backup
passphrase ([Backup.md](Backup.md)). A secret whose envelope did not open when the backup was
taken is carried as its envelope, which is short enough for an older server's column.


The `[Settings]` secrets
------------------------

**Which.** Every `[Settings]` key whose value is a secret by its name - the rule
`GET /api/v1/settings/ini` masks a value by (`IniSettingStore::IsSecretName`) - except
`PasswordPepper` and a key the server reads as a number. Over the keys this server reads
that is `SRSSecret`, `BATVSecret`, `OAuth2HmacSecret`, `OAuth2IntrospectionClientSecret`,
`OutboundOAuth2ClientSecret`, `OutboundOAuth2FixedToken`, `MetricsServerAuthToken`,
`MetricsServerAuthPassword` and `ServiceAccountPassword`, and `GssapiServicePassword`, which
is read by name. The same rule decides both, so a secret setting added later is sealed
without anybody remembering to add it, and what the lists mask and what the store seals
cannot drift apart. `IniSettingStore::IsProtectedName`.

**How it is kept.** The row in `hm_inisettings` and the line in the `[Settings]` section -
which since schema 6042 is the server's copy of the row ([SettingsApply.md](SettingsApply.md))
- hold the same envelope. Everything that reads a `[Settings]` value reads it opened: the
settings loader, COM's `GetIniSetting`, `GET /api/v1/settings/ini/{name}`, the pending list,
and whatever reads the file's copy with no database open - the bootstrap and a server whose
table cannot be read (`IniFileSettings::ResolveString_`). `hMailServer.exe /Register` loads no
settings since 6.3.6, when it stopped writing the error log, and reads no
`ServiceAccountPassword`: it takes the service's account from its command line alone
([ServiceAccount.md](ServiceAccount.md#register-takes-the-account-from-its-command-line)). A save of the value already stored keeps
its envelope, so a settings page that writes every field it shows does not rewrite every
secret on it; an envelope written back as it is - a profile's way back, a test putting back
what it read - is stored as it is. A value forced from `[SettingsOverride]` is read as
written, since that section is the administrator's own and the server never writes it.

**The values already stored.** The first start of a node holding the database's key seals
each value still as written, the row first and then the line, and the application log names
them once the logger is configured (`IniSettingStore::Synchronize`, step 3, through
`StoredSecrets::Note`). A line the service cannot write - the Linux
package's file is root's - leaves the row sealed and the line as it was, which read the same,
and `HM5803` names the setting at each start until `sudo hmailserver --set-setting <name>`
writes it.

**The length.** An envelope has to fit the 4,000 characters a setting may have, which
leaves about 2,900 for the secret. A longer one - a long bearer token in
`OutboundOAuth2FixedToken`, say - is stored as written, as it was before, and said once a
process when it is saved (`HM7491`), and in the application log at each start.

**One that does not open here.** The server runs as though it were not set, and the error
log names it, once a process for each setting (`HM7492`), with the verbs that carry the key
here and the one that clears it when the key is lost. For the metrics listener that means no credential: on a loopback address `/metrics`
then answers without one, and on any other address it answers 503.


ProtectStoredSecretsWithDPAPI
-----------------------------

The switch keeps its name and its default, 1, and now governs both kinds of stored secret:

* the machine-bound ones it always governed - at 0 they are written with the legacy
  Blowfish scheme, whose key is in the source, as before;
* the sealed ones of this page - at 0 nothing is sealed, a value saved is stored as
  written, and **the next start opens what was sealed and stores it as written again**, in
  the row and in the file, second factors included, wherever this node holds the key. At 1
  again, the next start seals them again. So switching it off undoes this row rather than
  leaving values nobody can open.

It is read as the settings store holds it - its `[SettingsOverride]` line, its row, and only
then this machine's copy in the file - so that a node whose own file lacks the line still
does what the shared row says. With the stored-secrets key carried to every node there is no
reason to turn it off for a pair; it is the way back for somebody who wants the secrets as
written. At 0 the setup checklist's `stored_secrets_key` item is not needed. The key and its
record stay as they are: switched on again, the same key seals again.


Rolling back
------------

An older server does not know `HMK1:` envelopes. Rolling back restores the pre-upgrade
database, whose rows are as written, but the `[Settings]` lines of each machine's
`hMailServer.ini` stay sealed until that older server's first reconcile puts the rows back
into the file - and its `/Register`, which the older installer runs, reads
`ServiceAccountPassword` from the file before that (up to 6.3.3 it did; this version's does
not), hands Windows the envelope as the
password, and the service cannot log on. Restore the pre-upgrade `hMailServer.ini` with the
database, or set `ServiceAccountPassword` again once the older version runs and register the
service again ([Upgrading.md](Upgrading.md#rolling-back)).


What is still kept as written
-----------------------------

What the sweep of the create scripts and the settings found beside the three above, and why
each stays as it is.

| What | Where | Why |
|---|---|---|
| `PasswordPepper` | `[Settings]` in `hMailServer.ini` only | It is never in the database or a backup, which is what sealing would protect it from, and the way a restore onto new hardware gets it back is its value copied into that machine's file ([Backup.md](Backup.md)): a machine-bound envelope there would be a pepper that opens nowhere, and every Argon2id and scrypt account would stop signing in |
| The links in files sent as links, messages that expire, password-protected messages, quarantine digests and a list's member pages | `hm_files.filetoken`, `hm_confidential.cftoken`, `hm_protectedmessages.pmtoken`, `hm_digesttokens.dttoken`, `hm_distributionlistsrecipients.distributionlistrecipienttoken` | Bearer tokens the server finds a row by, so an envelope, which differs every time it is made, cannot be the key; three of them also name a file on disk. Kept as a digest, as a recovery token is (`rttoken`), is a change of its own |
| An alert rule's signing secret, a managed list's link secret | `hm_alertrules.alertrulesecret`, `hm_distributionlists.distributionlistsecret` | HMAC keys the server reuses, in columns of 255 and 64 characters. The same treatment as the second factor, left for a row of its own |
| A Web Push subscription's `auth` secret | `hm_pushsubscriptions.pskeyauth` | The browser's key for encrypting to it, in a 32-character column; the same |
| Private keys a browser wrapped | `hm_pgpring.pgrsecret`, `hm_zeroaccess.zasecret`, `hm_smimekeys.smimekey` | Encrypted in the page under a passphrase this server never sees |
| A mailbox key's password-wrapped half | `hm_accountkeys.akpassword` | Wrapped by the account's password (Argon2id, AES-256-GCM) - [EncryptionAtRest.md](EncryptionAtRest.md) |
| The administrator password given to the installer with `/adminpassword=` | The installer's command line | In transit rather than at rest, on the command line while setup runs. `HM_ADMIN_PASSWORD` keeps it off (*What the Windows installer did, and does*); the installer no longer puts it on the database tools' |


Errors
------

| Code | Said when |
|---|---|
| `HM7491` | A `[Settings]` secret whose sealed form would be longer than 4,000 characters, stored as written (once a process for each setting) |
| `HM7492` | A `[Settings]` secret sealed under a key this node does not hold (once a process for each setting) |
| `HM7493` | An account's second factor sealed under a key this node does not hold (once a process) |
| `HM7494` | The administrator password could not be written: its hash could not be made, or a re-hash could not be written to the file (once a process) |
| `HM7495` | At a start: the database records a key or holds sealed values and this node has no key, or its key line does not open here, or its key is not the database's, or the database holds values sealed under a key that is not the database's. Said at each start until it is mended |
| `HM7496` | A process that did not run a start - a verb - sealed nothing, because its key is not the database's or the database's record could not be read; what it saved is stored as written (once a process; on standard error from a command on Windows, and on Linux run as root) |
| `HM7498` | A start asked for with `sc start hMailServer --apply-new-install-defaults`, to finish a new installation whose administrator could not open the database, could not make the stored-secrets key: the reason - the service runs as an account that may not write `hMailServer.INI`, or the database records another key - and what makes it ([ServiceAccount.md](ServiceAccount.md), "A new installation whose administrator cannot open the database") |
| `HM7497` | `--forget-sealed-secrets KEYID` cleared what was sealed under that lost key: every item, in one entry (on standard error from the command on Windows, and on Linux run as root) |
| `HM6412` | On Linux, the stored-secret key file is missing when a secret is opened: now once a process, at Medium, rather than at every read at High |

None of them is said on a server as it is installed, nor on one upgraded without a key: that
server says in the application log, once a start, that sealing is off.


Checked by
----------

`Security/SecretsAtRest.cs` (REST port 10260, metrics port 10261), on a bench whose key the
fixture makes with `--create-stored-secrets-key` when it has none:

* the installer's MD5 signing in over COM and over REST and written again as PBKDF2, a
  wrong password changing nothing, and a sign-in with the old password leaving alone a newer
  one the verb wrote into the file beside the running service;
* `hMailServer.exe --set-admin-password` from the environment and from standard input, the
  environment first, refusing with neither and never printing the password;
* a second factor sealed as `HMK1:` under the server's key and verifying, before and after a
  save, and refusing every code once its envelope is copied into another account's row; one
  stored as generated read as it is and sealed at the next start;
* a backup and restore carrying a second factor and a `[Settings]` secret opened and sealing
  both again under new envelopes;
* every `[Settings]` secret sealed in the row and the line and read back whole, beside a
  setting that holds none; one an older server stored as written sealed at the next start;
* `ProtectStoredSecretsWithDPAPI` switched to 0 opening a sealed setting and a sealed second
  factor back to as written, and switched on again sealing them again;
* the key recorded in the database as the fixture's own, and `--create-stored-secrets-key`
  refused where the database records it and this node holds none;
* the key carried to a node without it: exported to a file only SYSTEM and the administrators
  can read, never printed and never over a file that exists, and the export recorded for the
  setup checklist, whose item turns from `stored_secrets_key_not_exported` to
  `stored_secrets_key_exported`; the node without it making no key of its own, saying `HM7495`
  and refusing every code of a sealed second factor, which stays enrolled; a damaged key file
  and another key refused, the other key by the database's record; and the code verifying
  again once the key is imported;
* the key file saved again with a UTF-8 byte order mark, and in UTF-16LE with its mark,
  imported as the key the node holds, which changes nothing, and the same file with blanks
  after it to more than 64 KiB refused for its size;
* `--forget-sealed-secrets` refused without a key id, while the service runs, and for the
  key the server holds; and with the lost key's id clearing a second factor and a `[Settings]`
  secret sealed under it - the account enrols again, the setting reads as not set - while one
  sealed under the server's own key still verifies, the record stays, and `HM7497` says so;
* a protected metrics token still guarding its listener.

`Installation/InstallerPackaging.cs` holds the installer to writing no MD5 and no `[INI]`
entry for the password, and to handing it over in the environment only, before the database
step. `API/RestApiSetupChecklist.cs` holds the checklist to its ten items. The seal itself -
round trip, another purpose, another account, another key, a changed byte - is in the
server's self-test (`StoredSecrets::Test`), and so is the import's reading of a key file: one
saved with LF line ends, with CRLF and a UTF-8 byte order mark, and in UTF-16LE with its mark
read through to its key, one of 64 KiB read, one a byte larger refused for its size, and one
that is not there said to be unreadable. On Linux `mailbox-crypto-tests` runs the same
self-test, and watches every block of memory the key verbs' code gives back to the heap - the
key file read by `--import-stored-secrets-key` in each of those three forms, the key read from
its line, exported by `--export-stored-secrets-key` and the exported file read back - for a
piece of the key in base64 or hex, as narrow text, as a `String` and in UTF-16LE, or of its
bytes: none may hold one. The import's last step, the key protected and written into the
line, runs only with a database open, which that program has not; what it copies - the hex,
the line, the line's UTF-8 that the machine's store protects - is written over by the same
guards, as the code shows (`StoredSecrets::StoreKey_`, `Crypt::EnCrypt`).
