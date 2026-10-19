# Relocating an hMailServer installation

Moving a configured installation from one directory to another - `C:\Program
Files (x86)\hMailServer` to `C:\Program Files\hMailServer`, or onto another
drive - is not one change. The installation path is recorded in the registry, in
the service, in the COM registration, in `hMailServer.ini`, and in a handful of
database columns that hold absolute file names an administrator typed. This is
the ordered list of every one of them, what repairs each, and the two places
where the honest answer is "there is no clean fix".

It is written for 6.3.0, and steps 1, 4 and 9 were revised on 15 September 2026
for schema 6042, which made the database the store for the settings the
`[Settings]` section of `hMailServer.ini` used to hold; steps 3, 6, 9, 11 and 12
were revised on 19 October 2026 for 6.3.7: the copy's permissions and
`hMailServer.exe /ProtectFolders`, a certificate's files changed with **Edit…**,
and setup run again at the new root. Every claim below was read out of the code;
the file and line references are there so you can check any of them.

**Installing somewhere new is a different, solved problem.** `/DIR="<path>"` on
the installer puts a *fresh* installation wherever you want. This document is
about an installation that already has mail in it.

**Copying the tree, or reinstalling into the new folder.** This document copies
the whole installation, its programs included. Where somebody who is not an
administrator may have changed a program in the old folder - an installation at a
drive's root before 6.3.7, say; [ServiceAccount.md](ServiceAccount.md), *Before
upgrading an installation at a drive's root*, says when - do not copy it: follow
*Reinstalling into a new folder* on the same page instead, which brings across the
mail, the built-in database, the settings file and the event script, each read
first, and has the release's setup put the programs in the new folder. Its step 3
runs setup as this document's step 11 does, and its step 4 moves the database's
paths by this document's step 9.

**The good news, first.** The mail store itself is already relocatable. Message
rows hold a bare `{guid}.eml` with no directory (`Message::GenerateFileName`,
`Common/BO/Message.cpp:261`), and the path is rebuilt at every read against the
data directory (`PersistentMessage::GetFileName`,
`Common/Persistence/PersistentMessage.cpp:1502`). Moving the data directory and
pointing `DataFolder` at its new home moves every message with it. The exception
is a database that predates hMailServer 5.4, where the column can hold a full
path; step 0 tells you whether yours does.

---

## 0. Before you touch anything

1. **Take a backup and verify it restores.** Everything below is reversible only
   while the old tree and a good backup both exist.

2. **Ask the server whether its message rows are relative.** In the Control
   Panel, *Monitoring & troubleshooting → Diagnostics → Test message file locations*
   (`Common/Diagnostics/TestDataDirectory.cpp:31`). If it reports
   `ERROR: Full paths are stored in the database`, the move is still possible but
   step 8 becomes mandatory rather than optional.

3. **Write down what you are about to strand.** The inventory is in step 1. Doing
   it afterwards means finding out from a failure.

---

## 1. The inventory

### In `Bin\hMailServer.ini`

| Section | Key | What breaks if it is left pointing at the old tree |
|---|---|---|
| `[Directories]` | `ProgramFolder` | Language files, the database scripts and 7-Zip for backups are looked for under it (`IniFileSettings.cpp:154`, `:1072`, `Compression.cpp:84`) |
| `[Directories]` | `DataFolder` | Every message path is rebuilt from it. The store appears empty |
| `[Directories]` | `DatabaseFolder` | The SQL Server Compact database file, if you use that backend |
| `[Directories]` | `LogFolder` | Logs are written to the old tree, silently |
| `[Directories]` | `TempFolder` | Message assembly and backup staging |
| `[Directories]` | `EventFolder` | Event scripts are not found; events stop running |
| `[Database]` | `PostgreSQLSslRootCert` | PostgreSQL TLS verification fails |
| `[Database]` | `TlsCaFile` | On MySQL, MariaDB and PostgreSQL, `TlsMode=verify-full` reads it, and refuses to connect once it is gone (HM7531) |

Leave the `[Database]` connection keys alone. They name a server, not a path.

### In the settings store (`hm_inisettings`)

These were `[Settings]` keys of `hMailServer.ini`, and the file still holds a copy
of each. Since schema 6042 the database is where they are stored: a value edited
into the file is named in the error log as `HM5804` and put back at the next start
(`IniSettingStore::Synchronize`). They are changed in step 9, not in step 4.

| Setting | What breaks if it is left pointing at the old tree |
|---|---|
| `ArchiveDir` | Archiving stops; existing archived copies are not found |
| `AcmeCertificateDirectory` | ACME state and issued certificates |
| `OAuth2PublicKeyFile` | Token validation refuses |
| `UpdateTrustRootsFile`, `UpdateLogPublicKeyFile` | Update verification refuses. From 6.3.6 these two, with the other three settings of the live update's trust, are read from the `[Settings]` section of the file alone and never from this store ([UpdateFeed.md](UpdateFeed.md), "Where the trust is read from"): change them in the file, in step 4, and restart the service |
| `RestApiCertificateFile` / `PrivateKeyFile`, `MetricsServerCertificateFile` / `PrivateKeyFile`, `WebServicesCertificateFile` / `PrivateKeyFile` | The listener starts without TLS, or does not start |

### In the registry

| Value | Where | Repaired by |
|---|---|---|
| `InstallLocation` | `HKLM\SOFTWARE\hMailServer` **in the 32-bit view** | Step 5, by hand |
| Service `ImagePath` | `HKLM\SYSTEM\CurrentControlSet\Services\hMailServer` | Step 6, `/Register` |
| `LocalServer32` (79 keys) and `InprocServer32` (15) | `HKCR\CLSID\...` | Step 6, `/Register` |
| TypeLib `win64` and `HELPDIR` | `HKCR\TypeLib\...` | Step 6, `/Register` |
| `InstallLocation`, `UninstallString`, `DisplayIcon` | `HKLM\...\Uninstall\hMailServer_is1` | Nothing. See step 11 |

The 32-bit view is not a mistake and not optional. The server reads that value
through `KEY_WOW64_32KEY` (`Common/Util/Registry.cpp:40`), and the installer
writes it there for the same reason
(`hmailserver/installation/section_registry.iss`). **Writing the 64-bit view
changes nothing and looks like it worked.**

### In the database

Each of these holds a path an administrator typed. None of them is rewritten by
anything, and there is no function that enumerates them.

| Table and column | What it is |
|---|---|
| `hm_sslcertificates.sslcertificatefile`, `.sslprivatekeyfile` | Certificate and key files |
| `hm_domains.domaindkimprivatekeyfile`, `.domaindkimsecondaryprivatekeyfile` | DKIM signing keys - by default, those the key rotation generated live under the data directory's `DKIM` folder |
| `hm_dkimrotations.dkrcurrentkeyfile`, `.dkrnextkeyfile`, `.dkrretiringkeyfile`, `.dkrretiredkeyfile` | The DKIM key rotation's copy of the same paths ([DkimKeyRotation.md](DkimKeyRotation.md)). The rotation follows the domain's own columns at its next pass, so correct those; a retiring key's path is only used to delete it, and a stale one deletes nothing, because the file must be inside the current `DKIM` folder |
| `hm_tcpipports.portclientcertificatecafile` | The CA bundle for inbound client certificates |
| `hm_settings` row `backupdestination` | Where backups are written |
| `hm_settings` rows `avclamwinexec`, `avclamwindb` | ClamWin's scanner and database |
| `hm_settings` row `customvirusscannerexecutable` | An external scanner command line |
| `hm_inisettings` | The path settings listed above, under *In the settings store* |
| `hm_archiveindex.archivepath` | One row per archived copy |
| `hm_quarantine.quarantinefilename` | Held messages |
| `hm_rule_actions.actionfilename` | A rule that runs a program or writes a file |
| `hm_messages.messagefilename` | Relative since 5.4; see step 8 |

---

## 2. Stop the service

Nothing in this runbook may run against a live server. Stop it, and confirm it is
stopped, before anything is copied.

---

## 3. Copy the tree

Copy, do not move. The old tree is the rollback.

Include `Bin\hMailServerApiKeys.ini` if it exists: the REST API key store lives
there (`Common/Util/RestApiServer.cpp:2414`), and no installer manifest mentions
it.

**A copy takes the permissions of the folder it lands in**, not those of the tree
it came from: `robocopy` without `/COPY:S` or `/SEC`, `xcopy` without `/O`, `copy`
and Explorer write no entries of their own on what they make. Under `C:\Program
Files` that gives `BUILTIN\Users` - every account that can sign in to the computer -
Read on the new `Data`, `Logs`, `Temp`, `Database`, `Events` and
`Bin\hMailServer.INI`: every message, the logs, the built-in database and the
database's password, which setup makes private at every installation and upgrade
from 6.3.6. At a drive's root on a Windows client it gives `Authenticated Users`
Modify on all of it, the programs in `Bin` included. And the entries the old
installation's grant gave a service that runs as `NT SERVICE\hMailServer` are not
copied, so that service cannot write the new data, log and temp folders; a server
that cannot write its log directory starts and then says nothing. Step 6 puts both
right, from the new `Bin`.

So that the programs are never copied into a folder anybody else may change, make
the new root first and give it what setup gives a program folder it makes - SYSTEM
and the Administrators full control, the Users Read & execute, nothing inherited -
before anything is copied into it. From an elevated prompt:

```
mkdir "<new root>"
icacls "<new root>" /inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F *S-1-5-32-545:(OI)(CI)RX
robocopy "<old root>" "<new root>" /E
```

The SIDs are SYSTEM's, the Administrators' and the Users', the same in every
language of Windows. Where no user may read the folder the new root is made in,
leave out the Users' grant at the end, as `/ProtectFolders` does. `robocopy` ends
in 1 when it has copied files, and in 8 or more when something could not be copied.

---

## 4. Rewrite the file keys in `hMailServer.ini`

Edit `<new root>\Bin\hMailServer.ini` and change the keys from step 1's
`hMailServer.ini` table whose value was under the old root: the `[Directories]`
folders and `[Database]` `PostgreSQLSslRootCert` and `TlsCaFile`. Nothing else of
that table; a path in `[SettingsOverride]` and the two trust files named below move
here too.

**Not the settings-store paths.** Editing one of them in the `[Settings]` section
changes nothing: at the next start the stored value is used, the edit is named in
the error log as `HM5804`, and the line is put back to the old path. They move in
step 9, once the moved server is running.

What is still read from this file, and only from it, is what the server needs to
reach the database or to let an administrator in without one: `[Directories]`,
`[Database]` and `[Security]` (the administrator password and its second factor),
with the language list in `[GUILanguages]`. So are the sections the settings store
has not taken over - `[LDAP]`, `[SendingLimits]` and `[SendingLimitsOverrides]`,
none of which holds a path; a `[SettingsOverride]` section, if you have one, which
is applied over the store at every start (a path in it moves here too); and six
`[Settings]` keys kept out of the database on purpose: `PasswordPepper`, which is
not a path, and from 6.3.6 the live update's trust, of which
`UpdateTrustRootsFile` and `UpdateLogPublicKeyFile` are paths and move here too.

The file is read from the directory the running `hMailServer.exe` sits in when
one is there, and otherwise from the registry (`Common/Util/Utilities.cpp:102`),
so the copy you edit is the one the moved server will read.

---

## 5. Repoint the registry root

From an elevated prompt:

```
reg add "HKLM\SOFTWARE\hMailServer" /v InstallLocation /t REG_SZ /d "<new root>" /reg:32 /f
```

`/reg:32` is the whole of the point. See step 1.

---

## 6. Re-register the server, and make its folders private

From an elevated prompt, from the **new** `Bin`:

```
hMailServer.exe /Register
hMailServer.exe /ProtectFolders
```

`/Register` repairs the service's `ImagePath`
(`ServiceManager.cpp:45` → `ChangeServiceConfig`), all 79 `LocalServer32`
registrations, the 15 `InprocServer32` ones, and the type library's `win64` and
`HELPDIR` values, and keeps the account the service runs as. It must run elevated,
and it must run from the new tree - it registers the path of the executable that
runs it.

`/ProtectFolders` gives the new tree the permissions setup gives an installation,
which a copy by hand does not (step 3): setup runs it at every installation and
upgrade from 6.3.6, and nothing else runs it for you. It finds the folders from where the
`hMailServer.exe` that runs it is, never from `hMailServer.ini`, and:

- makes the program folder's own `Data`, `Logs`, `Temp`, `Database` and `Events`,
  with everything in them, and `Bin\hMailServer.INI` private - SYSTEM and the
  Administrators full control, `NT SERVICE\hMailServer` what the grant gives it and
  Read on `hMailServer.INI`, and nothing the folder above hands down, the Users'
  Read above all;
- from 6.3.7, hardens the program folder itself, `Bin` and `ControlPanel`: SYSTEM
  and the Administrators may change them, the Users may read and run them where
  every user could read them before, and nobody else may change them, whatever a
  drive's root hands down; a folder somebody else owns is given to the
  Administrators.

Each line it writes says `made private`, `already private`, `hardened`, `already
hardened` or `FAILED` with the reason, and it ends in 0 when each was made private
or hardened, or was already; 1 when one could not be; 2 when it was not run from an
installation's `Bin`; 3 when it was not run as an administrator. Run it after step
4, not before: an editor that saves `hMailServer.ini` by writing a new file and
renaming it over the old one gives it `Bin`'s permissions again. A folder that
`[Directories]` names outside the new root - a data folder kept on another drive -
is not changed, only checked: where Users, Authenticated Users, Everyone or another
group of every user may read it, a `CHECK` line names them, with the two `icacls`
that close it. `hMailServer.exe --report-only
/ProtectFolders` says what it would change, and changes nothing. What it does, and
why, is in [ServiceAccount.md](ServiceAccount.md), *What the other users of the
computer may read*.

**A service that runs as anything but LocalSystem** - `sc qc hMailServer` names
its account on the `SERVICE_START_NAME` line, `NT SERVICE\hMailServer` for the
virtual account - needs its folders granted again, as setup grants them after
`/ProtectFolders`:

```
hMailServer.exe /GrantServiceAccess
```

It gives the account Modify on the new `Data`, `Logs`, `Temp` and `Database`, Read
on the program's other folders and Modify on `Bin\hMailServerApiKeys.ini`, and
reports what the settings file and the database name that those grants do not
reach. Until step 9 has moved them, that includes every path the database still
holds into the old tree, a certificate's or a DKIM key's: it lists each, `OUTSIDE`,
`NOT APPROVED` or `REFUSED`, and ends in 4 or 5, its grants made all the same. Run
it again after step 9, when its report no longer names the old tree. A data
folder kept outside the new root is granted only once approved, with
`--approve-modify "<folder>"` before the verb ([ServiceAccount.md](ServiceAccount.md),
*Approving a folder*).

---

## 7. Start it, and check what it read

Start the service and read the application log.

**A server whose anchors are stale reports `Running` and serves an empty
configuration.** That failure is silent, and it is the one this whole document
exists to prevent: `build/preflight-tests.ps1` carries a check for it precisely
because it has happened. Confirm over COM or in the Control Panel that the
**Installation paths** section of *Monitoring & troubleshooting → Diagnostics*
shows the new paths, that your domains are listed, and
that the log file you are reading is under the new log directory.

---

## 8. Convert full message paths, if step 0 found any

Only if *Test message file locations* reported full paths.

Run the **Data Directory Synchronizer** against the new data directory. It walks
the data directory and converts each absolute row to a partial name
(`Tools/DataDirectorySynchronizer`, `MailImporter::ReplaceMessagePath_`). It is
the supported converter; a `UPDATE hm_messages SET ...` is not, because the rows
are not uniform and the file layout is derived, not stored.

---

## 9. Fix the database-borne paths

For each row from step 1's settings-store and database tables that pointed
under the old root:

- **The settings-store paths.** In the Control Panel under **Settings**, whose
  search box finds each one by name; or `Settings.SetIniSetting` over COM,
  `PUT /api/v1/settings/ini/{name}`, or `hmctl ini set <name> <value>`. Each
  stores the new value in the database, writes it into the file's copy as well,
  and applies it - except the listeners' certificate paths and the ACME
  certificate directory, which are bound when the servers start and wait for a
  reinitialise (**Apply now**, or `POST /api/v1/server/reinitialize`;
  [SettingsApply.md](SettingsApply.md) lists them). Until then the server runs on
  those old paths, which still resolve because step 3 copied the tree rather than
  moving it. Do not make the old tree unreachable (step 12) until that
  reinitialise has picked up the new paths.
- **TLS certificates.** *Certificates & encryption → TLS certificates*: select
  each certificate whose files moved, **Edit…**, and give it the new certificate
  and private key file names. It keeps its id, so every TCP/IP port that presents
  it keeps it; the ports read its files again at the next reinitialise (**Apply
  now**) or restart. Over COM, setting the certificate's `CertificateFile` and
  `PrivateKeyFile` and calling its `Save()` does the same; the REST API adds and
  deletes a certificate, and changes none. **Do not delete a certificate and add it
  again**: the one added is a new certificate, and each port that presented the
  deleted one is left with none - an SSL/TLS or STARTTLS port without a
  certificate does not start (`HM5113`) until another is chosen for it on
  *Connections & protocols → TCP/IP ports*. The page's question before a delete
  names those ports.
- **DKIM keys.** *Domain → DKIM*, both the primary and, if used, the secondary.
  A domain whose key rotation manages its keys refuses that edit: cancel the next
  key, retire or wait out the old one and switch the schedule off first, or let
  the rotation make a new key under the new data directory.
- **Client-certificate CA bundle.** *Connections & protocols → TCP/IP ports*, per
  port. A port set to *require* a client certificate refuses every client when its
  bundle cannot be loaded.
- **Backup destination, ClamWin, custom scanner, rule actions.** Each in its own
  page.
- **Archive index.** `hm_archiveindex.archivepath` holds one row per archived
  copy and is matched on the exact string by retention, legal hold and the
  address eraser. Do not bulk-rewrite it. If the archive directory moved, move
  the archive back or accept that pre-move rows point at the old location; there
  is no supported converter for this column.

---

## 10. Verify before deleting anything

- Send and receive a message.
- Complete a TLS handshake on every port that has a certificate.
- Connect on any port whose client-certificate policy is *require*.
- Run a backup to completion. It refuses outright when its destination is
  unwritable, which is the check you want.
- Run an event script, if you use them.
- Check the log directory is being written.

---

## 11. What stays broken, and why

- **Add/Remove Programs** still points at the old `unins000.exe`. Do not edit
  `unins000.dat`; it is a compiled record, and a hand edit produces an
  uninstaller that removes the wrong tree. Never run the old uninstaller either:
  it stops the service and runs the old tree's `hMailServer.exe /Unregister`,
  which removes the service and the COM registration that the new tree now
  holds. The supported repair is to run the release's setup again at the new
  root, which rewrites the uninstall metadata - below.
- **Start-menu shortcuts** hold absolute targets. Re-create them, or run setup
  again at the new root, which writes them for it.
- **Message content.** The old path appears inside messages the server itself
  wrote - a delivery report, for example, names the file it could not find.
  Those are historical records of what happened. **Never rewrite message
  content**, and be careful with any "search and replace across the
  installation" approach for exactly this reason.

**Running setup again at the new root.** Setup run over an installation shows no
folder page and installs into the folder it finds - the old one, from the uninstall
entry - so it is given the new root with `/DIR`. And an installation that began in
`C:\Program Files (x86)`, as a 32-bit installation, has an uninstall entry in the
registry's 32-bit view, which every upgrade since has left there and whose folder
setup takes whatever `/DIR` says. Where the first line below finds that entry, take
it away with the second, then run setup, from an elevated prompt:

```
reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\hMailServer_is1" /reg:32
reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\hMailServer_is1" /reg:32 /f
hMailServer-<version>-x64.exe /DIR="<new root>"
```

Setup finds the settings file in the new `Bin` and goes on as an upgrade of the
moved installation, the service's account kept. It writes the uninstall entry and
the shortcuts for the new root, runs `/Register` there - `sc qc hMailServer` then
names the new `Bin` - and runs `/ProtectFolders` and, for a service that runs as
anything but LocalSystem, the grant again. This is the same step as step 3 of
*Reinstalling into a new folder* in [ServiceAccount.md](ServiceAccount.md).

---

## 12. Keep the old tree

Keep it, unreachable by the service, for at least one full backup cycle. Once it
is deleted, the only rollback is the step 0 backup. Delete it by hand, never with
its uninstaller (step 11).

Keep it private while it is there. Nothing runs `/ProtectFolders` on a folder no
installation is in, and where 6.3.5 or earlier left its `Data`, `Logs`,
`Database` and `Bin\hMailServer.INI` taking the permissions of the folder they are
in, every user may still read the mail, the logs, the built-in database and the
settings file there: make it the Administrators' and SYSTEM's alone as step 4 of
*Reinstalling into a new folder* in [ServiceAccount.md](ServiceAccount.md) does.

---

## Seeing where the paths are, without doing any of this

The Control Panel's *Monitoring & troubleshooting → Diagnostics* page has an **Installation paths**
section that lists the registry root, every `[Directories]` key, the derived
directories, and every path-shaped setting the running server holds but
`[Database]` `TlsCaFile`, with the ones that do not exist marked. It changes
nothing; it is there so that the inventory in step 1 can be read off a running
server rather than assembled by hand.

## Why the paths are not one setting

This came up as GitHub issue 158, on the project's former home.
The short answer is that `ProgramFolder` is not the root of anything: it is one
of six independent absolute strings, and the registry holds a seventh copy that
the server actually uses to find its own INI file. From 6.3.0 a *relative*
value in the five non-program directories is resolved against the program folder,
so a new installation can be written to be movable; an existing installation's
absolute values are left exactly as they are, because rewriting a configured
server's paths during an upgrade is not something an upgrade may do.
