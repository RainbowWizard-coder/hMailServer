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
`[Settings]` section of `hMailServer.ini` used to hold. Every claim below was read
out of the code; the file and line references are there so you can check any of
them.

**Installing somewhere new is a different, solved problem.** `/DIR="<path>"` on
the installer puts a *fresh* installation wherever you want. This document is
about an installation that already has mail in it.

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

Grant the service account access to the new tree. The installer grants it to
`NT SERVICE\hMailServer` on the data, log and temp directories; a copy made by an
administrator will usually inherit different rights, and a server that cannot
write its log directory starts and then says nothing.

---

## 4. Rewrite the file keys in `hMailServer.ini`

Edit `<new root>\Bin\hMailServer.ini` and change the keys from step 1's
`hMailServer.ini` table whose value was under the old root: the `[Directories]`
folders and `[Database]` `PostgreSQLSslRootCert`. Nothing else.

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

## 6. Re-register the server

From an elevated prompt, from the **new** `Bin`:

```
hMailServer.exe /Register
```

One command repairs the service's `ImagePath`
(`ServiceManager.cpp:45` → `ChangeServiceConfig`), all 79 `LocalServer32`
registrations, the 15 `InprocServer32` ones, and the type library's `win64` and
`HELPDIR` values. It must run elevated, and it must run from the new tree - it
registers the path of the executable that runs it.

---

## 7. Start it, and check what it read

Start the service and read the application log.

**A server whose anchors are stale reports `Running` and serves an empty
configuration.** That failure is silent, and it is the one this whole document
exists to prevent: `build/preflight-tests.ps1` carries a check for it precisely
because it has happened. Confirm over COM or in the Control Panel that
*Settings → Directories* shows the new paths, that your domains are listed, and
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
- **SSL certificates.** The Control Panel's certificate grid is read-only except
  for the passphrase, so each certificate must be deleted and re-added with its
  new file names, or set over COM.
- **DKIM keys.** *Domain → DKIM*, both the primary and, if used, the secondary.
  A domain whose key rotation manages its keys refuses that edit: cancel the next
  key, retire or wait out the old one and switch the schedule off first, or let
  the rotation make a new key under the new data directory.
- **Client-certificate CA bundle.** *Advanced → TCP/IP ports*, per port. A port
  set to *require* a client certificate refuses every client when its bundle
  cannot be loaded.
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
  uninstaller that removes the wrong tree. The supported repair is to re-run the
  installer at the new root, which rewrites the uninstall metadata.
- **Start-menu shortcuts** hold absolute targets and nothing repairs them.
  Re-create them, or re-run the installer.
- **Message content.** The old path appears inside messages the server itself
  wrote - a delivery report, for example, names the file it could not find.
  Those are historical records of what happened. **Never rewrite message
  content**, and be careful with any "search and replace across the
  installation" approach for exactly this reason.

---

## 12. Keep the old tree

Keep it, unreachable by the service, for at least one full backup cycle. Once it
is deleted, the only rollback is the step 0 backup.

---

## Seeing where the paths are, without doing any of this

The Control Panel's *Monitoring & troubleshooting → Diagnostics* page has an **Installation paths**
section that lists the registry root, every `[Directories]` key, the derived
directories, and every path-shaped setting the running server holds, with the
ones that do not exist marked. It changes nothing; it is there so that the
inventory in step 1 can be read off a running server rather than assembled by
hand.

## Why the paths are not one setting

This came up as GitHub issue 158, on the project's former home.
The short answer is that `ProgramFolder` is not the root of anything: it is one
of six independent absolute strings, and the registry holds a seventh copy that
the server actually uses to find its own INI file. From 6.3.0 a *relative*
value in the five non-program directories is resolved against the program folder,
so a new installation can be written to be movable; an existing installation's
absolute values are left exactly as they are, because rewriting a configured
server's paths during an upgrade is not something an upgrade may do.
