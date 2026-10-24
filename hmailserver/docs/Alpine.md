Alpine Linux: the server on musl, an OpenRC service and an APKBUILD
===================================================================

**The server builds and runs on Alpine Linux, with musl as its C library, from the
release's source tarball and Alpine's own packages. The tree carries an APKBUILD,
which builds it as aports would, an OpenRC service, and the two install scripts an
Alpine package needs; GitLab CI builds the package on Alpine 3.24, runs the tester
programs in its `check()`, installs it in a clean container, starts it with
`rc-service` and asks SMTP and IMAP for their greetings, and runs the whole Linux
suite against it. No Alpine release carries it yet: that is a merge request to
aports, which needs a named maintainer (Roadmap3, "Alpine Linux").**

| | |
|---|---|
| The APKBUILD | `hmailserver/source/Server/platform/packaging/alpine/APKBUILD`, with `hmailserver.pre-install` and `hmailserver.post-upgrade` beside it |
| The OpenRC service | `hmailserver/source/Server/platform/packaging/openrc/hmailserver.initd` and `hmailserver.confd`, installed as `/etc/init.d/hmailserver` and `/etc/conf.d/hmailserver` |
| The source | `hmailserver-<v>.tar.gz`, the release's source tarball ([SourceTarball.md](SourceTarball.md)); the APKBUILD installs the OpenRC files from inside it |
| Packages | `hmailserver`, `hmailserver-openrc` (installed with it wherever OpenRC is) and `hmailserver-cli` (`hmctl`, which needs Python) |
| CI | `alpine-package`, `alpine-install`, `alpine-suite` (`.gitlab/ci/alpine.yml`, `build/ci/alpine.sh`) on linix, for master and `batch*` |

**Where it stands (29 September 2026):** see [What has run](#what-has-run).

Installing it
-------------

Until aports carries it, build the package from a release's tarball with the
APKBUILD (below), then install the files it made with `apk add`. Once it is in
aports' `testing` repository:

    apk add hmailserver

Either way the rest is the same. The package makes the `hmailserver` account and
group, `/var/lib/hmailserver` and `/var/log/hmailserver` (0750, the account's), and
installs the configuration as `/etc/hmailserver/hMailServer.ini`, 0640
`root:hmailserver`, with its `[Database]` section empty, as the `.deb` and the `.rpm`
do. Then, as root:

1. **A database.** For PostgreSQL on the same machine:

        apk add postgresql18
        rc-update add postgresql
        rc-service postgresql start    # the first start makes the cluster
        psql -U postgres -c "create role hmailserver login createdb password '<password>'"

   The server connects to MySQL and MariaDB too, through the MariaDB connector it
   opens when such a database is configured: `apk add mariadb-connector-c`. It is
   not a dependency of the package, as it is not of the `.deb`.

2. **The configuration's `[Database]` section**, in
   `/etc/hmailserver/hMailServer.ini`: `Type=PostgreSQL`, `Server=127.0.0.1`,
   `Port=5432`, `Database=hmailserver`, `Username=hmailserver`,
   `Password=<password>`.

3. **The database, made by the server as its account**, the administrator
   password, which the REST API needs, and the stored-secrets key, exported at once
   (the packaging guide's step 4c, `platform/packaging/README.md`, says why):

        su -s /bin/sh -c 'hmailserver --check-config' hmailserver
        su -s /bin/sh -c 'hmailserver --create-database' hmailserver
        hmailserver --set-admin-password
        hmailserver --create-stored-secrets-key
        hmailserver --export-stored-secrets-key /root/hmailserver-stored-secrets.key

4. **The service:**

        rc-update add hmailserver
        rc-service hmailserver start

   The first start secures the new database, as the systemd unit's does, when it
   comes within a day of `--create-database`, which says so
   ([SecureDefaults.md](SecureDefaults.md)): a self-signed certificate, STARTTLS on
   25, 587, 110 and 143, and a password refused without TLS from any address but the
   machine's own.

An upgrade (`apk upgrade hmailserver`, which the server's update check names, since
the package is built without the live update) brings the database schema forward
as the service's account when the configuration names a database, as the `.deb`
does, and says to restart the service: `rc-service hmailserver restart`.

The OpenRC service
------------------

`/etc/init.d/hmailserver` says what the systemd unit
(`platform/packaging/hmailserver.service`) declares, as far as OpenRC can say it:

- the server runs as `hmailserver:hmailserver`, with `CAP_NET_BIND_SERVICE` in its
  ambient set - all it needs for 25 and the other ports below 1024 - no new
  privileges (`no_new_privs`), umask 0027 and `/var/lib/hmailserver` as its
  directory;
- it does not fork, so `start-stop-daemon` puts it in the background and keeps its
  pid in `/run/hmailserver.pid`; what it writes to its standard output and error goes
  to `/var/log/hmailserver/console.log`, its own logs beside it;
- before each start, `checkpath` makes the data and log directories if they are
  gone, and, as the service's account, `--apply-new-install-defaults --if-marked`,
  as the unit's `ExecStartPre` - a failure there does not stop the start. So
  `--create-database`, where `openrc-run` and this service are installed and the
  configuration is `/etc/hmailserver/hMailServer.ini`, says that the service's next
  start secures the new database, as it says of the unit's first start under
  systemd;
- `rc-service hmailserver reload` sends SIGHUP, which re-reads the configuration
  (the listeners stop and start again); `checkconfig` runs `--check-config`;
- a stop sends SIGTERM and waits up to five minutes for the drain, then SIGKILL, as
  the unit's `TimeoutStopSec=300`;
- it needs `net`, uses `dns` and `logger`, and starts after `firewall`, `postgresql`
  and `mariadb` when they are there.

What it cannot say: the unit's filesystem sandbox (`ProtectSystem=strict`,
`PrivateTmp`, `ProtectHome` and the rest), its system call filter and its emptied
capability bounding set have no OpenRC equivalent. `/etc/conf.d/hmailserver` names
another configuration file (`cfgfile`), other data and log directories, or other
capabilities (`capabilities`).

The APKBUILD
------------

It follows Alpine's APKBUILD reference and aports' conventions:

- **The build** is CMake with Ninja (`samurai`) and `-DCMAKE_BUILD_TYPE=MinSizeRel`,
  as `newapkbuild`'s template has it, with the live update off naming
  `apk upgrade hmailserver` and Alpine's shared Boost (`-DHM_BOOST_STATIC=OFF`), as
  the Debian source package and the RPM spec build ([SourcePackages.md](SourcePackages.md)).
  `makedepends` is `boost-dev cmake gnu-libiconv-dev libpq-dev openldap-dev
  openssl-dev samurai zlib-dev`; iconv is GNU libiconv's, not musl's
  ([What musl changed](#what-musl-changed)). The run-time dependencies are the
  shared libraries abuild finds and `ca-certificates-bundle`, the authorities the
  server's HTTPS requests are checked against, as the `.deb`, the `.rpm` and the
  PKGBUILD depend on `ca-certificates`.
- **`check()`** runs the server's `--version` - every static constructor runs before
  `main`, so a server that cannot start fails the build - and every `*-tests`
  program the build makes.
- **`package()`** installs with CMake, removes the systemd unit, installs the OpenRC
  script and its `conf.d` from the tarball into `-openrc`, moves `hmctl` into `-cli`,
  and gives the configuration and the directories their owners and modes.
  `pkgusers` and `pkggroups` name the account, which `hmailserver.pre-install` makes
  on the target (`addgroup -S`, `adduser -S`) before the files it owns are unpacked.
- **`sha512sums` is not in the tree.** The tree is committed before its tarball is
  made, so there is no sum to write; abuild refuses to build until
  `abuild checksum` has written it, from the release's tarball once that is verified
  against the release's Sigstore bundle. The CI job writes it from the
  `source-tarball` job's file.
- **`pkgver`** is checked against `Version.h` by `build/check-linux-version-stamp.sh`,
  as the PKGBUILD's is, and stamping a release edits it (RELEASE.md, "Version
  stamp").
- **`apkbuild-lint`** (atools) has nothing to say about it; the `alpine-package` job
  fails if it does.

To build it by hand in `alpine:3.24`, as the job does:

    apk add abuild atools
    abuild-keygen -a -i -n
    mkdir -p ~/aports/testing/hmailserver /var/cache/distfiles
    cp APKBUILD hmailserver.pre-install hmailserver.post-upgrade ~/aports/testing/hmailserver/
    cp hmailserver-<v>.tar.gz /var/cache/distfiles/
    cd ~/aports/testing/hmailserver
    abuild checksum
    abuild -r

(`abuild -F` as root in a container.)

### The APKBUILD in aports

The merge request is the maintainer's: an account on `gitlab.alpinelinux.org`, a
fork of `aports`, `testing/hmailserver/` with the three files, `abuild checksum`
against the verified release tarball, `apkbuild-lint`, `abuild -r`, and one squashed
commit, `testing/hmailserver: new aport`. The Maintainer line then names that person.
aports' mailing list is not a route today: Alpine's wiki says "Submitting patches
via the mailing list is currently broken" (read 27 September 2026).

What musl changed
-----------------

The server had never been built on musl. Built on Alpine 3.24 (musl 1.2.6, GCC 15.2,
Boost 1.84, OpenSSL 3.5, libpq 18, OpenLDAP 2.6) on 29 September 2026, every one of
its 796 objects but one compiled, every tester program passed, and the Linux suite
and a probe of musl's iconv found the rest. Four things, each fixed where it was:

1. **`strerror_r`** (`Common/Util/File.cpp`, the one object that did not compile).
   glibc's is the GNU one, which returns the message; musl's is the POSIX one, which
   returns a number and writes the message into the buffer, and the code took the
   GNU one's answer as a `const char *`. Two overloads of a small helper now take
   whichever the C library declares, with no test of the library's name: musl
   defines no macro that says it is musl.
2. **The name of US-ASCII** (`Common/Util/Unicode.cpp`). The server gave iconv
   glibc's canonical name for it, `ANSI_X3.4-1968`, which musl's iconv does not
   know, so every `us-ascii` part failed to convert: a script's text added to an
   empty body, which a mail client labels `us-ascii`, was lost
   (`EventsJavaScript.TextAddedToAnEmptyBodyIsWritten`, the one test of that run
   that failed for musl). It is now `US-ASCII`, which glibc, musl and GNU libiconv
   all know. GNU libiconv, which the package now builds with (3), knows the old
   name too; the new one is the name every iconv knows.
3. **UTF-7, in musl's iconv.** There is none: musl's iconv has no UTF-7, no
   ISO-2022-JP-2, and writes no BIG5 (measured with `iconv_open` against Alpine
   3.24's musl). IMAP's folder names are written in modified UTF-7, which the server
   converts through iconv's UTF-7, so a server on musl's iconv would give every
   folder name outside ASCII as nothing. `CMakeLists.txt` now asks the iconv it
   found for UTF-7 and US-ASCII both ways, and refuses one without them, naming GNU
   libiconv; the APKBUILD builds against Alpine's `gnu-libiconv` (1.18, the library
   LGPL-2.1), which has every character set the server names. A glibc has UTF-7
   everywhere the project builds, so nothing changes there; Fedora's, EL's and
   openSUSE's glibc keeps BIG5, ISO-2022-JP and most code pages in a package glibc
   only recommends, which their images do not install - a gap of those packages,
   not of musl, which the `.rpm` and the RPM spec close from 6.3.6 by requiring it
   (PackagesRpm.md, "The character sets").
4. **MSVC's `%Iu`** (`platform/hm_platform.h`). The tree writes a `size_t` as
   MSVC's `%Iu` 13 times in 10 of the server's formats (and once in the fuzz
   harness), every argument a `size_t`, and the POSIX build's format translator handed it to
   `vswprintf` as it was. glibc printed it all the same - it reads the `I` as a flag
   of its own, the locale's alternative digits, and the argument as an
   `unsigned int`, which cuts a size above 4 GB to its low 32 bits - but musl
   refuses the whole format, and the call wrote nothing. Every IMAP `FETCH` item sent
   as a literal - `BODY[...] {n}`, `BINARY[...]`, `BINARY.SIZE`, `PREVIEW` - lost its
   name and size, and 23 tests of the suite failed for it. The translator now
   rewrites `%I` alone as C's `%z`, for glibc and musl alike. A test program of the
   two translators passed on both; the committed ones had written nothing on musl,
   and 705032704 for 5000000000 on glibc.

And one thing that was the test machine's, not musl's: the backup tests need
7-Zip, which the CI image has and a bare Alpine container has not, so the suite's
job installs Alpine's `7zip` (and the MariaDB connector, as the CI image has it).

The CI jobs
-----------

- **`alpine-package`** (build): `build/ci/alpine.sh package` in `alpine:3.24`, pinned
  by the digest of its linux/amd64 manifest. It unpacks the `source-tarball` job's
  tarball and checks it, copies the APKBUILD and its install scripts out of it into
  an aports-shaped directory, runs `apkbuild-lint`, makes a signing key for the job
  alone, runs `abuild checksum` and `abuild -r` - the build, `check()` and the
  packages - holds the licences the server's package carries in
  `/usr/share/hmailserver/licenses` (the AGPL-3.0 text and the notices of QuickJS,
  Unicode's data for UTS 46, HTML's list of named character references and the
  Feather icons the webmail's page is drawn after, which the install rules put
  there) to the tarball's files by SHA-256
  (`build/ci/alpine.sh licences` runs that step alone), and keeps the packages,
  the key's public half, `abuild.log` and `lint.txt`.
- **`alpine-install`** (test): `build/ci/alpine.sh install` in a clean container:
  the packages installed with `apk add` beside Alpine's PostgreSQL 18 in a cluster of
  its own, the account, the unit-less layout and the modes checked, the database made
  as above - `--create-database` must say that the OpenRC service's next start
  secures it - then `rc-service hmailserver start` (OpenRC told it runs in a container,
  `rc_sys="docker"`, and that the container's network is there), the server checked
  to run as `hmailserver`, SMTP (220, STARTTLS in EHLO, 221) and IMAP (`* OK`) asked,
  a reload and the same again, and `rc-service hmailserver stop`.
- **`alpine-suite`** (test): `build/ci/alpine.sh suite` - the installed package's
  `/usr/bin/hmailserver` run through `build/ci/linux-suite.sh` (the whole of
  `LinuxRegressionTests`) beside the PostgreSQL 18 service `linux-suite` uses. The
  suite runs in Alpine, since a musl server does not run elsewhere, with Alpine's
  `dotnet10-sdk`, which is the 10.0.303 `global.json` names.

They run for a push to master or a `batch*` branch that changes more than
documentation, never for a tag, a merge request or a schedule; release needs none of
them. `alpine-package` joins `linix-package-build`, the resource group of the package
builds, and `alpine-suite` joins `linix-suite` with `linux-suite`, so that the two
suites, each hours long, never share linix's CPUs.

What has run
------------

On linix on 29 September 2026, in `alpine:3.24` (3.24.2) pinned as the jobs pin it,
from `batch42` with this item's changes, beside `batch42`'s own pipeline:

- **The first musl build** (CMake and Ninja by hand, `Release`, musl's own iconv):
  every object but `File.cpp`'s compiled; with its fix the server and the eight
  tester programs linked, `--version` answered `hMailServer 6.3.5 build 44`, and every
  tester passed.
- **The Linux suite against that server**, by hand, with the tarball's suite and
  Alpine's `dotnet10-sdk` 10.0.303: 663 tests passed and two failed before the run
  was stopped when the VMware host's restart was announced - the backup test, for
  want of 7-Zip in the container, and `TextAddedToAnEmptyBodyIsWritten`, the
  US-ASCII name (above), which passed in `batch42`'s own glibc suite the same day.
  A probe of musl's iconv then found the missing UTF-7, ISO-2022-JP-2 and BIG5
  (above).
- **`alpine.sh package`**, the job's script, in a fresh container on a source tarball
  made by `build/ci/source-tarball.sh` from a scratch commit of the tree:
  `apkbuild-lint` had nothing to say, `abuild checksum` took the tarball from
  `/var/cache/distfiles` (its SHA-512 is the file's; nothing was downloaded), and
  `abuild -r` built with GNU libiconv - CMake's new check passing - ran `check()`
  (the version and the eight testers, all passing) and made `hmailserver` (11.5 MB),
  `hmailserver-openrc` and `hmailserver-cli`, in 40 minutes with four compilers.
  abuild's only warnings were apk's, that the new repository had no index yet.
- **`alpine.sh install`**, in a clean container: the packages installed, the account,
  the modes (0750 and 0640 `root:hmailserver`, 0750 `hmailserver` for the data and
  log directories) and the unit's absence checked, the database made, the
  administrator password and the stored-secrets key set, and `rc-service hmailserver
  start` made the first start's certificate and turned STARTTLS on; the server ran as
  `hmailserver` with `CapAmb` and `CapEff` `0x400` (`CAP_NET_BIND_SERVICE` alone),
  SMTP greeted with 220 and offered STARTTLS, IMAP greeted `* OK IMAPrev1`, and it
  reloaded and stopped. OpenRC in a container says it cannot make its cgroup
  (`/sys/fs/cgroup` is read-only there), which does not stop it.
- **`alpine.sh suite`**, the job's script, against PostgreSQL 16, twice. First on a
  package from before the `%Iu` fix, stopped once the fix was built, at 1404 passed,
  281 skipped and 23 failed, the IMAP literals (above); the backup and empty-body
  tests the first run failed passed. The 23 then passed alone against the fixed package.
  Then the whole suite on the final package: **1897 passed, 331 skipped, none
  failed, of 2228**: the counts of `batch42`'s own glibc `linux-suite` the same day,
  in 142 minutes of `dotnet test` (glibc's took 124). The suite's
  `SslSetup.Reinitialize` waits its full 15 seconds for a restart it did not see go
  down, which on a quiet machine is often, so the run is slower the quieter the
  machine; the job's four hours hold it.
- **The re-check, 30 September 2026**, on the tree rebased onto `batch42`'s
  `c97fa7f03` - 6.3.6's Public Suffix List, SURBL, backup and live-update code, never
  built with musl before, and eleven tester programs - with the re-check's fixes,
  from a source tarball `source-tarball.sh` made of that commit. `alpine.sh package`
  in 28 minutes: `apkbuild-lint` had nothing to say, `check()` ran `--version` and all
  eleven testers, and the package depends on `ca-certificates-bundle`.
  `alpine.sh install` passed, `--create-database` now saying that the OpenRC
  service's next start secures the new database, which that start then did.
  `alpine.sh suite`: 1898 passed, 332 skipped and one failed of 2231, in 151 minutes
  of `dotnet test` on a quiet linix. The one was `EventLogIsWrittenAsUtf16`, which
  still asked for UTF-16's byte order mark after 6.3.6 made the Linux event log
  UTF-8, on glibc as on musl; the test now asks each platform for its own encoding,
  and passed alone against the same server.
- **The three jobs in `batch42`'s pipeline 2896082335**, 30 September 2026, on
  linix, all passed: `alpine-package` built hmailserver 6.3.6 from the pipeline's
  source tarball in 1,781 s, its `check()` running the testers; `alpine-install`
  installed the packages, `rc-service` started the server as `hmailserver`, SMTP
  with STARTTLS and IMAP answered, and it reloaded and stopped; and `alpine-suite`
  ran the whole Linux suite against it, 1899 passed, 332 skipped and none failed of
  2231. In 2897966924 the three were cancelled with the pipeline before they started.
- **The licences, for 6.3.7**, on server2 on 14 October 2026: `alpine.sh package`
  on a source tarball of the commit that installs the two data sets' notices -
  `apkbuild-lint` had nothing to say - made a package whose `.PKGINFO` licence
  names `Unicode-3.0 AND BSD-3-Clause` and that carries `AGPL-3.0.txt`, `QuickJS.txt`,
  `Unicode-data.txt` and `HTML-named-character-references.txt`, each the tarball's
  file; `alpine.sh licences`, run over the package `batch48`'s `6e1a336c5` made,
  refused it: it carries no `Unicode-data.txt`.

What is left
------------

- **aports.** The owner's part: a named maintainer with an account on
  `gitlab.alpinelinux.org`, who opens the merge request (above). Packages in
  `testing` move to `community` later, by Alpine's own process.
- **Row 71's test** - on a clean Alpine edge with `testing` enabled, `apk add
  hmailserver` installs the current release, `rc-service hmailserver start` starts
  it and port 25 answers - waits for aports. `alpine-install` runs the same steps on
  the package the pipeline built.
- **aarch64** is in `arch` and built nowhere; aports' builders would build it.
- **The auto-ban firewall hook** is installed at `/usr/lib/hmailserver/autoban-hook`,
  where the server looks, and changes nftables or iptables only with
  `CAP_NET_ADMIN`, which the OpenRC service does not grant: set
  `capabilities="^cap_net_bind_service,^cap_net_admin"` in `/etc/conf.d/hmailserver`,
  deliberately, as the hook's header says for the unit. Not tried.
