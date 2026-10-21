The snap: hMailServer from the Snap Store
=========================================

**The server as a snap, `hmailserver`, for amd64 and arm64: strictly confined, on
core24, made from the release's own Ubuntu 24.04 `.deb`, running as the account
`snap_daemon` and keeping everything it writes under `/var/snap/hmailserver/common`.
Its database is a PostgreSQL or MariaDB/MySQL server of yours, reached over the
network. GitLab CI builds it for both architectures on linix; it was installed,
configured, started, asked over SMTP, POP3, IMAP and REST, and removed under snapd on
2 October 2026 (below). Its name is registered in the Snap Store to the owner's
account, and `SNAPCRAFT_STORE_CREDENTIALS` is set, both by 16 October 2026: after a
release, `snap-upload`, started by hand on `master`'s pipeline, puts the release's
snaps on `edge` and `candidate`, and `stable` is promoted by hand (*The Snap Store*,
below).**

**6.3.7 is not in the store, and nothing is.** The store answers that it has no
snap called `hmailserver` (read on 21 October 2026). 6.3.7's two snaps were built
and checked in `master`'s pipeline 2958945008 on 18 October 2026, and its
`snap-upload` (job 17257943981) stopped before it reached the store or the
credential: the image had no `git` for the job's check of the release tag, which
`build/ci/snap-tools.sh` installs since. The job cannot be run again for 6.3.7. It
uploads only from the pipeline of the release's own commit, and a retry of the old
job runs the old script. The two builds' artifacts are kept, with no expiry (jobs
17257943944, amd64, and 17257943945, arm64), so that 6.3.7 can still be uploaded by
hand (*6.3.7 by hand*, at the end of *The Snap Store*, below); if it is not, the
next release is the first in the store.

| | |
|---|---|
| The packaging | `snap/snapcraft.yaml`, `snap/hooks/configure`, `snap/local/bin/*` |
| The build | `build/ci/snap-build.sh`, with snapcraft from `build/ci/snap-tools.sh`; the jobs `snap-build-amd64` and `snap-build-arm64` (`.gitlab/ci/snap.yml`) |
| The test | `build/ci/snap-install-test.sh`, by hand on linix: in a container, or with `--remote` on a VM with AppArmor |
| The upload | `snap-upload` (`.gitlab/ci/channels.yml`, [Channels.md](Channels.md#snap-store)), started by hand on `master`'s pipeline after a release: `edge` and `candidate`; `stable` is promoted by hand |

Installing and starting it
--------------------------

Once it is in the store:

    sudo snap install hmailserver

Until then, a snap a pipeline built (`snap-build-<arch>`'s artifacts) installs with
`sudo snap install --dangerous hmailserver_<version>_<arch>.snap`; `--dangerous`
because a file from outside the store carries no store signature.

The service is installed but not started: it has no database yet. Name one, give the
database password and the administrator's, and start it:

    sudo snap set hmailserver db.type=postgresql db.host=192.0.2.10 \
        db.name=hmailserver db.user=hmailserver
    printf '%s\n' 'the database password' | sudo hmailserver.set-database-password
    printf '%s\n' 'the administrator password' | sudo hmailserver --set-admin-password
    sudo snap start --enable hmailserver.daemon

The database must exist and belong to that role, as for the `.deb`
([packaging/README.md](../source/Server/platform/packaging/README.md), step 2); the
first start creates the schema in it. The first start then does what the `.deb`'s unit
does: a new installation's defaults ([SecureDefaults.md](SecureDefaults.md)) -
a self-signed certificate, STARTTLS on 25, 587, 110 and 143, and a password refused
without TLS from any address but this machine's. `sudo snap logs -n=50 hmailserver`
shows what it did.

A start with no database named says so in the log and stops, and is not started again
until you start it.

The configuration
-----------------

`snap set hmailserver <key>=<value>` sets the same things the container image's
environment does ([ContainerImage.md](ContainerImage.md)); the service applies them at
its next start, and a `snap set` while it runs restarts it at once. A key the snap
does not read, or a value it cannot pass on, makes `snap set` fail with the reason, and
nothing is changed.

| Key | What it sets |
|---|---|
| `db.type` | `postgresql` (or `postgres`, `pgsql`) or `mysql` (or `mariadb`) |
| `db.host`, `db.port`, `db.name`, `db.user` | the database server and the database; the port is 5432 or 3306 unless set |
| `db.tls-mode` | `disable`, `prefer`, `require` or `verify-full` ([DatabaseTls.md](DatabaseTls.md)) |
| `db.tls-ca` | a PEM file of CA certificates, in `/var/snap/hmailserver/common/data` and readable by `snap_daemon` (below) |
| `db.wait-seconds` | how long the start waits for the database server to take a connection (0 to 999999) |
| `rest.port`, `rest.bind-address` | the REST API and the Control Deck: off (0) and `127.0.0.1` unless set |
| `rest.certificate-file`, `rest.private-key-file` | its certificate and key, in `/var/snap/hmailserver/common/data` and readable by `snap_daemon`; the server requires TLS on any address but the loopback |
| `profile`, `profile-confirm` | a setting profile applied at the first start ([Profiles.md](Profiles.md)): `office`, `gateway`, `relay`, `sender` or `none`, and `yes` for one that closes a listener or weakens a check |
| `create-database`, `upgrade-database` | `false` to leave either step out of the start (both on unless set) |
| `setting.<key>` | a `[Settings]` key, in lower case: `snap set hmailserver setting.maxsubmissionsperipperminute=30` |
| `property.<name>` | an `hm_settings` row, in lower case: `snap set hmailserver property.maxmessagesize=20480` |

`setting.*` and `property.*` go to the server's `--apply-environment` as
`HM_SETTING_<key>` and `HM_PROPERTY_<name>` ([EnvironmentConfiguration.md](EnvironmentConfiguration.md)):
the server checks each name and value, stores them in the database and refuses a
change to them from the Control Deck or the REST API while they are set here;
`snap unset hmailserver setting.<key>` hands the setting back at the next start.

Only the top-level keys above are read. snapd cannot list the keys a snap was given,
so a misspelt top-level key (`snap set hmailserver dbs.host=...`) is kept by snapd and
does nothing; a misspelt key under one the snap reads (`db.hots`) is refused.

**No secret is a `snap set` value.** snapd gives a snap's configuration to every
command of the snap, whoever runs it, and `snap get` to every user of the machine, so
a password set there could be read back by any of them. A key whose name says
password, secret or passphrase is refused, and so is a `setting.<key>` or
`property.<name>` the server itself counts as a secret by its name - the rule by which
it withholds a value from every listing and export (`IniSettingStore::IsSecretName`):
a name holding `password`, `passwd`, `secret`, `token`, `apikey`, `credential`,
`community`, `privatekey`, `keyfile`, `hash`, `salt` and the rest, but not a path to a
file, an address, the password policy or a hash's work factor. Each refusal says
where the value goes instead:

- the database password: `hmailserver.set-database-password`, one line on standard
  input, into `/var/snap/hmailserver/common/secrets/database-password` (root's, mode
  0600; `--remove` takes it away). The service reads it as root at each start and
  writes it into the configuration's `[Database]` section, which only `snap_daemon`
  reads;
- the administrator's: `sudo hmailserver --set-admin-password`, which keeps a PBKDF2
  hash and never the password;
- any other secret setting: `sudo hmailserver --set-setting <key>` (CommandLine.md).

`[Database]` is written from `db.*` only when `db.type` is set. Without it, the
section is whatever the configuration file holds, which can be edited by hand
(`sudo nano /var/snap/hmailserver/common/etc/hMailServer.ini`), and the password file
is not used.

The commands
------------

| Command | What it is |
|---|---|
| `hmailserver.daemon` | the service, `snap.hmailserver.daemon.service`: `sudo snap start --enable`, `stop`, `restart`, `logs hmailserver` |
| `hmailserver` | the server's own verbs against the snap's configuration ([CommandLine.md](CommandLine.md)): `sudo hmailserver --check-config`, `--set-admin-password`, `--set-setting`, `--create-stored-secrets-key`, `--sample-domain create` and the rest |
| `hmailserver.hmctl` | the REST API's client ([CommandLine.md](CommandLine.md)) |
| `hmailserver.set-database-password` | above |

`sudo hmailserver ...` runs the verb as `snap_daemon`, the service's account, as every
command in the container image runs as its `hmailserver` account: whatever the verb
writes - the configuration, the stored-secrets key, files in the data directory, the
log - is the service's afterwards, never root's. Run without `sudo` it runs as you, and
only `--version` and `--help` work, since the configuration is `snap_daemon`'s alone. A
file a verb reads or writes by a path you give (`--export-stored-secrets-key FILE`,
`--import-stored-secrets-key FILE`) must be one `snap_daemon` can write or read in
the one place the snap may reach, `/var/snap/hmailserver/common` - in practice its
`data/` directory, since `common` itself is root's: `sudo hmailserver
--export-stored-secrets-key /var/snap/hmailserver/common/data/key.export`, then
`sudo mv` it out. A file of yours the server is to read - a key to import, `db.tls-ca`,
the REST API's certificate and key, a listener's TLS certificate - goes there with the
account as its owner: `sudo install -o snap_daemon -g snap_daemon -m 0600 key.pem
/var/snap/hmailserver/common/data/`.

Where it keeps things
---------------------

Everything is under `/var/snap/hmailserver/common` (`$SNAP_COMMON`), which every
revision of the snap shares and a refresh keeps:

| | |
|---|---|
| `etc/hMailServer.ini` | the configuration, `snap_daemon`'s, 0600, in a 0700 directory; made at the first start from the `.deb`'s, with the directories below |
| `data/` | the messages, `temp/`, `events/` and `database/`, and the stored-secrets key; `snap_daemon`'s, 0750 |
| `log/` | the server's logs; `snap_daemon`'s, 0750 |
| `secrets/database-password` | root's, 0600, in a 0700 directory |

`ProgramFolder` - the DBScripts, the Control Deck and the licences - is the running
revision's (`/snap/hmailserver/<revision>/usr/share/hmailserver`) and is set again at
each start, since a refresh mounts the new revision at another path. The other
directory lines are yours once the file exists, but strict confinement lets the snap
write nowhere outside `$SNAP_COMMON`.

**Back up** `etc/` and `data/` with the database; the stored-secrets key in
`data/.hmailserver-secret-key` opens what the database keeps sealed
([SecretsAtRest.md](SecretsAtRest.md)).

**Removing it.** `sudo snap remove hmailserver` stops the service and removes the snap
and `/var/snap/hmailserver` - after snapd has saved a snapshot of that directory,
messages included, under `/var/lib/snapd/snapshots`, kept for 31 days unless
`snap set system snapshots.automatic.retention` says otherwise. `sudo snap remove
--purge hmailserver` removes it without a snapshot. The database is yours and is not
touched either way.

Why strict, and as snap_daemon
------------------------------

A snap is confined strictly, or classically - which confines nothing and runs it as
an ordinary program of the machine - or in devmode, which the store does not publish to
its stable channel. A mail server needs the network and its listening ports, which the
`network` and `network-bind` interfaces give a strict snap, and nothing of the
machine's own files: its configuration, mail and logs can all live in `$SNAP_COMMON`.
The store grants classic confinement only to a few kinds of tool (compilers, editors,
IDEs) that must reach the whole machine, which a mail server is not. So strict.

Under strict confinement, snapd starts a snap's service as root. The service gives
that up at once: `hmailserver-daemon`, as root, makes the directories directly under
`$SNAP_COMMON`, reads the snap's configuration and the password file, then runs the
rest through `setpriv` as
`snap_daemon` - the account snapd makes for snaps that ask for it with
`system-usernames` (uid and gid 584788) and lets only them change to - with
`CAP_NET_BIND_SERVICE` as its one capability, for ports 25, 110, 143, 465, 587, 993 and
995, and `no_new_privs`. That is how the `.deb`'s unit runs it (`User=hmailserver`,
`AmbientCapabilities=CAP_NET_BIND_SERVICE`), and it is what the server expects: run as
root, `--apply-new-install-defaults` refuses, and a stored-secrets key or a log file
root made is one the service could not open. The configuration is `snap_daemon`'s own,
as it is the `hmailserver` account's in the container image, because the image's
entrypoint, which the snap runs unchanged, writes `[Database]` and the settings as
that account at each start.

Root does nothing inside the account's directories, and cannot: snapd's AppArmor
profile gives a snap's root `CAP_CHOWN`, `CAP_SETUID` and `CAP_SETGID` for the drop to
the account, and on purpose neither `CAP_DAC_OVERRIDE`, `CAP_DAC_READ_SEARCH` nor
`CAP_FOWNER`, so that ordinary file permissions bind root too. So root makes `etc/`,
`data/` and `log/` and gives them to the account, and the account itself sets their
modes, makes `data/temp`, `data/events` and `data/database`, makes the configuration
and decides whether a database is named in it. Likewise a program the snap runs
from core24 must be on the profile's list of the base's programs; the one the
entrypoint needs that is not, `timeout`, is in the snap itself.

`snap_daemon` is one account for every snap that asks for it, not this snap's own.
Where snapd confines strictly (`snap debug confinement` says `strict`, as on Ubuntu),
another snap running as `snap_daemon` is kept out of `/var/snap/hmailserver` by its
own AppArmor profile. Where it does not (`partial`: a distribution whose snapd
confines without AppArmor), nothing but the account's permissions stands between
this snap's configuration, mail and keys and another snap running as `snap_daemon`:
install this snap there only where no other snap uses that account.

What the snap does not do, and why:

- **The live update.** snapd updates a snap. The server knows it runs in one (its
  update check reports `installKind` `snap`, names `snap refresh hmailserver` and
  downloads and installs nothing - [LiveUpdateKinds.md](LiveUpdateKinds.md)), and the
  helper and its units are not in the snap.
- **Auto-ban in the firewall** (`AutoBanFirewall`): the snap may not change the
  machine's firewall, and the hook is not in it. The
  server's own auto-ban works as everywhere; fail2ban on the machine can read
  `/var/snap/hmailserver/common/log`.
- **A database over a Unix socket.** The snap cannot reach the machine's
  `/var/run/postgresql`; name the database server by address, the loopback included.
- **Files outside `$SNAP_COMMON`**, such as a certificate under `/etc/letsencrypt`:
  copy it into `/var/snap/hmailserver/common/data`, owned by `snap_daemon` (a deploy
  hook of the ACME client can: `install -o snap_daemon -g snap_daemon -m 0600`).
- **logrotate**: the server starts a new pair of logs each day, and with
  `LogDeleteDays` set (`sudo hmailserver --set-setting LogDeleteDays=30`) deletes the
  older ones itself; the snap carries no logrotate rule, so nothing else does.

How it is built
---------------

`snap-build-amd64` and `snap-build-arm64` (`.gitlab/ci/snap.yml`) run
`build/ci/snap-build.sh` on linix, each in Ubuntu 24.04 of its own architecture (arm64
under qemu-user through linix's binfmt handler), after the `deb-ubuntu24.04-<arch>`
job whose `.deb` it takes - in a `batch*` pipeline at once, on `master` once that
`.deb`'s build has been played, never for a tag or a merge request:

1. `build/ci/snap-tools.sh`: snapcraft is itself a snap, and the job container has no
   snapd. So snapcraft and core24 are downloaded from the Snap Store at a pinned
   revision, each held to the SHA3-384 of the store's signed assertion for that
   revision, and unpacked where snapd would mount them; `/usr/local/bin/snapcraft`
   runs snapcraft with the environment snapd would give it. snapd's `snap pack`,
   squashfs-tools, Python, curl, git and openssh-client (`snap-upload` holds the
   pipeline's commit to the release tag's with `git fetch` and `git verify-tag`) come
   from Ubuntu's archive. Nothing comes from GitHub, and nothing needs privilege.
2. The `.deb` goes where `snapcraft.yaml`'s part reads it, and `snapcraft pack
   --destructive-mode` builds the snap in the job's own container - no LXD, no remote
   build - since core24 is Ubuntu 24.04, as the container is. The part unpacks the
   `.deb` unchanged and takes from Ubuntu 24.04's archive what core24 lacks: libpq5,
   libldap2, the MariaDB client the server opens when a MySQL database is configured,
   and 7-Zip - and `timeout` from Ubuntu 24.04's coreutils, which core24 has but the
   snap's AppArmor profile does not let it run from there (above).
3. The snap is held to what it must be: its `meta/snap.yaml` names `hmailserver`, the
   `.deb`'s version without its Debian revision, the architecture, core24, strict
   confinement, the stable grade and `snap_daemon`; it carries the server, the
   commands, the hook, the Control Deck and the licences, and no systemd unit and no
   live-update helper; the server it carries finds every library in the snap or core24
   and answers `--version` with that version; and the configuration helper passes its
   own self-test.
4. `snap/hmailserver_<version>_<arch>.snap` for `snap-upload`, and in
   `snap-build/<arch>/` its `meta/snap.yaml`, file list, SHA-256, SHA3-384 and
   snapcraft's log.

**Moving snapcraft or core24 to a newer revision.** For each architecture, ask the
store which revision a channel holds and its SHA3-384:

    curl -s -X POST https://api.snapcraft.io/v2/snaps/refresh \
      -H 'Snap-Device-Series: 16' -H 'Snap-Device-Architecture: amd64' \
      -H 'Content-Type: application/json' \
      -d '{"context":[],"actions":[{"action":"download","instance-key":"k","name":"snapcraft","channel":"9.x/stable"}],"fields":["revision","version","download"]}'

then check it against the store's signed assertion, on a machine with snapd:
`snap download snapcraft --revision=<revision>` (with `UBUNTU_STORE_ARCH=arm64` for
the other architecture) and `sudo snap ack snapcraft_<revision>.assert`, which refuses
an assertion the store did not sign; its `snap-sha3-384` line is the same hash in
base64url. Put the revision and the hash in `snap-tools.sh`'s `PINS` and run both
builds.

What has run
------------

On 2 October 2026, on linix, from batch42's `deb-ubuntu24.04-amd64` and `-arm64`
packages of pipeline 2897966924 (6.3.6, SHA-256 `759174dd...` and `ac9d0199...`),
with snapcraft 9.1.3 (revisions 19192 and 19189) and core24 20260824 (2124 and 2125):

- **Built:** both snaps, by `snap-build.sh` in the jobs' images - amd64 packed in about
  20 seconds, arm64 under qemu in about two minutes; every check of step 3 passed for
  both, and snapcraft's linters reported nothing.
- **Installed and run (amd64):** `build/ci/snap-install-test.sh` in a privileged
  Ubuntu 24.04 container with systemd as PID 1 and snapd (2.77.1, from the store, as
  snapd installs itself for a core24 snap): 62 checks, none failed - install
  disabled, the start without a database, a `[Database]` written into the
  configuration by hand taken for one, five refused `snap set`s, the database by
  `snap set` and the password file, the administrator's hash, `sudo hmailserver
  --check-config` and `hmailserver.hmctl`, SMTP on 25 and 587 with
  STARTTLS, POP3, IMAP with STARTTLS, the new-installation marker gone, the process as
  `snap_daemon` with `CAP_NET_BIND_SERVICE` alone, `no_new_privs` and snapd's seccomp
  filter, no password in its environment, nothing of root's in its directories, the
  configure hook restarting it for `rest.port`, REST 200 and 401, the update check's
  `snap` kind, a restart, and `snap remove --purge` leaving no process, listener,
  directory or unit. linix's 32 binfmt handlers and 184 AppArmor profiles were the same
  before and after.
- **Under AppArmor (amd64):** the same test with `--remote`, in a throwaway Ubuntu
  24.04 VM - the cloud image of the day, under QEMU without KVM in a container on
  linix, so that no profile reached linix's kernel - with snapd 2.77.1 confining
  strictly: 63 checks, none failed - those above, and AppArmor enabled, the daemon's
  profile loaded in enforce mode, the running server under it, and no AppArmor denial
  of any of the snap's profiles in the kernel's log from the test's start to its end.
  There `snap_daemon` with `CAP_NET_BIND_SERVICE` as its ambient capability binds 25,
  110, 143 and 587: `network-bind` and that capability are enough.
- **Negative controls under AppArmor:** the snap as first written, root making the
  account's subdirectories, failed its first start, in a loop: `install` denied
  `CAP_FOWNER`, `CAP_DAC_READ_SEARCH` and `CAP_DAC_OVERRIDE`. Run under the daemon's profile, core24's `/usr/bin/timeout` is
  denied (`exec`), as root and as `snap_daemon`, so `db.wait-seconds` failed until the
  snap carried its own. Without `PGGSSENCMODE=disable`, each connection to PostgreSQL
  left a denial of `/etc/gss/mech.d` in the kernel's log; run from a home directory
  of mode 0750, each `sudo` command of the snap left one of `CAP_DAC_READ_SEARCH` (so
  the test runs its commands from `/`).
- **Negative controls**, each a copy of the snap with one thing changed, each failing
  the test at its own checks: the service run without the change to `snap_daemon` (12
  failures: uid, gid and groups 0, every capability, no `no_new_privs`, and no
  STARTTLS, since the server refuses to apply a new installation's defaults as root
  and leaves its marker root's); no `system-usernames` (snapd makes no `snap_daemon`,
  so the directories cannot be made its, and nothing starts); no new-installation
  defaults (4: STARTTLS on 25, 587 and 143, and the marker); a configure hook that
  checks nothing (6: each refused `snap set` taken). The configuration helper's own
  self-test (49 cases) fails 6 with its secret check taken out, 2 with the
  `$SNAP_COMMON` check taken out, 5 with the server's secret rule replaced by the
  word list (a community, a token and an API key taken, the password policy and a
  hash's work factor refused), 3 with a fragment missing from its copy of the
  server's list, which it compares with `AuditTrail.cpp`, and 1 with `true` taken
  for a port.

**Not yet shown:**

- **arm64 installed**: the arm64 snap was built and checked, not installed.
- **A MySQL or MariaDB database under AppArmor**: the tests use PostgreSQL; what the
  MariaDB client reads at a connection has not been put to the profile.
- **The other apps' work under AppArmor** beyond the test's: `hmctl` against the REST
  API, and verbs such as `--export-stored-secrets-key`.
- **The store's review** of a snap that asks for `system-usernames`.

The Snap Store
--------------

`snap-upload` ([Channels.md](Channels.md#snap-store)) uploads the release's snaps to
`edge` and `candidate`; `stable` is promoted by hand. It does nothing until the owner
has done this, once:

1. **An Ubuntu One account** with two-factor authentication, at
   `https://login.ubuntu.com`, then signed in once at `https://snapcraft.io` to accept
   the developer terms.
2. **The name.** At `https://snapcraft.io/register-snap`, register `hmailserver`. If
   the store holds it for review, the comment to give:

   > This snap is Progressive Robot's fork of hMailServer, an open-source mail server
   > (SMTP, IMAP, POP3), AGPL-3.0-or-later, maintained by Progressive Robot Ltd at
   > https://gitlab.com/hmailserver/hmailserver. It is not the original hMailServer by
   > Martin Knafve, which is developed separately and has releases of its own; the
   > snap's title, summary and description say so. The snap is built by the fork's own
   > CI from its release packages and published by the fork's maintainers.

3. **The credentials.** snapcraft runs on Linux; in a terminal on linix, which the
   maintainers open for you, in a throwaway container so that nothing is installed on
   linix itself:

       docker run --rm -it -v "$PWD:/w" -w /w ubuntu:24.04 bash -c \
         'bash build/ci/snap-tools.sh > /dev/null && snapcraft export-login --snaps=hmailserver \
            --channels=edge,candidate \
            --acls package_access,package_push,package_update,package_release \
            --expires YYYY-MM-DD -'

   from a checkout of the repository. It asks for the Ubuntu One e-mail address,
   password and second factor, and prints the credentials. In GitLab, Settings, CI/CD,
   Variables: add `SNAPCRAFT_STORE_CREDENTIALS` with that text, protected, masked and
   hidden, for the environment `snap-store`. Write a date at most a year away for
   `YYYY-MM-DD`: the credentials expire then, and once they have, `snap-upload`
   fails with the store's refusal, and the same command makes new ones.

   **`--channels=edge,candidate` is what keeps `stable` a person's step.** A
   credential without it may release any revision to any channel, `stable` among them,
   and every device following `stable` takes that revision at its next refresh; the CI
   variable can be read by any job of a protected pipeline that declares the
   `snap-store` environment, since GitLab Free has no protected environments. So
   `snap-upload` asks the credential what it may do (`snapcraft whoami`) before it
   uploads anything, and refuses one with no channel restriction, one whose channels
   name `stable`, and one of which `whoami` says nothing; and its own release list may
   not name `stable` either. The first `snap-upload` after making it is its trial: the
   store's channel caveat is what holds it, and if the store refuses the release to
   `edge` or `candidate`, make it again with `--channels=latest/edge,latest/candidate`,
   the track written out. `package_update` (the
   store listing) is in the list because the store's own recipe for CI has it; leave
   it out if an upload works without it. (Since the second security review of 6.3.6;
   until then this step made a credential for every channel.)

After that, for each release: on `master`'s pipeline for the release's commit, play
`deb-ubuntu24.04-amd64` and `deb-ubuntu24.04-arm64` (the snap builds follow them),
then `snap-upload`. Install the candidate on a machine of your own (`sudo snap install
hmailserver --candidate`), and when it is right, promote each architecture's revision
with your own login, never the CI credential, which cannot: `snapcraft login`, then
`snapcraft release hmailserver <revision> stable`, with the revisions `snap-upload`
printed, and `snapcraft logout`.

**6.3.7 by hand.** `snap-upload` cannot take 6.3.7 (above), so its snaps go up from
a Linux shell with snapcraft and the owner's own login, in about half an hour:

1. Download the two snaps, `hmailserver_6.3.7_amd64.snap` and
   `hmailserver_6.3.7_arm64.snap`, from the artifacts of jobs 17257943944 and
   17257943945 of pipeline 2958945008, which are kept.
2. `snapcraft login`.
3. `snapcraft upload --release=edge,candidate hmailserver_6.3.7_<arch>.snap`, for
   each of the two files.
4. Install the candidate on a machine of your own (`sudo snap install hmailserver
   --candidate`) and try it.
5. Before the release to `stable`, set the store listing at
   <https://snapcraft.io/hmailserver/listing>, with the same login: the title
   "hMailServer (Progressive Robot's fork)", a summary and a description that open
   with "Progressive Robot's fork of hMailServer, not the original by Martin
   Knafve", and the website <https://gitlab.com/hmailserver/hmailserver>. The two
   snaps were built on 18 October 2026 from `snap/snapcraft.yaml` as it stood then -
   the title "hMailServer", a summary that does not say fork, and the company's site
   as the website - so a listing that took their text would read as the original's;
   `snapcraft.yaml` says the fork from the next release.
6. `snapcraft release hmailserver <revision> stable`, with each revision the uploads
   printed, when it is right.
7. `snapcraft logout`.
