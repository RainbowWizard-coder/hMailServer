<!--
  https://www.progressiverobot.com
  Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
  SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Packaging the Linux build

Everything here is support material for one binary. `hmailserver/source/Server/CMakeLists.txt`
builds `hmailserver` from `platform/main.cpp` and the core, and drives CPack; these
files are what turns that executable into something a machine can be handed.

Nothing here is compiled or shipped on Windows. The Windows server is still built
from `hMailServer.sln` and installed by the Inno Setup scripts under
`hmailserver/installation`.

## What each file is

| File | What it is |
|---|---|
| `hmailserver.service` | The systemd unit. `Type=simple`, running `/usr/bin/hmailserver --foreground` as the `hmailserver` user, with the hardening this program can actually tolerate. Every option it deliberately does **not** set is listed at the bottom of the file with the reason. |
| `hMailServer.ini` | The packaged default configuration, installed to `/etc/hmailserver/hMailServer.ini`. The six `[Directories]` keys point at the packaged layout; `[Database]` is deliberately empty. |
| `hmailserver.logrotate` | Installed as `/etc/logrotate.d/hmailserver`. Uses `copytruncate`, and the file explains at length why that and not a signal. |
| `debian/postinst` | Creates the user, the directories and the systemd enablement. Does not start the server. |
| `debian/prerm` | Stops the server on removal, and deliberately not on upgrade. |
| `debian/postrm` | Undoes the systemd enablement; on purge removes the configuration directory and says plainly what it is leaving behind. |
| `rpm/post` | The `%post` scriptlet: the same work as `debian/postinst`, in the shape rpm expects. |
| `rpm/preun` | The `%preun` scriptlet: stop and disable, but only on a real uninstall. |
| `PKGBUILD` | The Arch package. Builds from the repository with the same CMakeLists, and uses `sysusers.d` and `tmpfiles.d` instead of an install script - which is what Arch asks for now. |
| `build-appimage.sh` | Wraps an already-built tree as an AppImage. Read the paragraph at the top of it before using the result for anything. |
| `hmailserver-update` | The live update's apply step: the Linux counterpart of `hMailServer.Updater.exe`, installed as `/usr/lib/hmailserver/hmailserver-update` and carried inside the AppImage. A POSIX sh script that reads the server's request, checks every line of it, verifies the package again as root, stops the unit, installs (or swaps the AppImage), starts the unit, rolls back when it does not come back, and writes the same one-line outcome the Windows helper writes. `apply`, `check`, `--dry-run`; the header is the specification. |
| `hmailserver-update.path` | Watches `/run/hmailserver/apply-request` as root and starts the service below when the server writes it. Enabled and started by `postinst` / `%post`. |
| `hmailserver-update.service` | A `oneshot` that runs `hmailserver-update apply` as root, outside the server's cgroup, so that stopping the server does not kill the helper. |

The `$1` argument means completely different things to dpkg and to rpm - a verb
in one and a count in the other - and each script says so at the top, because
carrying a maintainer script from one to the other without noticing is the
classic way to write one that never fires.

## Building the packages by hand

### Prerequisites

Debian and Ubuntu:

```sh
sudo apt install cmake ninja-build clang \
     libssl-dev zlib1g-dev libpq-dev libldap-dev \
     libboost-thread-dev libboost-chrono-dev libboost-filesystem-dev libboost-regex-dev \
     rpm file
```

Fedora and RHEL:

```sh
sudo dnf install cmake ninja-build clang \
     openssl-devel zlib-devel libpq-devel openldap-devel boost-devel \
     rpm-build dpkg
```

`clang` is a preference and not a requirement: GCC 13 and later builds the same
tree (`export CC=gcc CXX=g++`), and the CI matrix builds with both. An earlier
version of this page said GCC could not compile `Common/Util/StdString.h`; that
was believed rather than measured, and measurement said otherwise.

`rpm`/`rpm-build` and `dpkg` are only needed to produce the *other* distribution's
package: CPack builds both generators from one tree.

### Configure and build

```sh
export CC=clang CXX=clang++

cmake -S hmailserver/source/Server -B build/linux -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr

cmake --build build/linux
```

`-DCMAKE_INSTALL_PREFIX=/usr` matters more than it looks. `GNUInstallDirs` answers
`/etc` and `/var` for `sysconfdir` and `localstatedir` only when the prefix is
`/usr`; with any other prefix the configuration would be installed to
`<prefix>/etc/hmailserver` and the logrotate rule to `<prefix>/etc/logrotate.d`,
where neither is read.

### The .deb and the .rpm

`CPACK_GENERATOR` is already `DEB;RPM`, so one command makes both:

```sh
cmake --build build/linux --target package
```

or, from inside the build directory, either one on its own:

```sh
cd build/linux
cpack -G DEB
cpack -G RPM
```

The files land in the build directory as `hmailserver_<version>_amd64.deb` and
`hmailserver-<version>-1.x86_64.rpm`, or their `arm64`/`aarch64` equivalents - the
CMakeLists picks the architecture name each packager uses from
`CMAKE_SYSTEM_PROCESSOR`. The names are the ones the server's update checker
builds for its own platform and matches exactly on the release page, so they are
uploaded as they are and never tidied; `.github/workflows/linux-build.yml` attaches
them to the release for the tag itself, and RELEASE.md has the step.

`<version>` is not written anywhere in this directory. The CMakeLists reads it out
of `hmailserver/source/Server/Common/Application/Version.h`, which is the same
header the Windows resources and the installer stamp from; `pkgver` in the
PKGBUILD below is the one Linux copy of the number that cannot be derived, and
`build/check-linux-version-stamp.sh` - a step of the Linux workflow - fails the
build when it disagrees with that header. No version literal belongs in this
README either, which is why the commands below are written with a placeholder.

### Arch

```sh
cp hmailserver/source/Server/platform/packaging/PKGBUILD /tmp/hmailserver-pkg/
cd /tmp/hmailserver-pkg
makepkg -si
```

It fetches the tag `v${pkgver}` from the public repository rather than using the tree
it was copied out of, which is what makes the resulting package reproducible by
somebody who does not have your working copy.

### The AppImage

```sh
hmailserver/source/Server/platform/packaging/build-appimage.sh --build-dir build/linux
```

It packages what is already built, downloads `linuxdeploy` and its AppImage plugin
if they are not cached, and writes `hMailServer-<version>-x86_64.AppImage`. The
Linux workflow builds one on a release tag and attaches it beside the packages.

## What lands where

From the CMakeLists install rules, with the prefix above:

```
/usr/bin/hmailserver                        the server; one file
/usr/bin/tlds.txt                           the public-suffix list, read from the
                                            binary's own directory; without it every
                                            DMARC organisational-domain decision
                                            reports 4335
/usr/bin/dh2048.pem                         the Diffie-Hellman group, also read from
                                            the binary's own directory; without it
                                            every TLS context reports a critical 5603
/usr/share/hmailserver/DBScripts/*.sql      schema creation and upgrade scripts
/usr/share/hmailserver/WebAdmin/index.html  the Control Deck, served at GET / by the
                                            REST listener
/usr/share/hmailserver/WebAdmin/languages/  the Deck's catalogues, one JSON file per
                                            language, served at /deck-lang/<code>.json
/usr/share/hmailserver/licenses/QuickJS.txt the licence of QuickJS, the JavaScript
                                            engine compiled into the binary for the
                                            event scripts (MIT asks for its notice)
/usr/lib/systemd/system/hmailserver.service the unit
/usr/lib/systemd/system/hmailserver-update.path     the live update's trigger
/usr/lib/systemd/system/hmailserver-update.service  and the root service it starts
/usr/lib/hmailserver/hmailserver-update     the live update's apply step (see below)
/usr/lib/hmailserver/autoban-hook           what auto-ban runs to reach the firewall
/etc/hmailserver/hMailServer.ini            the configuration
/etc/logrotate.d/hmailserver                the log rotation rule
```

The two `/usr/bin` data files and the two under `/usr/share/hmailserver` are found
by different mechanisms and it matters which: `tlds.txt` and `dh2048.pem` are read
from the running executable's own directory (`Utilities::GetBinDirectory`), while
`DBScripts` and `WebAdmin` are read from `ProgramFolder` in the configuration -
which this package sets to `/usr/share/hmailserver`. Move either and the
corresponding pair has to move with it.

Created by the maintainer scripts, not by the package payload, and therefore not
removed with it:

```
/var/lib/hmailserver           0750 hmailserver:hmailserver   the mail store
/var/lib/hmailserver/temp      0750 hmailserver:hmailserver   scratch space
/var/lib/hmailserver/events    0750 hmailserver:hmailserver   event scripts
/var/lib/hmailserver/database  0750 hmailserver:hmailserver   unused on this platform
/var/log/hmailserver           0750 hmailserver:hmailserver   the logs
/etc/hmailserver               0750 root:hmailserver
/etc/hmailserver/hMailServer.ini 0640 root:hmailserver
```

**The event scripts** in `/var/lib/hmailserver/events` are JavaScript on this
platform: `EventHandlers.js`, run by the QuickJS engine compiled into the server,
with the script language set to `JavaScript` (`hmctl settings set scripting
--set-string language=JavaScript`). The setting is seeded `VBScript`, as on
Windows, and while scripting is on a VBScript or JScript script is refused when it
is loaded - one line in the error log saying to set `JavaScript` - rather than left
silently unrun. The
objects a handler is given, how a Windows script ports and what a handler may not
do are [EventScripts.md](../../../../docs/EventScripts.md).

Created by the server itself, the first time a stored secret is written, and
therefore in neither list:

```
/var/lib/hmailserver/.hmailserver-secret-key   0600 hmailserver:hmailserver   the stored-secret key
```

It is 32 random bytes and it is the key under which every password the server
has to present to somebody else - route, fetch-account and per-domain relay
passwords, private-key passphrases, the administrator's TOTP secret - is
AES-256-GCM encrypted in the database, as `LINUX1:<base64>`. It stands in for
Windows DPAPI, which this platform does not have, and the INI's paragraph on
`ProtectStoredSecretsWithDPAPI` says what that does and does not promise. Two
things follow for an operator: **back it up with the database**, because a
database restored without it has lost every stored password (the error log names
the file, HM6412, when that happens); and it is refused, with the `chmod` that
fixes it, if its mode lets any account but its owner read it (HM6411). If it
cannot be made or read, the secret is not stored and the log says so - the server
does not fall back to the fixed-key Blowfish scheme on its own.

The configuration is owned by root. It carries the administrator password and
the database password, which is why. Two commands rewrite it, both run as root
and both keep its owner and mode: `--set-admin-password` (step 4 below) and
`--set-setting` (step 7). The service reads it and, since schema 6042, also
tries to keep the `[Settings]` section in step with the settings store in the
database; on this layout it cannot write the file, so a setting changed later
over the REST API or with `hmctl` is stored, and applies when it is saved (or
waits for a reinitialise or a restart, for the keys
[SettingsApply.md](../../../../docs/SettingsApply.md) lists), but its line in the
file lags, and the error log says so (`HM5806` when it is stored, `HM5803` at each
start after) until the file is writable by the service. See *What is still owed*.

## Install and get running

The whole sequence, in order. The package installs the server enabled and stopped
on purpose: it has no database yet, and a mail server that cannot reach one writes
an error to the log every second until somebody notices.

**1. Install the package.**

```sh
sudo apt install ./hmailserver_<version>_amd64.deb      # or
sudo dnf install ./hmailserver-<version>-1.x86_64.rpm
```

If you are using MySQL or MariaDB, install its client library too. The server
opens `libmariadb.so.3` with `dlopen` at run time rather than linking it, so no
package manager will pull it in: `libmariadb3` on Debian and Ubuntu,
`mariadb-connector-c` on Fedora and RHEL, `mariadb-libs` on Arch.

**2. Create the database user, and either let it create the database or create
the database for it.** PostgreSQL:

```sh
sudo -u postgres createuser --pwprompt --createdb hmailserver     # step 4b creates the database, or
sudo -u postgres createuser --pwprompt hmailserver                # the tidier policy: no CREATEDB, and
sudo -u postgres createdb --owner hmailserver hmailserver         # step 4b fills the empty database
```

MySQL 5.7.9 or MariaDB 10.2.2 or later, with the database in `utf8mb4` if you
create it yourself - [MigratingDatabaseBackend.md](../../../../docs/MigratingDatabaseBackend.md#mysql-and-mariadb)
says why:

```sh
sudo mariadb -e "CREATE DATABASE hmailserver CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;
  CREATE USER 'hmailserver'@'localhost' IDENTIFIED BY '<password>';
  GRANT ALL PRIVILEGES ON hmailserver.* TO 'hmailserver'@'localhost';"
```

**3. Edit the configuration.**

```sh
sudoedit /etc/hmailserver/hMailServer.ini
```

Two things have to change, and the file has a paragraph on each:

* `[Database] Type` - `PostgreSQL` or `MySQL`.
* `[Database] Server`, `Database`, `Username`, `Password` and `Port`. `Port` has
  to be written out (`5432`, `3306`); it is put into the connection string as it
  stands and there is no useful default.

Leave `[Security] AdministratorPassword` alone; the next step writes it.

**4. Set the administrator password.** It is read from standard input with echo
off, hashed with PBKDF2 exactly as the Control Panel hashes it on Windows, and
written to the file. The file is `0640` and owned by root, so this runs as root:

```sh
sudo hmailserver --set-admin-password
```

**4b. Create the schema.** `DBSetup` is a Windows tool; on this platform the
server does the same work itself. `--create-database` connects to the server
`[Database]` names with no database selected, creates the database in the
backend's own dialect, and runs the create script for its type:

```sh
sudo -u hmailserver hmailserver --create-database
```

An existing empty database is used as it is. One that already holds an
hMailServer schema is left as it is - the create script never runs over it -
and the command prints its version and exits 0, so a container or a script can
run it at every start; a schema from a newer build exits 1. If the database
already exists and is older than this build, `--upgrade-database` runs every
upgrade script from its version to this one, in order, and refuses a database
from a newer build. The package's post-install
step runs it on upgrade; it is safe to run by hand and reports "nothing to do"
when there is nothing to do. Both commands refuse the two Windows-only backends
by name.

**5. Check it before starting anything.**

```sh
sudo -u hmailserver hmailserver --check-config
```

It reads the file, prints the configuration path, the three directories and the
database type it made of what you wrote, and exits without opening a listener or
the database. A `Database type: 0` means the `Type` key did not take.

**6. Start it.**

```sh
sudo systemctl start hmailserver
sudo systemctl status hmailserver
journalctl -u hmailserver -f
```

The server's own logs are in `/var/log/hmailserver`; the journal carries the
startup line, anything written to standard error, and the operational events -
database down, a listener that could not bind, a failed backup, the disk floor -
which reach syslog through the `WindowsEventLogEnabled` setting. The name of that
setting is a Windows inheritance; on this platform it means syslog.

**7. Administration.** There is no Control Panel here and no COM: the Windows
administration program is an ATL application over an ATL API, and neither is in
this build. What there is instead is the REST API and, on the same listener, the
Control Deck - a single page the package installs at
`/usr/share/hmailserver/WebAdmin/index.html` and the server serves at `GET /`,
with its twenty language catalogues in `WebAdmin/languages` beside it, so the
page comes up in the browser's language, or the one chosen in its header.
Turn on the REST listener with the server's own command, and restart:

```sh
sudo hmailserver --set-setting RestApiPort=8080
sudo systemctl restart hmailserver
```

Not by editing `RestApiPort` in the file. Since schema 6042 the database is the
settings store and the `[Settings]` section of `hMailServer.ini` is the server's
copy of it: a start adopts every `[Settings]` line the store has no row for -
at the first start against a database that is all of them, which is why the
packaged file can ship `RestApiPort=0`, adopted as stored and off - and while
the store has a row, an edit to the file is named in the error log (`HM5804`)
and put back at the next start. `hmctl` cannot help with this one setting,
because it needs the listener it would be turning on. `--set-setting` is the
door: it stores the row the way the REST API stores one, writes the server's
copy into the file, and prints what it stored; it runs as root because the file
is root's, and the file keeps its owner and mode. It refuses a name the server
does not read, a port that is not a number from 0 to 65535, and a
`RestApiBindAddress` that is not an IP address or `localhost`. A setting whose
value is a secret - `SRSSecret`, `OAuth2HmacSecret`, `PasswordPepper` and the
like - is refused with its value on the command line, where the shell's history,
the process list and sudo's log would keep it: give the name alone,
`sudo hmailserver --set-setting SRSSecret`, and type the value at the prompt,
which does not echo it, or pipe it in. Changing `PasswordPepper` locks out every
account whose password is an Argon2id or scrypt hash until its password is set
again, and the command warns when it does. `RestApiBindAddress`
is `127.0.0.1` in the packaged file, and stays so unless you also set a
certificate and key (`RestApiCertificateFile`, `RestApiPrivateKeyFile`, the same
way). Then open `http://127.0.0.1:8080/` in a browser: it signs in as `Administrator`
with the password from step 4 and writes much of what the API can write -
settings, domains, IP ranges, accounts and the external mailboxes they collect
from, rules, routes, certificates, listeners and the backup. Not account
aliases or distribution lists: those have routes and no page, so they are
reached with `curl` or any other HTTP client until the Deck grows a view for
them.

If that page instead says "Web administration page not installed", the file above
is missing: the server falls back to a built-in stub whenever it is not there, and
`hmailserver --check-config` will not tell you, because the page is not one of the
paths it prints. Check the file itself.

Everything the Deck does is the API underneath it, and the API authenticates the
same way. Reloading stops and restarts the listeners inside the running process,
so do it when nothing is mid-delivery. The first domain, and every one after it,
is a `POST` - the name is checked as the Control Panel checks it, and every other
setting takes the default a domain made there gets:

```sh
curl -u Administrator:your-admin-password -H "Content-Type: application/json" \
     -d '{"name":"example.com","active":true,"postmaster":"postmaster@example.com"}' \
     http://127.0.0.1:8443/api/v1/domains
```

It answers `201 {"name":"example.com","active":true,"postmaster":"postmaster@example.com"}`,
`409` when the domain exists and `400` with the reason when the name is not a
domain name. Accounts under it are created over
`POST /api/v1/domains/{domain}/accounts`; `PUT /api/v1/domains/{domain}` with
`{"active":false}` switches a domain off, and `DELETE /api/v1/domains/{domain}`
removes it with its accounts, aliases, lists and directories.

**Proven, 8 September 2026.** Against PostgreSQL 18: `--create-database` built
the schema, the server started and opened its SMTP, POP3, IMAP and REST
listeners, an account was created over the REST API, a message submitted over
SMTP with authentication was delivered, and the same message was read back over
IMAP with its subject intact.

## The live update

> In the tree since 18 September 2026.

The server's live update - the check against the release feed, the download
verified against its Sigstore bundle, the apply with a rollback - was a Windows
feature with a Windows helper. On this platform the first two parts were already
the same code; the apply is now these three files, and this is how it goes.

**Who may do what.** `hmailserver.service` runs as `hmailserver` with
`ProtectSystem=strict`, `NoNewPrivileges=yes` and a capability bounding set of
`CAP_NET_BIND_SERVICE` alone. It cannot run `dpkg`, cannot `systemctl stop`
itself, and cannot use `systemd-run` (that needs polkit's leave to start a
unit, and granting it to the service user would be a broader grant than the
one thing it needs). `systemd-run` is right where the caller is root, and is
what the server tries second - see *An AppImage* below. What the service CAN
do is write a file: the unit gives it `/run/hmailserver` (`RuntimeDirectory=`,
implicitly writable under `ProtectSystem=strict`, removed when the unit
stops). So the apply is a request, and the request is taken by root.

**The sequence.** `POST /api/v1/update/install` (or the Deck's button, or the
`UpdateWindow` opening on a verified download):

1. The server verifies the downloaded package once more, fetches and verifies
   the running version's own package from the release page as the rollback
   image (`Updates/rollback/`), exactly as on Windows, and writes
   `/run/hmailserver/apply-request`: `kind`, `version`, `package`, `rollback`,
   `rollback_version`, `unit` (read from its own cgroup), `outcome`, `log`,
   `wait` (`UpdateServiceWaitSeconds`), and `ready_url` when the metrics
   listener is on loopback without TLS. It waits up to fifteen seconds for the
   file to be taken, then answers state 4 (installing).
2. `hmailserver-update.path` sees the file and starts `hmailserver-update.service`,
   which runs `/usr/lib/hmailserver/hmailserver-update apply --request
   /run/hmailserver/apply-request` as root. The script reads the file and
   removes it (once, whatever happens next), and checks every line: `kind` is
   `deb`, `rpm` or `appimage`; the versions are version-shaped; the unit is
   `hmailserver*.service`; `outcome` and `log` are directly inside, and
   `package` and `rollback` somewhere inside, the Updates directory - which it
   takes from `DataFolder` in `/etc/hmailserver/hMailServer.ini`, root's own
   file, and never from the request; nothing is a symlink, and every path is
   resolved before the test. A request that fails a check is refused before
   anything is stopped, in the journal (`journalctl -u hmailserver-update`)
   and nowhere else: the Updates directory is the service user's, a process of
   theirs may still be running, and root writes nothing - not even the line
   that would say why - into a directory whose owner is alive to swap a file
   for a symlink between the check and the write. The server notices a request
   that was taken and never answered (its update task, once `wait` plus a
   quarter of an hour has passed with the state still 4 and no restart) and
   reports it as failed, pointing at that journal.
3. It stops the unit. From here no process runs as the service user - the
   unit's control group is emptied, and the service user has no session, no
   cron and no way out of it - which is why the Updates directory is only
   written from here on. What root writes there is made under a random name
   (`mktemp`, so nothing anyone planted under a guessable name is opened) and
   renamed into place, which replaces a symlink at the destination and never
   follows one; the log file and the outcome file are both written that way.
4. It runs `/usr/bin/hmailserver --verify-update <package>` - the same
   Sigstore verification the server did (digest, chain at the log's time, the
   release workflow's identity, the signature, the log's own signature), with
   the trust `/etc/hmailserver/hMailServer.ini` names or the public instance -
   and the same for the rollback image. A file that fails is not installed; the
   unit is started again and the outcome says so. Then the version rule,
   because a signature alone would let a taken-over service have root put a
   signed but superseded release back: the version is read from the package
   itself (`dpkg-deb -f ... Version`, `rpm -qp --qf %{VERSION}`, or the
   verified AppImage's own `--version`) and compared, with `dpkg
   --compare-versions` or `sort -V`, with what the package database says is
   installed (`dpkg-query -W`, `rpm -q`, the running image's `--version`). Not
   newer means not installed, and the unit is started again; a rollback image
   whose version is not the installed one is dropped, so the rollback can only
   ever put back what was running; and the request's `version` line is
   reported only when the package agrees with it. Together these are what make
   the request safe to accept from an unprivileged process: a compromised
   service can write any request it likes, and none of them gets root to
   install a file the release workflow did not sign, or one that is not an
   upgrade.
5. It installs: `apt-get install -y --no-install-recommends
   -o DPkg::Lock::Timeout=300 <package>` where apt-get exists (so a newer
   package's dependencies are met, and an `unattended-upgrades` run is waited
   for rather than tripped over), else `dpkg -i`; `dnf install -y` or
   `rpm -Uvh` on an rpm system. Only the rollback passes `--allow-downgrades`.
   The package's own `postinst` / `%post` runs the schema upgrade as the
   service user (`hmailserver --upgrade-database`) as it does on any upgrade;
   no token is needed, which is why none is issued here.
6. It starts the unit and waits up to `wait` seconds for it to be `active`,
   unrestarted, for ten seconds in a row - a unit under `Restart=on-failure`
   that keeps dying reads active a moment at a time - and for `/readyz` to
   answer 200 when there is one to ask.
7. If the service is not back: `rpm -Uvh --oldpackage` or `apt-get ...
   --allow-downgrades` with the rollback image, start, wait again.
8. It writes one line to `<DataFolder>/Updates/last-apply.txt` - `ok`,
   `failed`, `rolled-back`, `rollback-failed` or `no-rollback`, the version,
   the detail - owned by the service user, and exits. The server's update task
   reads the file at its next run (within fifteen minutes of the start), logs
   it, records it for `GET /api/v1/update` and the Deck's *The last install*,
   and moves it aside as `.reported`. `journalctl -u hmailserver-update` has
   the whole run; `Updates/apply-<version>.log` has the part after the stop.

**An AppImage.** `kind=appimage` when `APPIMAGE` is in the server's environment,
which the AppImage runtime sets before AppRun starts it. The artefact is
`hMailServer-<version>-<arch>.AppImage` from the release page, verified the
same way, and the apply is a swap: the new file copied beside the old one
(same file system, so the rename is atomic), the old kept as `<image>.previous`
- the rollback puts it back - and the unit restarted. The image must be owned
by the user applying the update (root under the units) and writable by nobody
else, in a directory of the same standing, because the running image is what
verifies the new one and is executed to do it. How the request reaches root
depends on how the image runs: under a unit with `RuntimeDirectory=hmailserver`
and the two update units installed from `usr/share/hmailserver/systemd/` inside
the image, the path route above; as root under a unit of its own, the server
copies the script out of the image into `Updates/` and starts it with
`systemd-run --unit=hmailserver-update-<version> --collect`, which is the
outside-the-cgroup start `systemd-run` exists for; from a terminal, or as an
ordinary user under a unit, it is told there is nothing to hand the update to,
and the verified file waits in `Updates/` for `mv` by hand. An AppImage is a
try-it-out artefact and stays one; this is so that trying it out includes the
update.

**What needs root.** Everything from step 2 on, which is why it is a separate
unit and not something the server forks: a child of the server lives in the
server's cgroup and `systemctl stop hmailserver` - the first thing the apply
does - would kill it. `hmailserver-update check` (as root) says whether a
machine is set up for this: the path unit active, `RuntimeDirectory` on the
server's unit, which package tool and probe are there. A server started under
the unit from before this change has no `RUNTIME_DIRECTORY` until it is
restarted under the new one - the upgrade that brings this in does that
restart - and says so if asked to apply before then.

**What is proven, and how.** Three layers, on 18 September 2026.

*The script's logic.* `build/test-hmailserver-update.sh` runs the real script
under dash against fakes on PATH - a `systemctl` that records what it is asked
and answers a planned state, package tools that record and exit as planned, an
`hmailserver --verify-update` that passes when a bundle is beside the file -
through thirty-one cases: the five outcomes for a `.deb`, the dpkg fallback,
the `.rpm` through dnf with its rollback through `rpm --oldpackage`, the
AppImage swap and its rollback, the refusals (a package outside the Updates
directory, a symlink, a foreign unit, an unknown key, an outcome path
elsewhere; each in the journal alone, with nothing written into the
requester's directory), the symlinks a requester might plant where root is
about to write (followed by nothing, the victim file unchanged), the root
re-verification refusing a tampered file and a tampered rollback image, the
version rule (a signed package that is not newer than the installed one, for a
`.deb` and for an AppImage; the request's version line overruled by the
package's; a rollback image that is not the installed version dropped; no
installed version recorded), an image with any write bit for its group or
others (0777, 0620, 0642, 0664) and a directory with one, the log written only
after the stop, and `--dry-run` running nothing: 138 checks, green on Ubuntu
26.04 (dash 0.5.12). It runs in
`.github/workflows/packaging.yml` on every change to these files, beside
`systemd-analyze verify` of the three units; the `.deb` and `.rpm` file
assertions in `linux-build.yml` cover the three new files, and the `.deb` job
checks that `postinst` left the path unit enabled and active.

*The verification, against a real release.* The Linux binary built from this
tree ran `hmailserver --verify-update` against the latest release's
`hmailserver_<version>_amd64.deb` and its `.cosign.bundle` as GitHub serves
them (the release of 15 September 2026), with the built-in public Sigstore
trust: it verifies (signed by the `sign-release.yml` workflow, recorded by the
log at 2026-09-15T01:00:17Z), and the same file with one byte appended is
refused with the two digests named. Doing this found that no real bundle had
ever verified on Linux before - see the finding below - and that is fixed in
the same change.

*The whole apply, for real, under systemd.* In a privileged Ubuntu 26.04
container running systemd 259 as PID 1 on the lab VM (root inside the
container, none on the host): the `.deb` from this tree installed with
`apt-get`, whose `postinst` enabled and started `hmailserver-update.path`;
PostgreSQL beside it, the schema created, the server started under the
packaged unit with `/run/hmailserver` owned by `hmailserver` at 0750; a second
package built from the same tree stamped one patch version higher (call the
tree's version V and that one V+1); both packages signed under a Sigstore of
the regression suite's own (`FakeSigstore`, the fake Fulcio and Rekor the
Windows fixtures verify against), with its trust root and log key named by
`UpdateTrustRootsFile` and `UpdateLogPublicKeyFile` in the root-owned
configuration; a release feed on loopback named by `UpdateFeedUrl`. Then, over
the REST API exactly as the Deck does it: `POST /api/v1/update/check` found
V+1 by its `.deb` name, `POST .../download` fetched and verified it,
`POST .../install` fetched and verified V's package as the rollback image,
wrote the request to `/run/hmailserver` and answered state 4 within a second;
the path unit started the service, which verified both packages again as
root, stopped the unit, ran `apt-get install` (a real upgrade, V to V+1,
`postinst` checking the schema), started the unit, saw it active with
`/readyz` answering for ten seconds, and wrote `ok <V+1> the package was
installed and the service is running` - owned by `hmailserver`; the server ran
V+1, and after its next start `GET /api/v1/update` carried `apply.status: ok`
and the file had become `.reported`. Then the rollback: the base put back to
V, PostgreSQL stopped, the same three calls; the upgraded V+1 came up `active`
but `/readyz` answered 503 for the sixty seconds asked for, the helper said so
("active, but http://127.0.0.1:9099/readyz did not answer 200"), ran `apt-get
install --allow-downgrades` with the rollback image (dpkg: "downgrading
hmailserver (V+1) to (V)"), started the unit, and - PostgreSQL having been
started again meanwhile - wrote `rolled-back <V+1> ...; hMailServer <V> was
reinstalled and the service is running`, with the server on V and ready.

What that leaves unproven: the same run against a *release-signed* package
under the public trust (the packages CI builds carry no bundle;
`sign-release.yml` signs at release time), which the first release after this
change is the occasion for; the `.rpm` route on a real dnf host, exercised
against the fakes only; and the AppImage swap under a real unit, likewise. The
Windows path is untouched by everything above except the bundle read, which
now takes the file's bytes as they are on both platforms.

## What the packaging pass found, and fixed

Each of these would have shipped a broken package, and each is fixed in the
CMakeLists rather than worked around here. Kept as a record of what to check
when the packaging changes.

* **The configuration was not a conffile.** An upgrade replaced
  `/etc/hmailserver/hMailServer.ini`, database credentials and all. The
  CMakeLists now generates a `conffiles` control file for the `.deb` and marks
  the file `%config(noreplace)` in the `.rpm`; the Arch package's `backup=`
  array was already this mechanism.
* **`tlds.txt` and `dh2048.pem` were not installed.** Both are read from the
  directory the executable is in, and without the second
  `SslContextInitializer` reports a critical 5603 for every TLS context. Both
  are installed beside the binary from `hmailserver/installation/Extras`.
* **The RPM had no `%postun`.** `rpm/postun` runs `systemctl daemon-reload`.
* **The configuration landed in `/usr/etc`.** CPack prefixes a relative
  `sysconfdir` with the packaging prefix; the install rules write `/etc`
  absolutely.
* **Every maintainer script would have shipped with CRLF** - `.gitattributes`
  forces it on the whole repository - and failed on the user's machine as
  `bad interpreter: /bin/sh^M`. This directory and `build/*.sh` have an
  `eol=lf` exception.
* **Paths were joined with a written backslash** in ninety-four places, so the
  first start wrote its log to a file called `Logs\hmailserver_....log` in the
  parent directory and delivery could not create a mailbox. Every join goes
  through `FileUtilities::Combine` and `PathSeparator` now, which is `/` here.
* **No real Sigstore bundle ever verified on Linux** (found 18 September 2026,
  by running `--verify-update` against a release's own bundle). The bundle was
  read with `ReadCompleteTextFile` and turned back into bytes with
  `Unicode::ToANSI`, a round trip through the wide `String`; Rekor's checkpoint
  carries an em dash, three UTF-8 bytes that `wcstombs` refuses in the C locale,
  so the whole bundle came back empty and every download failed as "not JSON".
  Windows never saw it because its code page carries those bytes through.
  `SigstoreVerifier::ReadBundle` now reads the file's bytes as they are, on
  both platforms, everywhere a bundle is read.

## What is still owed

* **The AArch64 packages come from CI, not from hands.** The `ubuntu-24.04-arm`
  job builds and installs the `.deb` on every push; nobody has installed one on
  ARM hardware by hand yet.
* **There is no re-keying command.** The stored-secret key file is made once
  and never rotated; replacing it by hand means re-entering every stored
  password, and there is no tool that re-encrypts them under a new key.
* **The service cannot write its copy of the settings.** The configuration is
  `root:hmailserver 0640` and the server runs as `hmailserver`, so every
  `[Settings]` change made over the REST API or with `hmctl` is stored, and
  applies when it is saved (or waits for a reinitialise or a restart, as
  SettingsApply.md lists), but is reported (`HM5806`, then `HM5803` at each
  start) because the line in the file cannot be brought up to date.
  `--set-setting` sidesteps it by running as root. Making the file
  group-writable would end the reports and let the service rewrite a file that
  holds the database password; the choice has not been made.
* **The service does not reopen its log on a signal**, which is why the
  logrotate rule uses `copytruncate`.
* **Directory authentication is a simple bind over LDAPS or StartTLS, and only
  that.** `[LDAP] BindMethod=1` (Negotiate) is Windows SSPI and does not exist
  here; the server reports HM6420 once and refuses the logon as a directory
  failure rather than a wrong password. A GSSAPI bind through Cyrus SASL would be
  its counterpart and is not written. The server's certificate must chain to the
  system trust store (`TLS_CACERT` in `/etc/ldap/ldap.conf`); there is no
  private-CA setting.

Proven since the list above was first written, and therefore not on it: the
PKGBUILD builds and installs on Arch Linux (`makepkg`, `pacman -U`,
`systemd-sysusers`, `/usr/bin/hmailserver --version`); MariaDB 11.8 runs the
same end-to-end sequence PostgreSQL does (schema created by `--create-database`,
an account over REST, a message over SMTP, read back over IMAP).

Compose and Helm
----------------

Beside the image, the two files people expect, in the tree since 15 September
2026:

* `docker/docker-compose.yml` - the server beside a PostgreSQL 16, the two
  passwords as Compose secrets from `./secrets/`, the REST listener (the
  Control Deck and the webmail) bound off loopback and so over TLS with a
  certificate and key from `./certs/`, the three volumes the image declares,
  and the image's own health check. `docker compose up -d`, then
  `https://<host>:8045/` as Administrator.
* `helm/hmailserver/` - a chart: a StatefulSet of one with its three volumes
  as claim templates, a LoadBalancer service with the mail ports (and the REST
  port when the listener is on), a Secret for the two passwords or an existing
  one named by `existingSecret`, a `kubernetes.io/tls` Secret for the REST
  listener named by `rest.tlsSecretName`, probes on port 25 or - when the
  metrics listener is on - on its `/livez` and `/readyz`. The database is not
  in the chart: point `database.host` at one; the chart refuses to render
  without it. `helm install mail ./helm/hmailserver --set database.host=... --set
  database.password=... --set administrator.password=... --set
  rest.tlsSecretName=...`.

Neither Helm nor Docker is on the Windows bench that gates the code, so the
workflow `.github/workflows/packaging.yml` lints and renders the chart with a
real install's values, checks its refusals, and resolves the Compose file, on
every change to either.

### Every setting from the environment

Beyond the sixteen variables the entrypoint has always read, every `[Settings]`
key and every `hm_settings` row this build reads has a name in the environment
(`hmailserver/docs/EnvironmentConfiguration.md`): `HM_SETTING_<key>` for a
`[Settings]` key of `build/ini-settings-baseline.txt`, `HM_PROPERTY_<name>` for an
`hm_settings` row of `SettingsDefaults.h`, and `_FILE` after either for a value
read from a file - a Secret. The entrypoint stores them with `hmailserver
--apply-environment` after the create and upgrade steps, whenever an `HM_`
variable other than its own is set or the data directory holds the
`.hmailserver-environment` file the verb keeps while it manages anything: every
value is checked first, nothing is stored when one is refused (a misspelt prefix
such as `HM_SETTINGS_` included), and the container does not start - `docker
compose logs` or `kubectl logs` says which variable and why, and never prints a
value a file supplied. `HM_REST_PORT` and the other three REST variables are
aliases of `HM_SETTING_RestApiPort` and its three neighbours. The environment wins
at every start, so the Control Deck and the REST API refuse a change to a setting
it names (409), and show it as set by the environment, until a start without the
variable; a value a file supplied stays withheld from every answer until an
administrator writes another. A switch is `1` or `0` for a `[Settings]` key; an
unquoted whole number in the chart's values is rendered as the number it is. The
entrypoint and the chart have been checked by reading and the POSIX verb by
compiling; none of the three has yet been run in a container.

With Helm, the values' `settings:` and `properties:` maps render into the
environment, and `settingsFrom:` mounts a Secret's key as a file and names it
through `_FILE`:

```yaml
settings:
  MaxSubmissionsPerIPPerMinute: "30"
properties:
  maxmessagesize: "20480"
settingsFrom:
  - secretName: mail-relay
    key: password
    property: smtprelayerpassword
```

With Compose, the same variables go under `environment:`, and a `_FILE` names a
path under `/run/secrets/`; the file beside the image carries the examples
commented out. `build/check-environment-names.py` holds every name the
entrypoint, the chart, the Compose file and this README mention to a setting.

### A setting profile at the first start

`HM_PROFILE` names one of the four setting profiles -
`office`, `gateway`, `relay` or `sender` (`hmailserver/docs/Profiles.md`) - and the
entrypoint applies it with `hmailserver --apply-profile NAME --once` after the
environment's settings are stored and before the server starts: every entry
checked, the values it changes saved as the way back, and each written through the
setter the REST settings route runs, one line each in the log. `HM_PROFILE=none`
records that no profile is wanted, so the checklist stops asking. `--once` does
nothing on a database something was recorded for before, so a restart - or a start
after an administrator reverted the profile in the Deck - leaves the settings
alone; an apply that failed part way is tried again at the next start, which is
refused while the way back it left is held, and says so. A setting the environment
manages is skipped and said to be. A profile that closes a listener (relay and
sender close IMAP and POP3), trusts an address range (gateway trusts XCLIENT and
ARC), turns a filter or a check off or weakens it (gateway turns greylisting off),
or shortens a window of days in force asks for a confirmation, which
`HM_PROFILE_CONFIRM=yes` gives; without it nothing is applied and the container does
not start, with the entries named. The entrypoint's `HM_PROFILE` and the POSIX verb
have been written and read; neither has been compiled for Linux or run in a
container.

