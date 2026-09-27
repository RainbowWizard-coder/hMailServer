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
| No hMailServer service yet - a new installation | `NT SERVICE\hMailServer`, or LocalSystem as earlier versions ran it | `NT SERVICE\hMailServer` |
| A service running as LocalSystem - an upgrade | keep LocalSystem, or move it to `NT SERVICE\hMailServer` | keep LocalSystem |
| A service running as any other account - an upgrade | keep that account, or move it back to LocalSystem | keep the account |

An upgrade never changes the account unless the page is answered to. A silent
installation takes the same choice as `/serviceaccount=virtual`,
`/serviceaccount=localsystem` or `/serviceaccount=keep`; without it, a new
installation gets the virtual account and an upgrade keeps what it has. When the
database is SQL Server reached with Windows authentication, the move page says
that the moved service needs a login there first.

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
4. For a move, the service is stopped and `hMailServer.exe /GrantServiceAccess`
   runs - with `--confirm` in an attended installation. By now the database names
   every certificate and key file, so the grant can say which are outside the
   folders it grants. Only when it ends in 0 is the service registered as
   `NT SERVICE\hMailServer`. Anything else - a folder nobody approved in a silent
   installation, a folder that can never be granted, No to the question, a grant
   that failed - leaves the service on LocalSystem, working, and says why; the
   installation still succeeds. A database step that failed leaves it on
   LocalSystem as well, where DBSetup can be run again.
5. The service is started.

A grant that fails at step 2 fails the installation - that service may not start
- and says so in its own words, not the database's.

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
6 the administrator answered No, and nothing was changed. The installer and the
Control Panel move the service only on 0.

## `hMailServer.INI` is read-only to the service

The file keeps four kinds of thing, and under `NT SERVICE\hMailServer` the service
writes none of them. What each writer does instead:

| What wrote the file | Under the virtual account | How |
|---|---|---|
| The `[Settings]` copy of the settings store, at every start, reinitialise and save (`IniSettingStore`) | Not written. The database is the settings store and its values are in use, as ever | Said once per start in the application log, not as an error. A setting removed while the file still has its line is kept stored as its default - which the server treats the same - because a dropped row would be put back from the line at the next start; a key this build does not read is refused with a sentence. Each row records what the file really holds, so an old copy is never taken for an edit, and an edit is named once. A backup restored stores its settings in the database |
| `PasswordPepper`, the one setting kept in the file only | Refused with a sentence | An administrator sets it in the file with an elevated editor and restarts the service |
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
not read at all. The Control Panel no longer edits them; its *External setup* page
asks for a stored password to be deleted.

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
| The live update | The helper runs the installer | Refused | The update is still checked; an administrator installs it from the release page (below) |
| The auto-ban's Windows Defender Firewall rules (`AutoBanFirewall`) | Written | Refused | Writing a firewall rule takes an administrator and there is no narrower right to grant. Said once in the error log (HM7460) and in the checklist; banned addresses are still refused by the server. `AutoBanCommand` still runs - as the service account, so a command that asks a router or a cloud firewall works and one that runs `netsh` locally does not. Rules a LocalSystem run left behind cannot be removed by the new account, and are named once (HM7461) with the PowerShell that removes them |
| A port | Any | Any | Windows lets any account bind any port, and the firewall's rule is per program: nothing to grant |
| The secret store | Machine-scope DPAPI | The same | Any account on the machine can decrypt it - which is why shared hosting stays declined |
| SQL Server with integrated authentication, on the same machine | The login `NT AUTHORITY\SYSTEM` | The login `NT SERVICE\hMailServer` | Create it: `CREATE LOGIN [NT SERVICE\hMailServer] FROM WINDOWS`, then a user for it in the hMailServer database with `db_owner` - before the move, which the installer's page says |
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
service and runs as the service's account; the installer it runs needs an
administrator. So under an account that is not an administrator, the apply is
refused before anything happens - before the rollback image is fetched and before
the drain - with the sentence, which REST, COM and the Deck's Update page show:
the service runs as `NT SERVICE\hMailServer`, which cannot run an installer, so it
checks for updates and an administrator installs them - **downloaded from the
release page, with its Authenticode signature checked** (`Get-AuthenticodeSignature`
reports it `Valid`), then run as an administrator. The copy the server downloaded
is not offered to be run: it is in the Updates folder, which the service itself
can write, and "verified" there is the service's own word. The unattended window
takes no backup for an apply that cannot happen, and says why once per process in
the error log (HM7463).

A scheduled task running as SYSTEM, which the service could trigger, was
considered and not built. The only file it could run is one in a folder the
service writes, so to be safe it would have to copy the installer somewhere the
service cannot reach, verify it there itself - not trusting the service's word,
which is the whole point - pin the publisher, refuse an older version, and write
its log and outcome nowhere the service could turn into a link to a system file.
That is a second updater running as SYSTEM, and a service that can make SYSTEM run
a file has given back what the account took away. An administrator installing the
signed release is simpler and safe.

On LocalSystem - an upgraded installation that kept it - nothing about the live
update changes.

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
hMailServer.exe --confirm /GrantServiceAccess
hMailServer.exe --service-account virtual /Register
net stop hMailServer && net start hMailServer
```

running the second line only when the first ended in 0. To go back to LocalSystem:
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
| HM7463 | Medium, once per process | The update window is open and the update cannot be applied unattended by this account |
| HM7464 | Medium | `/Register` was given an account it does not set - one with a password - and registered nothing |

None of them fires on a server as it is shipped: the firewall half of the auto-ban
and the unattended update window are both off until they are switched on.

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
* The live registration - a new installation under the virtual account, an upgrade
  that keeps LocalSystem, a move each way, the question the grant asks - is
  exercised by hand on a throwaway machine; nothing here registers a service on
  the bench.
