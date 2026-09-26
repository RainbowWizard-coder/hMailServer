# A new installation is secure before anyone configures it

Until 25 September 2026 a new installation listened on ports 25, 587, 110 and
143 with no TLS at all, and neither IP range it shipped with required TLS for
authentication - so a mail client that signed in from another machine sent its
password across the network in the clear, from the first minute, until an
administrator noticed. The create scripts still make exactly that database. What
changed is what the **installer** does with it, and what the **Linux packages'
first start** does with it: before the server first listens, a new installation
is given a certificate, STARTTLS on every listener, and a rule that a password
from anywhere but this computer waits for TLS.

## What a new installation starts with

| | Before | A new installation now | An upgraded server |
|---|---|---|---|
| Port 25, SMTP | no TLS | STARTTLS offered, optional | as it was |
| Port 587, SMTP submission | no TLS | STARTTLS offered, optional | as it was |
| Port 110, POP3 | no TLS | STARTTLS (STLS) offered, optional | as it was |
| Port 143, IMAP | no TLS | STARTTLS offered, optional | as it was |
| IP range *Internet* (every IPv4 address) | a password taken without TLS | **TLS required for authentication** | as it was; the checklist flags it |
| IP range *My computer* (127.0.0.1) | a password taken without TLS | unchanged | as it was |
| Certificates | none | *Self-signed (made at installation)* | as they were |

So, on a new installation:

* **From another machine**, `AUTH` on SMTP is not offered before STARTTLS and is
  refused with `530 A SSL/TLS-connection is required for authentication.` if it is
  sent; IMAP `LOGIN` and `AUTHENTICATE` answer `BAD A SSL/TLS-connection is
  required for authentication.`; POP3 `USER` and `AUTH` answer `-ERR`. After
  STARTTLS, every one of them works as it always has.
* **From this computer** - a webmail on the same machine, a script, a monitoring
  check on 127.0.0.1 - nothing has changed.
* **Mail from other servers** on port 25 is taken with or without TLS, as before.

### Why STARTTLS is optional and not required

On port 25 a requirement would refuse mail from every server that cannot start
TLS, which is still a real share of the internet's senders. On 587, 110 and 143
it would refuse this computer's own clients too, because a port's requirement
applies to every address. The range's rule is the one that protects the
password: it refuses a password before TLS from every address but the loopback,
on every listener, and it is the one an administrator can see and change in one
place. An administrator who wants a port to refuse everything before TLS sets
the port to *STARTTLS (required)*, as always.

## The certificate

`<data directory>/Certificates/self-signed-certificate.pem` and its key
`self-signed-key.pem` (`C:\Program Files\hMailServer\Data\Certificates` on a
default Windows installation, `/var/lib/hmailserver/Certificates` from the Linux
packages), recorded as the certificate **Self-signed (made at installation)**.

* **RSA 2048**, which every mail client of the last fifteen years takes.
* **825 days**, the longest a TLS server certificate may run and still be taken
  by Apple's clients when somebody has chosen to trust it.
* **The subject** names the host, as its common name and its subjectAltName, and
  gives the organisation as *hMailServer, self-signed at installation*. The host
  is the host name setting (the name the server gives itself in its greetings)
  when one is set, else the machine's DNS name; the organisation says where the
  certificate came from, so that the warning a mail client shows names something
  recognisable.
* **The key** is written readable by the server's account and the machine's
  administrators only - on Windows a protected ACL naming SYSTEM, the
  Administrators group, the account that wrote it and, when the service logs on
  as anything but LocalSystem (a virtual account such as `NT SERVICE\hMailServer`,
  or `ServiceAccountName`), that account, read from the service controller; on
  Linux mode 0600 owned by the service's user - as the DKIM rotation's keys are.
  A key that account could not be given is removed, and nothing is changed.

**Mail clients warn about it.** It encrypts - nobody on the network reads the
password - but nothing vouches for it, so a client asks its user to accept it the
first time, and a user taught to click through that warning is a user who will
click through the one an attacker's certificate raises. Replace it:

* **With ACME** (Let's Encrypt), the easy way: set `AcmeEnabled`,
  `AcmeContactEmail` and `AcmeDomains`, publish the names and let the challenge port be reached. When
  the first certificate arrives the ACME client **gives it to every listener that
  holds the self-signed one**, as it gives it to a TLS listener with none - and
  leaves alone any listener holding a certificate somebody chose.
* **With your own**: add it on the certificates page (Control Panel: *SSL
  certificates*; Deck: *Certificates*) and set it on each port.

Until then the setup checklist's *TLS on the submission port* says so
(`submission_self_signed`, below), rather than calling the port done.

## Only a database this installation created

The defaults are applied to a database **this installation created**, and never
to one that existed before, whatever its size. A small server in service - three
accounts, the stock listeners and ranges, never given a certificate - looks from
its configuration alone exactly like a new one, and requiring TLS for its
sign-ins would lock out every client it has that has never used TLS.

So the create step marks the database it makes, with the `hm_settings` row
`NewInstallDefaultsPending`, and nothing else writes it:

| Writes the mark | Does not |
|---|---|
| COM `Database.CreateInternalDatabase` - DBSetupQuick making the built-in database | COM `Database.SetDefaultDatabase` - DBSetup attaching a database that already exists |
| COM `Database.CreateExternalDatabase` - DBSetup when it creates the database | any upgrade, by DBUpdater, `--upgrade-database` or the server's own |
| `hmailserver --create-database` when it runs the create script | `--create-database` finding a database that already holds a schema |

The verb requires the mark and removes it once it has decided, either way, so a
database is weighed once. The mark carries the time it was made, and **the Linux
unit's first start does not act on a mark made more than a day from its clock**,
either way (exit 5, the file removed, the mark kept): the unit runs at the first
start, so an older mark belongs to a server that has run without these defaults
since - a first start that never reached its database, or an image built with the
database and first booted days later - and whose clients, a script relaying
through it among them, may have come to depend on listeners without TLS. A mark
from more than a day ahead is a clock that was wrong when the database was made
or is wrong now - one that jumps forward when the time is first synchronised,
say - and cannot say how old the database is either. **A run by hand has no such
limit**, and neither has the installer's, a minute after its create: it is somebody
asking for the defaults now, and the mark the unit kept is there for it. And it
still asks the shape the create script leaves:

* the four listeners - SMTP 25 and 587, POP3 110, IMAP 143 - on every address,
  with no TLS, no certificate and no client certificate asked for, and no fifth;
* the two IP ranges with the options, priorities and addresses the create scripts
  give them, and no third;
* no certificate at all;
* no account, no domain and no route - none is in a create script - the sample
  domain and its accounts not counted.

Anything else - no mark, or a database somebody has already configured, signed
in to or routes mail through - is left as it is: the verb says which it found and
exits 5. A database it cannot read well enough to tell is not judged at all, and
neither is one whose listeners and ranges it cannot read again just before it
changes them: exit 2, the mark and the Linux file kept, and the next run asks
again. The Internet range is set only once all four listeners offer STARTTLS. The mark is a
record of the database's history, not a setting: it is in no create script, has
no default, is not in the list of what is changed from stock, and is left out of
the XML backup, so that a restore never carries it onto another database.

**An upgrade is never changed**: its database has no mark. The installer does
not even run the verb for one (`IsNewDatabase`: the settings file had no database
before the installation), and on Linux `--create-database` marks nothing it did
not create. A server upgraded from any earlier release keeps its listeners and
its ranges, and the setup checklist tells its administrator what is left (below).

## Where it runs

**The Windows installer**, after the database step of a new installation and
before the profile the last page chose and the service's start - with the
service stopped, since the verb refuses to change a running server (exit 4). On
the built-in database's branch, DBSetupQuick has just created the database, so
anything but 0 is said in a message box (suppressed by `/SUPPRESSMSGBOXES`). On
the external database's branch the installer cannot see whether DBSetup created
the database or attached one - its exit code is 0 for both - and the mark decides:
a database DBSetup created carries it and is secured; one it attached does not,
and the verb's exit 5 is only logged. Every result goes to Inno's log. The
message offers running the verb by hand only where that still would do it - the
mark untouched - and after an exit 5 or 1 points to the checklist instead,
saying for an exit 1 that where the error log says nothing was changed, a second
run still applies them. A
database COM created but could not mark is **HM7512** in the error log, which is
then the reason for the exit 5 that follows. The installation succeeds either way.

**The Linux packages** (`.deb`, `.rpm`). The packages do not create a database:
the administrator names one in `[Database]` and runs
`sudo -u hmailserver hmailserver --create-database`, which creates it (or fills
an empty one), marks it, and leaves a one-line file, `.new-installation`, in the
`DataFolder` the configuration names - `/var/lib/hmailserver` as shipped, or
wherever the administrator moved it before this step. A database that was already
there - a reinstall, a purge and reinstall, a move to a new machine - gets
neither. The unit runs, before every start,

```ini
ExecStartPre=-/usr/bin/hmailserver --apply-new-install-defaults --if-marked
TimeoutStartSec=300
```

`--if-marked` with no file looks for `.new-installation` in the `DataFolder` of
the same configuration, `/etc/hmailserver/hMailServer.ini`, found by reading its
`DataFolder` and `ProgramFolder` and nothing else - not the whole configuration,
whose loader would write its reports (a `PreferredHashAlgorithm` it does not take,
HM5528 or HM5607) into the error log a second time at every start; the first
start of a new installation, which loads it to reach the database, is the one
time they are. It still takes a file named after it
(`--if-marked /path/to/file`), which it then looks for instead. The step exits at
once, opening nothing, when the file is not there - every start but the first
after `--create-database` - and otherwise tries the database once, applies the
defaults to a database marked within a day of its clock, and removes the file. It runs as the service's
user, so the key it writes is the service's. A database that cannot be reached at
that one try leaves the file, so the next start asks again, and the server's own
start says what is wrong with the database; `TimeoutStartSec` gives the one try
room, since the `-` forgives a failure and not a timeout.

**A `DataFolder` outside the unit's `ReadWritePaths`** (`/var/lib/hmailserver`
and `/var/log/hmailserver`) is read-only to the service - `/home` is not even
visible to it (`ProtectHome=yes`) - so the first start can neither write the
certificate under it nor remove the file, and the server cannot write its mail
there either. Add the directory with a drop-in before that first start:

```sh
sudo systemctl edit hmailserver
# [Service]
# ReadWritePaths=/srv/mail
```

`--create-database` says which of these it found when it made the database: that
the first start under systemd secures it, if it comes within a day of
`--create-database`, and a run by hand after that; that it will once the unit may write to
the data directory; or - with a configuration other than the packaged one, or on
a machine systemd does not run, such as a container - that nothing will do it by
itself, and the command that does.

The Arch package installs the same unit, and `--create-database` is the same
command there, so its first start does the same; it needs no maintainer script
for it.

**By hand**, for anything else - the AppImage, the container, a server started
by hand rather than by the unit, or a database whose defaults were left for
later - with the service stopped, and on Linux as the service's user: the verb
refuses to run as root, whose key the service could not read. With a
configuration other than the packaged one, name it (`--config`) before the verb.

```powershell
Stop-Service hMailServer
& "C:\Program Files\hMailServer\Bin\hMailServer.exe" --apply-new-install-defaults
Start-Service hMailServer
```

```sh
sudo systemctl stop hmailserver
sudo -u hmailserver hmailserver --apply-new-install-defaults
sudo systemctl start hmailserver
```

By hand it too changes only a marked database: one made by the create step whose
defaults were never decided - however long ago, since a run by hand has no
deadline: an image first booted days after its `--create-database`, whose first
start left the database alone, is secured this way. **An upgraded server cannot be given these defaults
by the verb**, and neither can one the verb has already answered (exit 5, or an
exit 1 part way): the checklist's *Passwords only over TLS* item (`sign_in_tls`,
below) says what to do, and gives the range's change as one click once the
listeners can start TLS.

| Exit | Meaning |
|---|---|
| 0 | Applied; or, with `--if-marked`, no file and nothing to do |
| 1 | Something could not be made or written; the lines say what was, and the error log has **HM7510** (the certificate could not be made or written - nothing was changed) or **HM7511** (the certificate's record could not be written - nothing was changed - or a listener or the range could not be; the rest stays as it was written). The mark is kept, so where nothing was changed a second run applies them |
| 2 | No database named in the configuration, it could not be opened or read well enough to judge, or its listeners and ranges could not be read again before the change (the mark and the Linux file are kept, and the next run asks again), or (Linux) run as root |
| 4 | Windows: the service is running |
| 5 | Not a database this installation created, not as the create script left it, or - at the unit's start, with `--if-marked` - marked more than a day from this machine's clock, either way: nothing was changed, and the line says why (the application log too, when it is on). In that last case the mark is kept for a run by hand |

Each change is on the **audit trail** under `hmailserver
--apply-new-install-defaults`, as the certificate, port and IP range rows it
wrote, and in the application log as one line. When a listener cannot be given
the certificate, the Internet range is **not** set to require TLS: a listener
with no TLS behind that rule would be one nobody outside this computer could sign
in to at all. The line says which, and the checklist shows what is left.

## The checklist, for every server

The setup checklist ([SetupChecklist.md](SetupChecklist.md)) judges every server
the same way, new or upgraded:

* **Passwords only over TLS** (`sign_in_tls`): for every IP range that lets a
  client connect from an address other than this computer's, whether it requires
  TLS for authentication (done), whether every listener it may use starts TLS
  before a password anyway (done), or neither. When every listener the range
  uses offers STARTTLS with a certificate that is there, the finding is
  `sign_in_plain`, and it carries a **fix**: the `PUT /api/v1/ipranges/{id}` with
  `require_tls_for_auth: true`, which the Control Deck offers as a button,
  *Require TLS for sign-in from this range*. When one of them has no TLS to start,
  requiring it would refuse every client there on that listener, so the finding
  is `sign_in_no_tls` instead: it names those listeners, says to give them a
  certificate and STARTTLS first, carries no fix, and the Deck's button opens the
  Ports view. The Control Panel shows the same item and opens its IP ranges page,
  where the setting is *Require TLS for auth* (*Require SSL/TLS when
  authenticating* in the range's dialog); its advice says to give a listener
  without TLS a certificate and STARTTLS on the TCP/IP ports page first.
* **TLS on the submission port** says `submission_self_signed`, action needed,
  while the port's certificate is the self-signed one - with the sentence that
  says to replace it and that ACME can.

## The regression benches

Nothing the benches are built from has changed. The Windows benches are made by
`build/upgrade-test-database.ps1` and `bench-setup.ps1`, and the Linux suite by
`hmailserver --create-database`: all of them run the create scripts, which still
make the four listeners without TLS and the two ranges without the requirement,
and none of them runs the installer or the unit. A bench whose database the
create path made carries the mark - the Linux suite's does, and a Windows bench
set up afresh by DBSetupQuick would - and that is harmless: nothing runs the verb
against a bench but its own fixture. Every fixture's `PerformBasicSetup` puts the
ports and the ranges back with `TCPIPPorts.SetDefault` and
`SecurityRanges.SetDefault`, which write the classic defaults as they always
have. And the suite's thousands of plain-text sign-ins are made from 127.0.0.1,
which the *My computer* range covers.

The fixture `Security/NewInstallDefaults` makes the bench into a database the
create step has just made - every port deleted and the four put back, the ranges
and certificates reset, its domain deleted, the mark written two days old, which
a run by hand acts on as the unit's start would not - runs the verb with the
service stopped, restarts the service, makes the domain again with an account in
it, and puts all of it back afterwards, step by step, restarting the service
whatever failed. A run killed half way leaves
`C:\HMTest\Data\Certificates`, which `build/preflight-tests.ps1` names as that
fixture's leftover; `-Clean` puts the database back, removes it and restarts the
service.

## What an installation on a clean machine is to check

The regression fixtures drive the verb without the installer. The proof that the
installer and the packages do it is an installation on a clean machine, and these
checks are **not yet written**: the installer smoke test, `build/ci/installer-smoke.ps1`
on the branch `ci-installer-smoke`, is to add them.

* **After a new installation with the built-in database**: the certificate record
  *Self-signed (made at installation)* over COM, its two files under
  `Data\Certificates`, and the key's ACL granting nothing to `BUILTIN\Users`; each
  of 25, 587, 110 and 143 with optional STARTTLS and that certificate; *Internet*
  requiring TLS for authentication and *My computer* not; no
  `NewInstallDefaultsPending` row left; from 127.0.0.1, STARTTLS offered and
  completing on all four (EHLO, IMAP `CAPABILITY`, POP3 `CAPA` with `STLS`) with
  a peer certificate whose subject says *self-signed at installation*; from the
  machine's own LAN address, no `AUTH` in EHLO on 587, `AUTH LOGIN` answered 530,
  IMAP `LOGIN` answered `BAD`, POP3 `USER` answered `-ERR`, and each taking the
  account's password after STARTTLS; the existing sign-ins over 127.0.0.1 passing
  without TLS as before.
* **After an upgrade** from a release that had no such defaults: every port's
  security and both ranges exactly as they were, no certificate record made, and
  the checklist over COM (`Utilities.GetSetupChecklist(false)`) answering
  `sign_in_tls` as action needed with `sign_in_no_tls` for *Internet*, and no fix.
* **After a new installation attached to an existing external database** (DBSetup,
  *Select a new hMailServer default database*): nothing changed, and exit 5 in
  Inno's log.
* **After the uninstall**: `Data\Certificates` kept with the rest of the data
  directory.

The Linux packaging job of the GitHub workflow `linux-build.yml` (in the git
history at `6399e08a5`) has these checks for the `.deb` - the file and the mark
removed, the record, the four listeners, the two ranges, the key's owner and
mode, STARTTLS on all four, and `AUTH` refused before STARTTLS from the runner's
own address and offered after it - but it ran only on GitHub Actions, and GitLab
CI's `linux-package-deb` job does not carry them yet, so nothing runs them
today.

## Where the code is

`Common/Application/NewInstallDefaults.{h,cpp}` (the verb, the certificate, the
mark and the test for a new database; `CertificateDirectory()` names the folder
whatever grants a service account its folders must also grant, since the key's
ACL inherits nothing); `hMailServer.cpp` and `platform/main.cpp` (the verb on each
platform, and `--create-database`'s mark and file); `COM/InterfaceDatabase.cpp`
(the two create methods' mark); `Common/Application/PropertySet.cpp` (the mark
kept out of the backup); `Common/Util/SetupChecklist.cpp` (the item and the fix);
`Common/Util/AcmeClient.cpp` (`ApplyCertificate_`, which takes the self-signed
certificate's listeners over); `installation/hMailServerInnoExtension.iss`
(`ApplyNewInstallDefaults`); `platform/packaging/hmailserver.service`. The
regression fixtures are `test/RegressionTests/Security/NewInstallDefaults.cs` (the
verb, with the service stopped: refused while it runs, applied to the bench made
into a new database, the refusal of a password from a non-loopback address before
STARTTLS and its acceptance after, and exit 5 on a second run, the mark gone) and
`test/RegressionTests/API/RestApiSetupChecklist.cs` (the item, the fix, the
finding with no fix and the self-signed finding).
