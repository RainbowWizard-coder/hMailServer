The service account
===================

The Windows service reads mail from anybody on the internet, in C++, and for as
long as this project has existed it has done that as LocalSystem, the most
privileged account on the machine. A flaw reachable through SMTP, IMAP or POP3
was a flaw with full control of the computer. Since 25 September 2026 a **new**
installation runs the service as `NT SERVICE\hMailServer` instead: the service's
own virtual account, which Windows makes from the service's name - no password,
no account to create - and which reaches only the folders it is granted. The
Linux packages of this same server already run as a user of their own
(`hmailserver.service`: `User=hmailserver`); this is the Windows half.

What this page covers: what the installer asks and does, what the account is
granted and by what, why nothing the service can write decides that, what the
service may no longer write - `hMailServer.INI` - and what became of each thing
that wrote it, the table of everything LocalSystem could do that the virtual
account cannot, the live update's answer, where the Control Panel, the Control
Deck and the checklist say which account the service runs as, and how to go back.

What it does not cover, because it stays declined: an installation under an
ordinary account for shared hosting (discussion #267). The secret store is
machine-scope DPAPI (`DataProtector.cpp`, `CRYPTPROTECT_LOCAL_MACHINE`), which
anyone with administrator rights on the machine can read; the COM registration is
machine-wide; and the message store is guarded by file permissions that an
ordinary user cannot set against the other users of the same machine - who, on
shared hosting, are the other tenants.

## The rule the rest follows

A service that can make an elevated program act on something it chose has given
back what the account took away. Three elevated programs act on this service's
behalf - the installer, `hMailServer.exe /GrantServiceAccess` and
`hMailServer.exe /Register` - so none of them takes a folder, a file or an
account from anywhere the service can write:

* **`hMailServer.INI` is not the service's to write.** It holds `[Database]`,
  `[Directories]` and `[Security]` - how the server reaches its database, where
  its folders are, and who may administer it - and the elevated programs read it.
  The service runs on it read-only (below).
* **The grant takes its folders from trusted places alone**: where the program
  is, and a list only administrators may write. A folder the configuration names
  is reported, never granted, until an administrator approves it after a question
  that names it.
* **`/Register` takes the account from its command line alone**, never from
  `ServiceAccountName`.

## What the installer does

The installer has a page for it, after the server profile, whenever the server
component is installed. It says which case applies:

| The machine | The page offers | The default |
|---|---|---|
| No hMailServer service yet - a new installation; or the LocalSystem service a new installation left when its database step did not complete (below) | `NT SERVICE\hMailServer`, or LocalSystem as earlier versions ran it | `NT SERVICE\hMailServer` |
| A service running as LocalSystem - an upgrade | keep LocalSystem, or move it to `NT SERVICE\hMailServer` | keep LocalSystem |
| A service running as any other account - an upgrade | keep that account, or move it back to LocalSystem | keep the account |

An upgrade never changes the account unless the page is answered to. A silent
installation takes the same choice as `/serviceaccount=virtual`,
`/serviceaccount=localsystem` or `/serviceaccount=keep`; without it, a new
installation gets the virtual account and an upgrade keeps what it has. When the
database is SQL Server reached with Windows authentication, the move page says
that the moved service needs a login there, which setup gives it first (step 4).

The order is the point:

1. The service is registered - `hMailServer.exe --service-account <word>
   /Register` - as **LocalSystem** when it is to be moved (a new installation, or
   an upgrade of a LocalSystem service that chose the move), and as its chosen or
   kept account otherwise.
2. A service that already runs as an account of its own is granted its folders
   again, for anything this version added.
3. The database step runs - DBSetupQuick for the built-in database, DBSetup for an
   external one - and the chosen profile is applied. That step writes `[Database]`
   into `hMailServer.INI` through the service, over COM, and sets the service's
   dependency on a local database server; LocalSystem may do both, and the
   virtual account may do neither.
4. For a move, when the database is SQL Server reached with Windows
   authentication, `hMailServer.exe --grant-database-login` gives
   `NT SERVICE\hMailServer` a login there, a user in the hMailServer database and
   `db_owner` (below) - as the administrator running setup, on a connection of its
   own, while the service is still LocalSystem's. The database step made or opened
   the database as LocalSystem; the moved service signs in as the virtual account,
   and without these it cannot open the database. When they cannot be given on a
   database this installation set up, the service is not moved: it stays on
   LocalSystem, working, the message gives the statements a SQL Server
   administrator runs, and setup ends with **10** (Upgrading.md, "If the upgrade
   fails"). Then the service is stopped and `hMailServer.exe /GrantServiceAccess`
   runs - with `--confirm` in an attended installation. By now the database names
   every certificate and key file, so the grant can say which are outside the
   folders it grants. Only when it ends in 0 is the service registered as
   `NT SERVICE\hMailServer`. Anything else - a folder nobody approved in a silent
   installation, a folder that can never be granted, No to the question, a grant
   that failed - leaves the service on LocalSystem, working, and says why; the
   installation still succeeds. A database step that failed leaves it on
   LocalSystem as well, and a new installation's leaves the mark that lets it be
   finished later (below).
5. The service is started. A service this installation has just moved is then held
   to opening its database as `NT SERVICE\hMailServer` - the database's version is
   read through it, as the installer reads it for an upgrade - since a service
   reports itself running before it has opened anything. One that did not start,
   or did not open its database, is moved back to LocalSystem and started again,
   and setup says why and ends with 10 (11 when its database step changed the
   database): until 6.3.6 such a server, which listened on no port, ended setup
   with 0.

A grant that fails at step 2 fails the installation - that service may not start
- and says so in its own words, not the database's.

**A new installation whose database step did not complete** - DBSetup refused a
setting, the database server refused the sign-in, the wizard was closed - has its
files, a service registered as LocalSystem for the database step, and no database;
none of what follows that step has happened: the secure defaults
([SecureDefaults.md](SecureDefaults.md)), the profile, the stored-secrets key
([SecretsAtRest.md](SecretsAtRest.md)) and the move. Setup ends with 10 and leaves
a mark, `NewInstallationPending` (the account chosen, `virtual` or `localsystem`)
and `NewInstallationProfile` (the profile, if one was) under
`HKEY_LOCAL_MACHINE\SOFTWARE\hMailServer` in the 32-bit view, writable by
administrators only. Either way the message offers then finishes it as a new
installation:

* **Run setup again**, with the setting corrected. It finds the mark - or the
  LocalSystem service beside a `hMailServer.INI` that names no database, which an
  earlier version's setup leaves - and sets it up as a new installation's: the
  pages offer, and a silent installation defaults to, the account and the profile
  the mark holds (`NT SERVICE\hMailServer` without one), `/serviceaccount=` and
  `/profile=` choosing otherwise, and the steps after the database run as for any
  new installation. A
  LocalSystem service whose `hMailServer.INI` names a database, or whose file is
  not found, is an upgrade's, and keeps LocalSystem unless the move is chosen.
  On the built-in database the same holds: setup run again finishes the
  installation, where `DBSetupQuick.exe` would set the database up and nothing
  after it, so the message after a new installation's failed step names setup.
* **Run `DBSetup.exe` as an administrator** from the `Bin` folder - its wizard, or
  `/unattended` with the settings in its environment
  ([WindowsOnArm.md](WindowsOnArm.md)). With the mark there, once the database is
  set up it runs the installer's steps itself, in the installer's order, with
  hMailServer.exe's verbs: the service stopped, `--apply-new-install-defaults`,
  the profile (`--apply-profile NAME --once --if-new --confirm`),
  `--create-stored-secrets-key`, and for the virtual account
  `--grant-database-login`, the grant and `--service-account virtual /Register`;
  then the service started and, when moved, held to opening its database, moved
  back to LocalSystem when it does not. Each step is said as it goes, and the mark
  is removed - kept only when the service would not stop, so that none of them
  ran. The wizard asks Windows for elevation itself when the mark is there;
  `/unattended` without elevation refuses, before it changes anything, with exit
  code 2.

**A new installation whose administrator cannot open the database** - SQL Server
with Windows authentication, where the database step made the database as
LocalSystem, a SQL Server administrator, and the person running Setup is none -
has its database, and `--apply-new-install-defaults` (exit 2) and
`--create-stored-secrets-key` (exit 1), run as that person, could not open it. Until
6.3.6 neither was ever applied: the recovery the message gives - the statements run
by a SQL Server administrator, then Setup again - runs as an upgrade. Now Setup,
and DBSetup finishing an installation, start the service with the argument that has
it apply them itself: `sc start hMailServer --apply-new-install-defaults`. The
service opens its database, applies a new installation's defaults with the verb's
own rules - the database's `NewInstallDefaultsPending` mark required, the shape of a
new database, the mark cleared once decided - and, for the new installation it
found and applied them to, makes the stored-secrets key when it runs as LocalSystem, which may write `hMailServer.INI` (the virtual account may
not, and says so, HM7498), before its listeners start. What it could not do is in the
error log (HM7498, HM7510, HM7511), each step in the application log when that is
on, and the Control Deck's setup checklist shows the outcome. An administrator can start it so at any time after; the mark is
acted on whatever its age, since the argument is the request. The profile chosen is
not applied this way: the Control Deck's checklist offers it.

Setup clears the mark before its own database step, so that the DBSetup it runs
leaves those steps to it, and writes it again only if that step does not complete
either; the uninstaller removes it. An update hMailServer.Updater runs - the live
update, or *Install update* in the Control Panel - leaves the mark alone and
upgrades the installation as it is: unattended, it keeps the account and changes
nothing a new installation would, and its database step is held to the version
before it, as any upgrade's is. Until 6.3.6 setup run again kept the service on
LocalSystem, and DBSetup set the database up and nothing else - no self-signed
certificate (the database kept its `NewInstallDefaultsPending` mark), no
stored-secrets key, LocalSystem (the installer smoke VM, 2 October 2026).

Every registration of an account other than LocalSystem also sets the service's
SID type to *unrestricted*, so that the service's token carries
`NT SERVICE\hMailServer` whatever account it logs on as - which is what lets one
set of grants serve the virtual account and a group managed service account alike.
A registration that leaves the service on LocalSystem leaves the SID type alone.

## What the account is granted: `hMailServer.exe /GrantServiceAccess`

Run elevated - by the installer, by the Control Panel's buttons, or by hand from
an elevated Command Prompt in the `Bin` folder. It grants the service's SID,
`NT SERVICE\hMailServer`
(`S-1-5-80-3504853920-2627644491-1723263464-2546768561-1700046809`, derived from the
name as Windows derives it), exactly this:

| Where | What | Why |
|---|---|---|
| The program folder's own `Data`, and everything in it | Modify | The message store, and its ACME, Certificates, DKIM, Discovery, Files, Recoverable, Blobs, Lists, Updates and other folders |
| The program folder's own `Logs`, and everything in it | Modify | The logs, and the crash dumps and the crash record, which are written there |
| The program folder's own `Temp`, and everything in it | Modify | Messages being received and scanned, and the virus scanners' copies |
| The program folder's own `Database`, and everything in it | Modify | The built-in SQL Server Compact database |
| Each folder approved in `HKEY_LOCAL_MACHINE\SOFTWARE\hMailServer\ServiceAccessPaths`, and everything in it | Modify, or Read | A data, log or backup folder, an archive folder, a certificate folder elsewhere - what an administrator said the service may reach |
| Each other folder in the program folder - `Bin`, `DBScripts`, `Events`, `Languages`, `WebAdmin`, `Addons` and the rest - and everything in it | Read | The program, its scripts, its languages and the Deck. Not the program folder itself: every service holds *bypass traverse checking*, so the account reaches the folders in it without, and an entry on it would make Windows walk everything below it - the message store included, a tree the service writes in once it has moved |
| Each file with protected permissions in the DKIM key folder and the Updates folder, when those are in a folder granted Modify | Modify, on the file | The DKIM rotation's keys and the live update's token are written with permissions of their own (`ProtectedFile`) that inherit nothing, so a service moved off LocalSystem would otherwise lose the keys it signs with |
| A certificate or key file the configuration names inside a folder above, whose own permissions keep that folder's grant from it | Read, on the file | TLS and DKIM signing |
| `Bin\hMailServerApiKeys.ini` | Modify, on the file | The REST API's key store, which the service writes when a key is made or revoked. Made empty when it is not there yet - which the REST API reads as no keys - so that the service can write into it without being able to create files in `Bin`. It is the one file there the service may write, because nothing elevated takes a path, an account or a command from it: it holds key hashes the REST listener checks, and the service could mint a key through its own API anyway |
| `hMailServer.INI` | Nothing | Read-only to the service - see below |

**The program folder is where the program is**: the folder above the `Bin`
folder `hMailServer.exe` runs from, not `[Directories] ProgramFolder`. A binary
that is not in a folder named `Bin` grants nothing (exit 2).

**What the configuration names is only reported.** The folders of
`[Directories]` (DataFolder, LogFolder, TempFolder, DatabaseFolder for the
built-in database, EventFolder), the backup folder, `ArchiveDir` and
`AcmeCertificateDirectory`, and the certificate and key files - the SSL
certificates, the domains' DKIM keys, the REST API's, the metrics listener's and
the web services' - are read from the file and from the database (the settings
from `hm_inisettings` itself, not the file's copy), and each is checked against
the grants above. One the grants reach needs nothing. Otherwise:

* a folder nobody has approved is listed `NOT APPROVED`;
* a certificate or key file is listed `OUTSIDE`, with the way to fix it: move it
  into `Data\Certificates`, which the data folder's grant reaches, or approve its
  folder for Read. It is never given Read where it is: the configuration could
  name any file on the machine;
* a folder that may never be granted is listed `REFUSED`, with why;
* a backup folder on a network share is noted: the service reaches a share as the
  computer account (`DOMAIN\COMPUTER$`), and that is granted on the share.

**Approving a folder.** `--approve-modify "<folder>"` or `--approve-read
"<folder>"` on the command line; or `--confirm`, with which the verb asks - in a
box of its own, on the desktop of the elevated process, which nothing running at
a lower integrity can answer - naming each folder not approved, each key file
outside and each refusal, before it changes anything. Yes approves the folders,
No changes nothing at all. An approval is recorded in
`HKEY_LOCAL_MACHINE\SOFTWARE\hMailServer\ServiceAccessPaths` (64-bit view), one
value per folder - its full path as the name, `Modify` or `Read` as the data - in
a key the verb makes owned by Administrators and writable by Administrators and
SYSTEM alone. If anybody else may change that key, or owns it, nothing in it is
granted and the run fails with the reason. Approve only a folder you recognise:
the service may then write in it.

**Never granted, approved or not**, a folder that:

* is not a full path on a fixed local drive - a share, a device path (`\\?\`,
  `\\.\`), a relative path or a stream;
* is a drive's root;
* is, or holds, the program folder; or is in it but outside its own `Data`,
  `Logs`, `Temp` and `Database` - `Bin` above all;
* is, holds or is in the Windows folder, a Program Files folder, the user
  profiles folder or `ProgramData\Microsoft` (every user's Start menu and startup
  folder);
* is or holds `ProgramData` (a folder of its own in it may be approved);
* is, or runs through, a junction, a symbolic link or a mount point.

Every path is resolved first - `..` taken out, short names made long - so none of
these can be named another way.

**How a grant is written.** One allow entry for the service's SID is added and
nothing is removed - no other entry is changed, an administrator's deny entry for
the service included; an explicit deny outranks any allow, so the line says the
grant FAILED and names it, and an inherited deny is named as the entry the new
one now comes before. While a folder is granted, it and every folder above it are
held open without sharing delete, so none of them can be renamed, removed or
swapped for a link while the permission is written through the path. A file is
granted through a handle opened without following links, and only when it has
one name - a hard link elsewhere would carry the grant to wherever else it is. A
second run finds the entries there and changes nothing: each line says
`granted`, `already there` or `FAILED` with the reason, and the last line counts
them. Windows hands a folder's entry down to everything below it as part of the
grant, which on a large message store takes minutes the first time and nothing
after that.

**What it checks, and only says.** When `Everyone`, `Authenticated Users`,
`Users`, `SERVICE` or the service itself may write the program folder, `Bin` or
`hMailServer.INI` - an installation outside Program Files can inherit that from a
drive's root - a `CHECK` line names the entry and the `icacls` that takes it away.
The grant removes nobody's entry.

`--report-only` says all of this - what would be approved and granted, and what
is not reached - and changes nothing.

Exit codes: 0 every grant made and everything the configuration names reached, or
confirmed; 1 a grant or an approval failed; 2 not in an installation's `Bin`
folder; 3 not run as an administrator; 4 folders not approved or key files
outside, not confirmed; 5 the configuration names a folder that is never granted;
6 the administrator answered No, and nothing was changed; 7 the grants were made,
but this account could not read the database, so the certificate and key files and
the backup and PST import folders it names are not known, and nobody confirmed. The
installer and the Control Panel move the service only on 0.

**A database it cannot read.** On SQL Server with Windows authentication, where
LocalSystem is a SQL Server administrator and the person running the grant is not,
the grant cannot read what the database names. It says so on a `NOT READ` line and
ends in 7 - until 6.3.6 it said so in a note and ended in 0, and the service was
moved without anybody knowing whether a key file it reads lay outside its folders.
With `--confirm` the question names the database it could not read, and Yes is a
0. A silent installation's move then waits, as for a folder nobody approved; an
upgrade of a service already moved goes on, since nothing is moved. A database not
set up yet names nothing, and is no 7.

## Its login on SQL Server: `hMailServer.exe --grant-database-login`

On SQL Server reached with Windows authentication - `[Database] Type=MSSQL` and no
`Username` - the server signs in as the account the service runs as. LocalSystem
is `NT AUTHORITY\SYSTEM` to a SQL Server on the same machine; the virtual account
is `NT SERVICE\hMailServer`, which has no login there until one is made, and no
user in the hMailServer database, which can only be made once the database
exists. The verb makes what is missing of the three, on a connection of its own,
as the account that runs it:

```
IF NOT EXISTS (SELECT 1 FROM sys.server_principals WHERE name = N'NT SERVICE\hMailServer') CREATE LOGIN [NT SERVICE\hMailServer] FROM WINDOWS;
USE [hMailServer];
IF NOT EXISTS (SELECT 1 FROM sys.database_principals WHERE name = N'NT SERVICE\hMailServer') CREATE USER [NT SERVICE\hMailServer] FOR LOGIN [NT SERVICE\hMailServer];
ALTER ROLE db_owner ADD MEMBER [NT SERVICE\hMailServer];
```

(`hMailServer` being the database `[Database] Database` names.) It looks first and
leaves what is there - a login, a user under another name for the same login, or
the login as the database's owner - and takes nothing away. Each step is one line:
`made`, or `there already`. It makes no user and changes no role in a database
that has DDL triggers of its own (`sys.triggers`, enabled, on the database): such
a trigger runs with the rights on SQL Server of whoever's statement fires it, so
anybody who was ever `db_owner` there - an earlier server's login, another
program's - could have left one to run as the administrator, server-wide. The
verb then says so, and a SQL Server administrator reads the triggers before
running the statements. Run it from an elevated prompt in the `Bin` folder as
an account that may make logins on that SQL Server (`securityadmin` or `sysadmin`)
and that can open the database; an administrator who is a SQL Server administrator
through the `Administrators` group is one only when elevated. The installer runs it
as the administrator running setup, before it moves the service (above), and so
does DBSetup when it finishes a new installation. When it cannot do a step, it says
which and why, and prints the statements above for a SQL Server administrator to
run, with the `sqlcmd` line that runs them. It says everything on its own output
and writes nothing to the error log or the Application log: a connection the
account running it cannot make would otherwise be reported there as the server's
own failure to reach its database (HM5028), and the log folder is the service's.

A SQL Server on **another computer** is left alone: there the virtual account signs
in as this computer's own account, `DOMAIN\COMPUTER$`, as LocalSystem does, so the
login that worked keeps working, and a login named `NT SERVICE\hMailServer` could
not even be made there. The verb compares SQL Server's `MachineName` and
`ComputerNamePhysicalNetBIOS` with this computer's name, and says which it found.

Exit codes: 0 the service's account can open the database - its login, user and
`db_owner` made now or there already - or needs nothing here, which the lines say
(not SQL Server with Windows authentication, or SQL Server on another computer); 1
one of them could not be made; 2 `hMailServer.INI` names no database, or the
database could not be opened as the account that ran it. Run before
`--service-account virtual /Register` with `&&`, it moves only a service that will
open its database.

## `hMailServer.INI` is read-only to the service

The file keeps four kinds of thing, and under `NT SERVICE\hMailServer` the service
writes none of them. What each writer does instead:

| What wrote the file | Under the virtual account | How |
|---|---|---|
| The `[Settings]` copy of the settings store, at every start, reinitialise and save (`IniSettingStore`) | Not written. The database is the settings store and its values are in use, as ever | Said once per start in the application log, not as an error. A setting removed while the file still has its line is kept stored as its default - which the server treats the same - because a dropped row would be put back from the line at the next start; a key this build does not read is refused with a sentence. Each row records what the file really holds, so an old copy is never taken for an edit, and an edit is named once. A backup restored stores its settings in the database |
| `PasswordPepper`, kept in the file only | Refused with a sentence | An administrator sets it in the file with an elevated editor and restarts the service |
| The live update's trust - `UpdateSigningIdentity`, `UpdateSigningIssuer`, `UpdateSourceRepository`, `UpdateTrustRootsFile` and `UpdateLogPublicKeyFile` - read from the file alone from 6.3.6 | Refused with a sentence, under every account: the process an update is checked for may not choose what it is checked against ([UpdateFeed.md](UpdateFeed.md), "Where the trust is read from") | An administrator sets them in the file with an elevated editor and restarts the service |
| `[Directories]` - COM `Directories.*` | Refused with a sentence | An administrator changes the file and approves a new folder for the service |
| `[Database]` - COM `Database.CreateInternalDatabase`, `CreateExternalDatabase`, `SetDefaultDatabase`, which DBSetup and DBSetupQuick call | Refused before anything is made | The installer does its database step under LocalSystem (above). To change the database later, move the service to LocalSystem for the change and back, or edit `[Database]` and restart |
| `[Security] AdministratorPassword` - COM `Settings.SetAdministratorPassword`, the Control Panel's *Administrator password* card | Refused with a sentence | `hMailServer.exe /SetAdministratorPassword`, from an elevated prompt, reads the password from standard input and writes the hash the card writes; restart the service. From PowerShell: `$p = Read-Host -AsSecureString 'New administrator password'; [Net.NetworkCredential]::new('', $p).Password \| & '.\hMailServer.exe' /SetAdministratorPassword` |
| `[Security] AdministratorTotpSecret` - COM `Settings.EnrolAdministratorTOTP` and `DisableAdministratorTOTP`, `POST` and `DELETE /api/v1/administrator/second-factor` | Refused with a sentence (409 over REST) | To turn it off, delete the line with an elevated editor and restart. **Enrolling needs an account that may write the file**: the server makes the secret and hands it out once, so move the service to LocalSystem for the enrolment and back |

`Bin\hMailServerApiKeys.ini`, a file of its own, is still written (above).

The server decides by asking the file - opened for writing and closed at once -
not by the account's name, so a service that may write it (LocalSystem, an
account an administrator granted) behaves as before. The Linux packages already
install `/etc/hmailserver/hMailServer.ini` as `0640 root:hmailserver`; the same
handling applies there, and a setting saved is no longer an error at every start.
The `[Settings]` copy is stale for anything that reads the file itself rather
than the database: the server when its table cannot be read, the installer (for
`ShutdownDrainSeconds`), the Control Panel when it has no COM connection, and a
person opening the file.

## `/Register` takes the account from its command line

`hMailServer.exe --service-account virtual|localsystem|keep|DOMAIN\name$ /Register`.
Without `--service-account` it keeps the account a service has, and makes a new
one LocalSystem. `DOMAIN\name$` is a group managed service account, which has no
password. Any other account has one, and is refused: it is set in `services.msc`
(the service's *Log on* tab), and every upgrade keeps it.

`ServiceAccountName` and `ServiceAccountPassword` are no longer used: the server still
loads them with its other settings (`IniFileSettings.cpp`), and nothing reads them. They were
what `/Register` used, and they are written by others: they are rows of the
settings store, which hMailServer administrators - who need not be Windows
administrators - set over REST and COM, and which the service writes; and the file
holding their copy is written by whichever account last could. A registration
that read them would let any of those make the next elevated `/Register` give the
service LocalSystem back. Asking whether the file is writable by administrators
alone would answer for one of the two places the value comes from, so the value is
not read at all. The Control Panel no longer edits them; its *Setup checklist* page
asks for a stored password to be deleted.

## What an administrator's command writes

`hMailServer.exe` is the service, and also the commands an administrator runs:
Setup, DBSetup and the Control Panel run `/Register`, `/RegisterTypeLib`,
`/Unregister`, `/GrantServiceAccess`, `--grant-database-login`,
`--apply-new-install-defaults`, `--apply-profile`, `--create-stored-secrets-key`,
`--set-admin-password` and the live update's two elevated, and an administrator
runs the rest - `--upgrade-database`, the stored-secrets commands, the store and
object-store commands, `--apply-environment`, `--check-database` - from an
elevated prompt. Each reads `[Database]` or `[Security]` from `hMailServer.INI`, or
changes the service, so each runs with the administrator's rights; and the folders
the service is granted Modify on - `Logs`, `Data`, `Temp`, `Database` - are folders
a program running as `NT SERVICE\hMailServer` can put a junction or a mount point
in. A file an administrator's process wrote there by its path could be written
wherever that leads.

So, since 6.3.6:

* **No log, no event, no crash record.** Every command line but `RunAsService`,
  `/Debug` and `/Test` forbids the logger's writes as the first thing it does
  (`ForbidServiceFolderWrites` in `hMailServer.cpp`, `Logger::
  ForbidWritesForThisProcess`): no line reaches the error log or any other log, the
  Application log, the `OnError` event script or the SQL log device; and it arms no
  crash oracle, whose record and dump go to the log folder. What each command did,
  and why it failed, is on its own standard output and standard error, where it
  already said it; Setup, DBSetup and the Control Panel keep that in their logs.
  `/Register` says there why a registration failed. Until 6.3.6 only the live
  update's verbs, the grant and `--grant-database-login` did this; the others wrote
  their errors to the error log, and every one armed the crash oracle.
* **No junction followed where Windows can refuse it.** On Windows 11 22H2 and
  Windows Server 2025 and later the same commands follow no junction that a user
  who is not an administrator made (`ProcessRedirectionTrustPolicy`); an older
  Windows refuses the request, and the next two rules hold there alone.
* **The new installation's certificate is made through held folders.**
  `--apply-new-install-defaults` writes the self-signed certificate and its private
  key into the data folder's `Certificates` folder. It holds every folder of that
  path open from the drive's root down - none of them a junction, a symbolic link
  or a mount point, none renamed or removed while held - makes `Certificates` and
  each file relative to the folder above, never through a link of its name, and
  checks each file it made to be in that folder before writing a byte to it. The
  key's permissions - SYSTEM, the Administrators, the account running it and Read
  for the service's account - are given when it is made, not set by its path later.
* **A store command that changes the data folder, or copies it, finds no link in
  it first.** `--compress-store` (and `--decompress`), `--encrypt-store`,
  `--decrypt-store`, `--recount-store`, `--copy-store-from-object-storage` and
  `--copy-store-to-object-storage` reach each message by its path. Before anything
  is read or changed the data folder is held from its drive's root down and every
  folder and file in it listed once; a junction, a mount point or a symbolic link
  anywhere in it stops the command, exit 1, naming it - nothing in the data folder
  is ever one. A file that is only tiered or deduplicated is not a link. A copy to
  the object store that removes nothing is held to it too: it reads with an
  administrator's rights and hands what it read to a store the service's account
  reads. `--measure-store` only reads, and says what it read to the administrator
  alone.
* **A store command reaches no file outside the data folder.** The names it
  rewrites, removes and copies over are the database's - each message's file name,
  each deleted item's, each held part's - and, on an object store, its keys: both
  are the service's to write. A row whose file is outside the data folder - a full
  path elsewhere, or a part that is `..` - stops every store command, naming the
  message, before it reads or changes a file; a key whose name would lead out of it
  is not listed; and a held part's name is its SHA-256 in hex or names no file.

What is left: a program that still runs as the service's account after the service
has stopped could make a link while a long store command runs, after the folder was
listed; on Windows 11 22H2, Windows Server 2025 and later the command does not follow
it. The SQL Server Compact database file in `Database` is opened by its path by the
commands that open the database; that engine opens only a file that is a database,
and creates none.

`build/check-elevated-verbs.py` holds the commands to these rules and lists them:
it refuses a command line added to the three that may log, a dispatch before the
decision, the crash oracle armed for any other, the certificate written by its path,
and a store command dispatched without the check. `--self-test` breaks each rule in
a copy and proves the check reports it.

## What LocalSystem could do and the virtual account cannot

Every right the service used that only LocalSystem - or an administrator - has,
and what changed:

| What | Under LocalSystem | Under `NT SERVICE\hMailServer` | How |
|---|---|---|---|
| Its own folders | Anything on the machine | The grants above | `/GrantServiceAccess` |
| `hMailServer.INI` | Written | Read only | The section above |
| A file you name that is outside them - a certificate, a key, a DKIM key file, a message to import, a backup to restore | Read wherever it is | Read only where the account may | The grant lists the configured ones as `OUTSIDE`; move them into `Data\Certificates`, or approve their folder for Read |
| The Windows event log's source (`HKLM\SYSTEM\...\EventLog\Application\hMailServer`) | Registered at the first error forwarded | Cannot write HKLM | `/Register`, which runs elevated, registers it; the running service only reads it (`WindowsEventLog::RegisterSource`). Writing to the Application log itself is allowed to every service |
| The COM AppID (`HKCR\AppID`) | Re-written at every start | That write fails, silently and harmlessly | `/Register` writes it, as it always has |
| The DCOM access and launch permissions | Unchanged | Unchanged | They are about the callers - the Control Panel, scripts - who stay the administrators |
| Changing its own service entry (COM `Utilities.MakeDependent`, which DBSetup uses to depend on a local database service) | Worked | Refused | The installer sets the database up while the service is LocalSystem; DBSetup run later falls back to `sc.exe config hMailServer depend= RPCSS/<service>` itself, as an administrator. The right is deliberately not granted: a service that may rewrite its own entry may make itself LocalSystem again |
| The live update | The helper runs the installer | The service cannot run it | The update is still checked, downloaded and verified; an administrator installs it with *Install update* in the Control Panel on the server, after Windows' prompt (below) |
| The auto-ban's Windows Defender Firewall rules (`AutoBanFirewall`) | Written | Refused | Writing a firewall rule takes an administrator and there is no narrower right to grant. Said once in the error log (HM7460) and in the checklist; banned addresses are still refused by the server. `AutoBanCommand` still runs - as the service account, so a command that asks a router or a cloud firewall works and one that runs `netsh` locally does not. Rules a LocalSystem run left behind cannot be removed by the new account, and are named once (HM7461) with the PowerShell that removes them |
| A port | Any | Any | Windows lets any account bind any port, and the firewall's rule is per program: nothing to grant |
| The secret store | Machine-scope DPAPI | The same | Any account on the machine can decrypt it - which is why shared hosting stays declined |
| SQL Server with integrated authentication, on the same machine | The login `NT AUTHORITY\SYSTEM` | The login `NT SERVICE\hMailServer` | `hMailServer.exe --grant-database-login` (above) makes it, a user for it in the hMailServer database and `db_owner` - which the installer runs before the move, as the administrator running setup; where that account may not, the service stays on LocalSystem and setup gives the statements for a SQL Server administrator |
| SQL Server with integrated authentication, on another machine | The computer account, `DOMAIN\COMPUTER$` | The computer account, as before | A virtual account is the computer account on the network, so the login that worked keeps working. For an identity of the server's own rather than the whole computer's, run the service as a group managed service account (`--service-account DOMAIN\hmail$ /Register`) and give that a login |
| MySQL, MariaDB, PostgreSQL | A user and password | The same | Nothing changes |
| A backup written to a network share | The computer account | The computer account | Grant `DOMAIN\COMPUTER$` on the share and its folder, as for LocalSystem |
| A backup or archive folder on this machine, set after the move | Written | Written once approved | Grant access lists it as not approved and asks |
| A custom virus scanner, ClamWin | Ran as LocalSystem | Runs as the service account | It reads the message in the temp folder, which is granted, and its own program and signatures, which the Users group can normally read. A scanner that needs more has to be granted it |
| ClamAV's `clamd`, SpamAssassin's `spamd` | Over TCP | Over TCP | Nothing changes: the message is streamed, not named by path |
| Event scripts | Ran with LocalSystem's rights | Run with the service account's | A script that writes files, starts programs or reaches a database with integrated authentication can do only what the account can |
| Active Directory sign-in (`LogonUser`, LDAP bind) | Worked | The same | No privilege is needed for a network logon. Kerberos acceptance with no explicit account (`GssapiAcceptor`) uses the computer account's keys under both; this is expected and not yet exercised on a domain-joined bench |
| Crash dumps | Written to the log folder | The same | `hMailServer.Minidump.exe` runs as the service account and writes to the granted log folder |
| "Log on as a service" | Implicit | Held through `NT SERVICE\ALL SERVICES` | A domain policy that replaces the right has to keep that group, or the service reports error 1069 and does not start |

## The live update

The helper that applies an update (`hMailServer.Updater.exe`) is started by the
service and runs as the service's account, and the installer it runs needs an
administrator. On LocalSystem - an upgraded installation that kept it - nothing
about the live update changes: the service hands the installer to the helper, in
the update window or when *Install update* is clicked, as
[UpdateFeed.md](UpdateFeed.md) describes.

Under an account that is not an administrator the service still checks for the
update, downloads it and verifies it, and from 6.3.6 **an administrator installs
it with one click**: *Install update* on the Status page of the Control Panel on
the server. Windows asks for permission once; everything after that happens by
itself, and the Control Panel comes back signed in, on the new version, when the
service is running. The service never runs anything as an administrator, and
nothing it can write decides what the administrator's side runs.

### What happens after the click

1. **Windows' prompt.** The Control Panel starts `Bin\hMailServer.exe
   /InstallUpdate` of its own installation elevated (`runas`), for the release the
   Status page shows. Only a Control Panel on the server does this: one connected
   to another computer says to open the Control Panel there, and so do the Control
   Deck and `POST /api/v1/update/install` (409, code
   `update_install_by_administrator`). A Control Panel that is not the
   installation's own - a copy run from elsewhere - says to open the installation's.
   If the prompt is declined, nothing has changed anywhere.
2. **The elevated stage checks the installation.** It finds the installation from
   the service's registration and `hMailServer.INI`, which only administrators can
   write, never from what the service says, and checks that nobody but SYSTEM, the
   Administrators and TrustedInstaller can change what an administrator's process
   will run or read there: the folders above the program folder and the program
   folder itself (deleting in them, deleting them, or changing their permissions or
   owner), and `Bin` and `ControlPanel` with every folder, program and library in
   them, `hMailServer.INI`, `AdminUpdate` and the trust files `hMailServer.INI`
   names (any write). Something others can change stops the install before
   anything has changed, and the Control Panel
   names the folder, the account, the right and the `icacls` command that removes
   it: a program run from there as an administrator could be one that account put
   there. The same rule is checked by the Control Panel before the prompt, from a
   table of cases both sides are tested against
   (`build/testdata/install-permissions-cases.json`).
3. **It copies the installer out of the service's reach.** `AdminUpdate\<id>` in the
   program folder - owned by the Administrators, SYSTEM and Administrators alone may
   change it, the administrator who clicked may read it - is made new for each
   install, and the installer and its Sigstore bundle are copied into it from
   `Data\Updates`, which the service writes. `AdminUpdate` itself lets anyone read
   its permissions, and nothing else of it, so that the Control Panel of every later
   install can hold it to the rule before the prompt. A name that leads through a
   link or a junction, a folder where a file should be, or a copy bigger than 1 GB
   is refused. The copies are held open, so that nothing can change them between
   the check and the install, and everything below is checked on the held bytes.
4. **It checks the installer itself**, trusting nothing the service said about it:
   - the **Authenticode signature**, the primary one, with Windows' own
     verification, and the **publisher pinned**: the certificate names exactly
     `CN=Progressive Robot Ltd, O=Progressive Robot Ltd, L=Chester, S=Cheshire West
     and Chester, C=GB`, its chain ends, by SHA-256, in Microsoft ID Verified Code
     Signing PCA 2021 and Microsoft Identity Verification Root Certificate Authority
     2020, it is issued for code signing, and the signature carries a timestamp. A
     file that Windows trusts but another publisher signed is refused. Where the
     installed `hMailServer.exe` is signed by somebody else - a site that builds and
     signs its own - the installer is held to that signer instead, the same name
     through the same root, and where the installed one is not signed at all there
     is nothing to hold it to, and the one-click install is refused. A name is
     compared whole: one with a NUL in it is refused, never read up to the NUL;
   - **revocation**, online, of the whole chain, within 30 seconds: a certificate
     that is revoked is refused, and so is one whose revocation could not be
     checked - said as the revocation server not being reachable, never as revoked.
     Windows' own answer is not enough for that: its Authenticode policy takes a
     revocation server that does not answer as good, so the stage reads the
     revocation status of each certificate in the chain Windows built, the root
     aside, and refuses where one is unknown or was checked offline (on the
     installer smoke VM before 6.3.6 was released, a revocation server held
     unanswered let an installer through as "revocation checked online"; with
     its outbound HTTP blocked, Windows answered 0 for the real 6.3.5 while each
     certificate of its chain below the root said its revocation was unknown and
     checked offline, and with HTTP open and nothing cached the same check
     fetched Microsoft's lists and passed). The chain read is the signer's; the
     timestamp's own chain is left to Windows' answer. So whoever can keep this
     computer from the revocation server - `www.microsoft.com`, where Microsoft
     publishes the lists for the project's certificates - can stop the one-click
     install: a firewall that lets out only the download, a proxy the check
     cannot pass, or somebody on the network between. That is a denial, refused
     before anything is changed, never an install of anything else; the
     installer can still be run by hand, where Windows' own check, which takes
     an unreachable revocation server as good, is what applies;
   - the **version** in its version resource is the release offered, the
     **architecture** in its description (`hMailServer Setup (x64)` or `(ARM64)`)
     is this installation's, and the release is **newer** than the running
     server, whose version is compiled into the stage;
   - the **Sigstore bundle** - the pipeline for that release's tag, and the
     transparency log - checked by a second copy of `hMailServer.exe` that the
     stage starts with no privilege but change-notify, the Administrators group
     and every other group UAC takes from an administrator (Backup Operators,
     Domain Admins and the rest) made deny-only, at low integrity, in a job that
     holds it to one process and 512 MB, and that is given the bundle and the
     installer's SHA-256 through a pipe and nothing else. The bundle is parsed
     there, not in the elevated process, and a child that a crafted bundle took
     over could write nothing of the administrator's: at medium integrity, the
     administrator's own unelevated level, it could still have written their
     profile and registry, which their programs and Windows' auto-elevating ones
     read. Its answer is not what the install rests on - the Authenticode pin is.
5. **The service prepares, on the administrator's request** (COM
   `Status.PrepareUpdateForAdministrator`, asked from this computer only): it
   checks its own download again, takes the backup (`UpdateBackupBeforeApply`, to
   the destination *Backup & restore* sets) and fetches the running version's
   installer as the rollback image. The stage copies the rollback image out and
   checks it as it checked the installer, held to the running version. A backup
   that fails asks the administrator whether to install without one; with no
   destination set, the window says the rollback puts the program back and not the
   database. Nothing elevated takes anything from the service's answer: the
   service's part is the backup, the drain and the token.
6. **The install begins** (`Status.BeginUpdateForAdministrator`): the service
   issues the database upgrade's single-use token, closes its listeners and lets
   the mail in progress finish (the drain), and the stage hands over to a copy of
   `hMailServer.Updater.exe` in the work folder, checked by its SHA-256. The
   Control Panel closes, so that the installer can replace it.
7. **The Updater installs** as the administrator: it ends every program still
   running from the installation, runs the held installer silently - the token in
   its environment, not on its command line - with its temporary files in the work
   folder, waits for the service to come back, runs the held rollback image if it
   does not, or if the installer ends with 10, but never once the installer's
   database step has changed the database (exit code 11, `upgraded-incomplete`;
   [Upgrading.md](Upgrading.md), "If the upgrade fails"), and asks Windows to hold
   off a sign-out or a shutdown while it works
   (the reason shows on the sign-out screen). It writes the
   outcome into the work folder, which only administrators can write, puts an
   entry in the Application log (HM7466 when it starts, HM7467 with the outcome),
   and opens the Control Panel again as the administrator who clicked, not
   elevated: as a child of that user's desktop shell, `explorer.exe` in the same
   session, with the shell's own token.
8. **The Control Panel comes back** signed in, from a handoff it wrote before it
   closed - DPAPI for the Windows user who clicked, bound to a key only that user
   and the Administrators can read in the work folder, good for 30 minutes and
   deleted when it is read - and shows the outcome. Where the administrator
   password has a second factor it asks for the sign-in instead. The service
   reports the outcome at its start, in the application log and the audit trail
   (object type `update`, interface `Windows`, the Windows account that approved
   it; [AlertsAndAuditTrail.md](AlertsAndAuditTrail.md)).

The whole of the elevated side writes only into the work folder, never into
`Data`, `Logs` or anything else the service can change, and `hMailServer.exe` loads
the libraries it links from `Bin` and System32 only (`/DEPENDENTLOADFLAG`). A
stage that finds another administrator's install under way on the server says so
and does nothing. `{app}\AdminUpdate` is removed by the uninstaller.

### The token

The token the installer's database upgrade presents is in the `Updates` folder, in a
file the service can write (`ProtectedFile`: SYSTEM, the Administrators and the
service), and a token lets its holder sign in over COM as the server's
administrator. That is not a way for the service to reach an administrator: a
service that could forge it holds everything the server's administrator can do
already, and redeeming it runs nothing elevated. In an administrator's install a
service that rewrote it could at worst make the genuine installer's database
upgrade fail, and the Updater put the previous version back - a denial, not a
compromise. The Updater reads the file only if it is no link, sits where it is
expected and holds exactly 64 lowercase hexadecimal characters, and gives it to a
6.3.6 or later installer in its environment (`HM_UPGRADE_TOKEN`), so that it never
appears on a command line - nor in the installer's log, which Inno Setup begins with
the command line it was started with; the installer reads it and removes it from its
own environment at once. The live update of a service running as LocalSystem gives it
the same way from 6.3.6; up to 6.3.5 it was on the installer's command line, and so
in `Updates\install-<version>.log` - a single-use token, spent once the installer's
database step had run, in a folder only SYSTEM, the Administrators and the service
can read.

### Why this and not a scheduled task

A scheduled task running as SYSTEM, which the service could trigger, was
considered and not built. The only file it could run is one in a folder the
service writes, so to be safe it would have to copy the installer somewhere the
service cannot reach, verify it there itself - not trusting the service's word,
which is the whole point - pin the publisher, refuse an older version, and write
its log and outcome nowhere the service could turn into a link to a system file.
That is a second updater running as SYSTEM, and a service that can make SYSTEM run
a file has given back what the account took away.

The administrator's install does every one of those checks, and differs from the
task in what starts it: an administrator's click at the server, and Windows'
prompt, which the service can neither cause, show nor answer. No elevated program
waits for the service or listens to it; the elevated stage starts from the click
or not at all, and what it runs is what it checked itself.

### From 6.3.4 and 6.3.5

A server on 6.3.4 or 6.3.5 under this account cannot install 6.3.6 this way: the
refusal is compiled into those versions, and their *Install update* asks the
service alone. It is installed by hand once - download the installer from the
release page, check that `Get-AuthenticodeSignature` reports it `Valid` and signed
by `CN=Progressive Robot Ltd`, and run it as an administrator, keeping the service
account on the installer's page. Windows SmartScreen may warn about a file
downloaded with a browser that it has not yet seen often; the signature is what to
check. From 6.3.6 on, each update is the one click above, and never meets
SmartScreen: the installer is not downloaded by a browser, and the Updater starts
it directly.

The unattended update window does not install under this account: when the window
is open and an installer is verified, the error log says once per process that an
administrator installs it with *Install update* in the Control Panel on this server
(HM7463), and no backup is taken for an install that cannot happen.

## Where you see it

* **The checklist** (`GET /api/v1/setup/checklist`, [SetupChecklist.md](SetupChecklist.md)):
  the tenth item, `service_account`. `service_account_local_system` (on Linux
  `service_account_root`) is *action needed* while the server runs as the most
  privileged account - a recommendation, as the missing profile is, never an
  error; `service_account_own` names any other account and is done. Under an
  account that is not an administrator, `autoban_firewall_not_permitted` is added
  while `AutoBanFirewall` is on, and `update_apply_by_administrator` while the
  update check is. The server judges from its own token, not from what the
  service manager was asked for.
* **The Control Deck's dashboard** shows the same finding, as information, while
  the server runs as LocalSystem or root, with a way to the checklist.
* **The Control Panel**, *Server limits & expert settings*, card *Windows service
  account*: what the service runs as, and three buttons, each run after the
  elevation prompt with what it said shown afterwards - **Grant access**
  (`--confirm /GrantServiceAccess`), **Move to NT SERVICE\hMailServer** (the same
  grant, then `--service-account virtual /Register` only if it ended in 0) and
  **Move to LocalSystem** (`--service-account localsystem /Register`). *External
  setup* carries the same item, and asks for a stored `ServiceAccountPassword` to
  be deleted.

## Moving it, and going back

To move a service that runs as LocalSystem to the virtual account: run the
installer again and choose it, use the Control Panel's button, or from an elevated
Command Prompt in the `Bin` folder

```
hMailServer.exe --grant-database-login
hMailServer.exe --confirm /GrantServiceAccess
hMailServer.exe --service-account virtual /Register
net stop hMailServer && net start hMailServer
```

running each line only when the one before it ended in 0. The first matters only
for SQL Server with Windows authentication, and says "Nothing to do" otherwise;
the Control Panel's button does not run it, so on such a database run it, or the
statements it gives, before the button. To go back to LocalSystem:
run the installer again and choose *Move it back to LocalSystem*
(`/serviceaccount=localsystem` silently), use the Control Panel's button, or

```
hMailServer.exe --service-account localsystem /Register
net stop hMailServer && net start hMailServer
```

The grants and approvals stay behind and do nothing under LocalSystem;
`icacls <folder> /remove "NT SERVICE\hMailServer" /T` takes the grants away, and
deleting the `ServiceAccessPaths` key the approvals, if you want them gone.

## Error codes

| Code | Severity | Said when |
|---|---|---|
| HM7460 | Medium, once per process | `AutoBanFirewall` is on and the account cannot write firewall rules |
| HM7461 | High, once per process | Auto-ban rules a LocalSystem run left in the firewall cannot be removed by this account |
| HM7462 | Medium | `/Register` could not set the service's SID type to unrestricted |
| HM7463 | Medium, once per process | The update window is open and the update cannot be applied unattended by this account: an administrator installs it with *Install update* in the Control Panel on this server |
| HM7464 | Medium | `/Register` was given an account it does not set - one with a password - and registered nothing |
| HM7465 | Medium | An administrator's install did not stop the service within 10 minutes of its start: the drain is left, the token revoked, and the server accepts mail again. The Updater's log in `AdminUpdate` says how far it got |
| HM7466 | Information, in the Windows Application log | An administrator's install has started: who, which version over which, and the work folder. Written by `hMailServer.Updater.exe`, with the administrator's account as the entry's user |
| HM7467 | Information, a warning or an error, in the Windows Application log | An administrator's install ended: installed, cancelled or refused (a warning), or not installed, rolled back, with its rollback failed, or installed with its database upgraded and a step not complete - `upgraded-incomplete`, not rolled back - (an error), with the reason |
| HM7468 | Medium | The service's preparation of an administrator's install failed: the step (the check of its download, the backup, the rollback image) and the reason |
| HM7469 | Medium, once per process | The outcome of the last administrator's install could not be read, or an install that began more than an hour ago wrote none - the Windows session it ran in may have been signed out |

None of HM7460 to HM7464 fires on a server as it is shipped: the firewall half of the
auto-ban and the unattended update window are both off until they are switched on.
HM7465 to HM7469 come only from an administrator's install. An administrator's install
that did not succeed is also reported as HM5480, as the unattended apply's is - and one
whose database step changed the database before a step did not complete, which nothing
puts back, as HM5481 ([Upgrading.md](Upgrading.md), "If the upgrade fails").

## Tests

* `ServiceAccountTester` (`Common/Util/ServiceAccount.cpp`), in the internal suite
  `Utilities.RunTestSuite` runs: the service SID against the one Windows publishes
  for TrustedInstaller; which folders a grant may reach, on a machine made up for
  the purpose - `[Directories] DataFolder` pointed at `Bin`, at the program folder
  and at `Data\..\Bin`, the Windows folder, Program Files, a profile, a share, a
  device path, a stream, a drive's root, all refused; the program's own four and a
  folder of its own on another drive, not - and the plan made from them: a
  malicious data folder refused and not granted, an unapproved backup folder
  listed, an approved one granted, an approval of the Windows folder refused, a
  key file outside listed and one inside read on the file alone; the logon
  `/Register` passes for each `--service-account`, an account with a password and
  LocalSystem under another name refused; a grant on a directory of its own under
  the temp folder, made, made again with nothing changed, the one explicit entry
  it leaves, handed down to a file that was there before and one made after, an
  administrator's own entry kept; a file with protected permissions granted
  without losing its protection or its other entries; a hard-linked file refused;
  a path through a symbolic link refused where one can be made; and a held folder
  that cannot be removed.
* `Installation/InstallerPackaging.cs`: the page's three forms and its defaults,
  `/serviceaccount=`, the word `/Register` is given and that `hMailServer.exe`
  knows it, that nothing reads `ServiceAccountName` any more, the registration
  before the database step and the move after it, the move only on a grant that
  ended in 0, `--confirm` in an attended installation, and a failed grant's own
  message.
* `API/RestApiSetupChecklist.cs`: the tenth item, judged from the server's own
  token and, beside a running service, checked against the account `sc.exe qc`
  reports.
* The administrator's install: `AdminUpdateTester` (`Common/Util/AdminUpdateTester.cpp`)
  holds every check of the elevated stage to its negative - the permission rule
  against the table of cases the Control Panel's `InstallationPermissionsTests`
  reads too; the work folder made new and an old one, one with others' rights and
  a junction refused; the copy through a junction, a link and a stream refused;
  the architecture marker; the pin alone, a subject with an attribute more or
  less, another chain, no code-signing use; and the real signed installers of
  6.3.1 to 6.3.5 (fetched onto the bench by `build/fetch-installer-fixtures.ps1`,
  checked by their SHA-256) passing, with 6.3.0 (unsigned), a byte changed, a file
  cut short or grown, the .NET runtime's installer (another publisher, trusted
  by Windows) and a catalogue-signed system file refused, the site-signer path
  held to the installed signer's name and root (the same name through another
  root refused), a name with a NUL in it refused, a revocation check with no
  time to run refused as not reachable, and a chain whose revocation status is
  unknown or offline below its root refused while one with only its root
  unchecked passes; and the lowered child's token read back -
  the Administrators and every group UAC filters deny-only, no privilege but
  change-notify, low integrity - its job's limit, its timeout and its output cap. `AdminUpdateStageTester`
  holds the stage's request to its child (a round trip, and every malformed
  frame refused), the child's answers, the Updater's outcome file, and the real
  6.3.5 bundle verified through the child as the stage runs it. The Control
  Panel's `AdministratorUpdateTests`, `AfterUpdateHandoffTests` (a handoff
  without its key, another user's, an old one and a second read refused) and
  `UpdateStatusTextTests`; `API/UpdateApply.cs`, which on the bench's LocalSystem
  service holds the administrator's path shut, and where the service is another
  account holds the 409 and the state; and `build/check-elevated-verbs.py`,
  which fails when a verb that runs elevated reaches the log or the crash
  handler. The whole install, with Windows' prompt, is run by hand on the
  installer smoke VM.
* The live registration - a new installation under the virtual account, an upgrade
  that keeps LocalSystem, a move each way, the question the grant asks - is
  exercised by hand on a throwaway machine; nothing here registers a service on
  the bench.
