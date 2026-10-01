The source packages: Debian and RPM
====================================

**A Debian source package and an RPM spec, each built from the release's source
tarball the way a distribution builds a package of its own: without the live
update, which the distribution's package manager stands in for, and against the
distribution's own shared Boost. They are what a Debian, Ubuntu, Fedora, EL or
openSUSE maintainer starts from, and GitLab CI builds both for seven releases
with the network refused, holds them to lintian and rpmlint, and installs and
starts the result in a clean container.**

| | Debian and Ubuntu | Fedora, EL and openSUSE |
|---|---|---|
| The packaging | `hmailserver/source/Server/platform/packaging/debian-src/debian/` | `hmailserver/source/Server/platform/packaging/rpm/hmailserver.spec` and `hmailserver.sysusers` |
| The source | `hmailserver_<v>+ds.orig.tar.gz`: the release's `hmailserver-<v>.tar.gz` repacked by `Files-Excluded` | `hmailserver-<v>.tar.gz` as the release carries it |
| Binary packages | `hmailserver`, `hmailserver-cli` | `hmailserver`, `hmailserver-cli` |
| CI (`.gitlab/ci/source-packages.yml`) | Debian 13, Ubuntu 24.04, Ubuntu 26.04 | Fedora 44, EL 10 (AlmaLinux 10), openSUSE Leap 16.0, Tumbleweed |

**Where it stands (29 September 2026):** built on the laptop - the Debian source
package with `dpkg-buildpackage` in WSL Ubuntu 26.04, lintian clean of errors and its
autopkgtest passing in a clean container; the spec with `rpmbuild` in Fedora 44 and
AlmaLinux 10 containers, rpmlint clean of errors, and the server it builds installed
and started in a clean container of each. A Fedora server that died
before `main` on 29 September was a file the laptop's WSL had corrupted, not a
defect of GCC 16, Fedora's flags or the server
([The Fedora crash](#the-fedora-crash-of-29-september-2026)). Not yet in a
pipeline: the seven targets' first run is `batch42`'s pipeline #1, once
`.gitlab-ci.yml` includes the file. See [What has run](#what-has-run). No
distribution carries either yet; Debian's, Fedora's and
openSUSE's own archives each need a named maintainer (Roadmap3, rows 66, 68 and
70), and these packages are what that person starts from.

How they differ from the project's own packages
-----------------------------------------------

The project's `.deb` and `.rpm` (`hmailserver_<v>_amd64.deb` and its siblings,
[PackagesDeb.md](PackagesDeb.md), [PackagesRpm.md](PackagesRpm.md)) are made by
CPack for anyone to install on any release, and update themselves. A
distribution's package is rebuilt by the distribution for each of its releases
and updated by its package manager, so these differ where that matters:

| | The project's packages | The source packages |
|---|---|---|
| Built by | CMake and CPack | `dpkg-buildpackage` (debhelper 13, `cmake+ninja`) and `rpmbuild` (`%cmake`) |
| The live update | on: `hmailserver-update` and its two units | off (`-DHM_LIVE_UPDATE=OFF`): no apply step, no helper, no units; the update check names `apt upgrade hmailserver`, `dnf upgrade hmailserver` or `zypper update hmailserver` (`-DHM_UPDATE_COMMAND`, [LiveUpdateKinds.md](LiveUpdateKinds.md)) |
| Boost | linked statically, so one package installs on every release | the release's shared Boost (`-DHM_BOOST_STATIC=OFF`): its sonames are in `Depends` and `Requires`, and the archive rebuilds the package when its Boost moves |
| `hmctl` | in the server's package, with Python a recommendation | its own package, `hmailserver-cli`, which depends on `python3`; the server recommends it. Its first line names `/usr/bin/python3` |
| The account | made by `postinst` or `%post` with `adduser` or `useradd` | a `sysusers.d` file: `dh_installsysusers` on Debian, rpm itself on Fedora 42 and later, `%sysusers_create_compat` on EL, `%sysusers_generate_pre` on openSUSE |
| The directories | made by `postinst` or `%post` | a `tmpfiles.d` file on Debian; owned by the package with their modes on RPM |
| The unit | enabled, not started | Debian: enabled, not started (`dh_installsystemd --no-start`); RPM: the distribution's preset decides (`%systemd_post`, `%service_add_post`), which leaves it disabled unless a preset names it |
| An upgrade | the schema upgraded, a running server restarted | the same: `postinst` or `%post` runs `hmailserver --upgrade-database` when a database is configured, then `try-restart` (`%systemd_postun_with_restart` on RPM) |
| A purge (Debian) | keeps the logs and the mail store | removes `/var/log/hmailserver`, as Debian Policy 10.8 asks; keeps the mail store, the database and the account |
| The tester programs | run by `linux-build` | run by the build itself: `dh_auto_test` and `%check` run `spf-tests`, `script-binding-tests`, `snmp-tests`, `object-store-signature-tests`, `singleton-tests`, `legacy-blowfish-tests`, `xml-tree-tests`, `datetime-tests`, `public-suffix-tests` and, from 6.3.6, `backup-tests`, `trust-anchor-tests` and `business-object-tests` - the first two of those on scratch files of their own under `$TMPDIR`, removed again, with no network, none of the system's configuration and no root - and both first run `hmailserver --version`, so a server that cannot start fails the build |

`-DHM_BOOST_STATIC` is new with these packages: `CMakeLists.txt` set
`Boost_USE_STATIC_LIBS` itself, which hid any value given on the command line, and
now takes it from the option, ON by default, so the project's own packages are
built as before.

The repack, and the test data with no licence
---------------------------------------------

The tarball is the release's own (`build/ci/source-tarball.sh`,
[SourceTarball.md](SourceTarball.md)): it already leaves out every path of
`build/source-exclusions.txt`, what is not free and what only the Windows build
uses. `debian/copyright`'s `Files-Excluded` begins with that list, as
`python3 build/check-source-tarball.py --files-excluded` prints it, so a tarball
uscan made from a git tag would hold the same files.

The Debian package leaves out five files more. The tarball keeps the whole test
tree because the suites and the fuzzing read it, and SourceTarball.md names the
test data in it that states no licence: the replies of three DKIM verifiers and an
Outlook message under `hmailserver/test/TestData/DKIM` (`altn.response.txt`,
`elandit.response.txt` and `Test 1.eml` under `Good`, `port25.response.txt` under
`PermFail`), and a security advisory's text,
`hmailserver/test/RegressionTests/Resources/SecuniaBody1.txt`, which the Windows
suite embeds. A Debian archive needs a licence for every file it distributes, and
the Debian build reads none of the five - it builds the server and runs the tester
programs, and reads nothing under `hmailserver/test` - so they are removed:
`Files-Excluded` lists them after the project's list, `mk-origtargz` (uscan's
repack) removes them, and the tarball takes the `+ds` suffix:
`hmailserver_<v>+ds.orig.tar.gz`, version `<v>+ds-1`. The rest of the DKIM
messages are, by their names and contents, Mail::DKIM's test corpus, under Perl's
terms, and are declared as such.

`build/check-source-tarball.py`, in the `checkers` job, holds this: the field must
begin with the list as it stands, and each pattern after it must match at least
one file, only files under `hmailserver/test/`, and none that `CMakeLists.txt` or
`debian/rules` reads. The CI build holds the repack to it as well: the repacked
tarball must have lost exactly those five files and gained none.

The RPM spec builds from the project's tarball as it is: rpm's source package
carries the tarball whole, and the five files are neither read nor installed. A
distribution that wants them out of its source package too repacks the way
Debian does; its `%prep` already removes every path of the list, for a source made
another way, and removes nothing from the project's tarball.

The copyright file, and the copy of QuickJS
-------------------------------------------

`debian/copyright` is in the machine-readable format (DEP-5). Beyond the
project's own files (AGPL-3.0-or-later, Martin Knafve's and the maintainers'), it
declares every third-party notice in the repacked tarball: QuickJS (MIT, as
`Expat`), the MIME coder (BSD-1-Clause), `CStdString` (its own notice), the RFC
7208 test suite (BSD-3-Clause), the Public Suffix List's data compiled into the
server (MPL-2.0), the ATL script host, which only the Windows server
compiles (its own notice), the DKIM corpus (Perl's terms), and the licence texts under
`hmailserver/docs/Licenses`, which are notices of components not in the source.
Since 6.3.6 no file is declared `none-stated`, a declaration a Debian archive does not accept
([the questions below](#questions-for-a-debian-or-fedora-maintainer)). The AGPL's
text is written out in full: Debian's `/usr/share/common-licenses` does not carry
it.

QuickJS 2026-06-04 is vendored unmodified under `hmailserver/source/Server/quickjs`
and linked into the server. No release these packages build for ships QuickJS as a
library the server could link instead, which is the embedded copy's justification;
the spec says so with `Provides: bundled(quickjs) = 2026.06.04`, as Fedora asks, and
`debian/copyright` in the paragraph's comment.

The jobs
--------

`.gitlab/ci/source-packages.yml`, two jobs for each of the seven releases, x86-64
only:

| Release | Image | Built by | Linted by |
|---|---|---|---|
| Debian 13 | `debian:13`, packages-deb.yml's digest | `srcpkg-debian13` | lintian |
| Ubuntu 24.04 | `ubuntu:24.04`, packages-deb.yml's | `srcpkg-ubuntu24.04` | lintian |
| Ubuntu 26.04 | `ubuntu:26.04`, packages-deb.yml's | `srcpkg-ubuntu26.04` | lintian |
| Fedora 44 | `registry.fedoraproject.org/fedora:44`, packages-rpm.yml's | `srcpkg-fedora44` | rpmlint |
| EL 10 | `almalinux:10`, by its amd64 manifest's digest | `srcpkg-el10` | rpmlint, from EPEL |
| openSUSE Leap 16.0 | `registry.opensuse.org/opensuse/leap:16.0`, packages-rpm.yml's | `srcpkg-leap16` | rpmlint |
| openSUSE Tumbleweed | `registry.opensuse.org/opensuse/tumbleweed`, packages-rpm.yml's | `srcpkg-tumbleweed` | rpmlint |

EL 10 is AlmaLinux 10 and not Red Hat's UBI 10, which the project's EL packages use:
UBI carries no `boost-devel`, which those jobs build from boost.org's tarball, while
a distribution's build takes Boost from the distribution, in CodeReady Builder on EL
10. rpmlint is EPEL's there.

**The build job** (`build/ci/packages/srcpkg-deb.sh`, `srcpkg-rpm.sh`) takes the
`source-tarball` job's artifact, and the packaging from inside the tarball, never
from the checkout, so what is built is what the tarball carries. With the network,
it installs the packaging tools and then the build's dependencies as the packaging
names them (`apt-get build-dep` on the source tree; `rpmspec -q --buildrequires`
for dnf or zypper). Then it builds with the network refused - `dpkg-buildpackage -us
-uc` or `rpmbuild -ba` - and runs lintian or rpmlint over what it built: an error
fails the job, and every warning is printed in the log and kept in `lintian.txt` or
`rpmlint.txt`. lintian runs with Debian's profile on the Ubuntu releases too: the
changelog names `unstable`, which Ubuntu's profile refuses as a distribution, and a
PPA upload names an Ubuntu series in its own changelog entry. Ubuntu 24.04's lintian
(2.117) takes a JavaScript or HTML file with a line of 512 characters or more for
minified code whose source is missing (`source-is-missing`, an error) and so reported
five files written by hand, which batch42's pipeline found on 30 September 2026:
`debian/source/lintian-overrides` overrides those five, with the reason, and the
lintian of Debian 13 (2.122) and of Ubuntu 26.04 (2.129) reports none of them. Last it holds the binary package to what was asked for: a Boost
soname among its dependencies, no `hmailserver-update` in it, the `sysusers.d`
file, and `hmctl` in `hmailserver-cli`. The debug information packages
(`hmailserver-dbgsym`, `hmailserver-debuginfo` and `-debugsource`) are built and
linted but not kept in the job's artifacts: nothing reads them, and Fedora's
debuginfo alone is 245 MB. The builds are in the resource group
`linix-package-build`, with the other package builds, one at a time on linix.

**Memory, and no LTO.** A distribution builds with debug information (`-g`), and
thirteen programs link the whole static core: the server, `openpgp-writer` and
the eleven tester programs `dh_auto_test` and `%check` run. One GNU `ld` of them reaches 3.0 GB with link-time
optimisation (measured on the laptop with one link at a time, 28 September; a
sampler of that day's LTO links also saw one at 6.0 GB), and 1.5 GB without it on
Fedora 44 and 0.9 GB on Ubuntu 26.04 (29 September), where one compiler peaked at
1.5 GB. Several links at once under
`-j4`, beside another build, ran WSL's 10 GB out of memory on 28 September 2026: the
kernel's OOM killer took a linker at 2.3 GB, and the build did not finish. So both packagings
pass CMake a Ninja pool of one for links (`-DCMAKE_JOB_POOLS=link=1
-DCMAKE_JOB_POOL_LINK=link`) - a pool only Ninja knows, so the spec sets
`CMAKE_GENERATOR=Ninja` for EL 10, whose `%cmake` would write Makefiles, where
Fedora's passes `-G Ninja` and openSUSE's builds with Ninja by the spec's
`__builder` - and the jobs run four compilers rather than the
package files' six, in linix's 8 GB jobs. Both also turn off the link-time
optimisation their distributions build with by default, for that memory: the spec
for Fedora, EL and openSUSE (`%global _lto_cflags %{nil}`), and `debian/rules` for
Ubuntu, whose `dpkg-buildflags` add `-flto=auto -ffat-lto-objects` where Debian's
do not (`optimize=-lto`): 3.0 GB in the linker alone, besides the LTRANS compilers
`-flto=auto` starts, one per processor the build sees. That is the only reason. The one Fedora 44 LTO link tried, on 28
September 2026, failed (`lto-wrapper`: an `ltrans` object with "section ... is
missing"), but it ran on the laptop whose file system was found the next day to
lose written data ([The Fedora crash](#the-fedora-crash-of-29-september-2026)), so
it says nothing about GCC 16: whether GCC 16 links the server with LTO is untested.
A packager with the memory may turn it on.

**The network is refused by a seccomp filter** (`build/ci/packages/offline.c`),
not by `docker run --network none` or `unshare -n`. A job on linix runs in an
unprivileged container with no Docker socket, and Docker's default seccomp profile
refuses `unshare(2)` to it: every `unshare -n`, `-rn` and `-Urn` was refused in a
default container on 28 September 2026. What any process may do is install a
filter of its own under `no_new_privs`, which every child inherits and none can
lift. The job compiles `offline.c` with the release's compiler before the network
is refused and runs the build under it: `socket(2)` fails with `EACCES` for every
address family but `AF_UNIX` and `AF_NETLINK` - no IPv4 or IPv6 socket, no packet
socket, no DNS query, loopback included - `io_uring_setup(2)` fails, since io_uring
can open a socket without `socket(2)`, and a system call from another
architecture's table is refused. It refuses making a socket, not using one: a
network socket already open when it starts would stay usable, and the jobs' shells
hold none. The job first proves it: a name lookup under it must fail.

**The install job** (`build/ci/packages/srcpkg-install.sh`) runs in a clean
container of the same release, the job's own image, which has never had a build in
it. For Debian and Ubuntu it runs the package's autopkgtest from the `.dsc` against
the two `.deb` files, with autopkgtest's `null` runner: autopkgtest installs the
packages and the test's dependencies from the release's archive, and the test,
`debian/tests/server-starts`, starts the server against a PostgreSQL cluster
`pg_virtualenv` makes for it and asks SMTP and IMAP for their greetings. The test
declares `isolation-container`, since it starts a service on port 25; the job's
container is that isolation, so the job passes `--ignore-restrictions` for that
one restriction. For the RPM releases dnf or zypper installs the two packages with
what they require; the job holds the account, the directories' modes, the
configuration's owner and `hmctl`'s first line to what the spec says, makes a
UTF8 PostgreSQL cluster with the release's own `initdb` (a container has no locale
to take the encoding from), runs the same test, and removes the packages.

The test (`debian/tests/server-starts`) works on copies: the packaged
configuration copied and edited as the packaging README tells an administrator, and
directories of its own under `/tmp`. As root it runs the verbs as an administrator
does (`--check-config`, `--create-database`, `--set-admin-password`,
`--apply-new-install-defaults`), starts the server as the unit does - as
`hmailserver`, with `CAP_NET_BIND_SERVICE` alone and no new privileges - and
requires a `220` greeting, `STARTTLS` among EHLO's extensions (the first start's
defaults turn it on), `221` to QUIT, and an IMAP `* OK` greeting. On a testbed
with something already on port 25 it moves the listeners up by 20000 first.

**When:** in a `batch*` branch's pipeline and in master's, wherever `source-tarball`
runs, since every rule that makes one of these jobs is `.own-runners`' own. Never in
a `v*` tag's pipeline: they check and make nothing a release carries, the tag's
tree has passed them in its batch's pipeline, and `release` needs none of them.
Never for a merge request or an unprotected ref.

Building them by hand
---------------------

With the tarball from `bash build/ci/source-tarball.sh` (or a release's), in the
release's own container as root, from the root of a checkout:

```
HM_SRCPKG_TARGET=debian13 HM_SOURCE_TARBALL=source-tarball bash build/ci/packages/srcpkg-deb.sh
HM_SRCPKG_TARGET=debian13 bash build/ci/packages/srcpkg-install.sh      # a clean container
HM_SRCPKG_TARGET=fedora44 HM_SOURCE_TARBALL=source-tarball bash build/ci/packages/srcpkg-rpm.sh
HM_SRCPKG_TARGET=fedora44 bash build/ci/packages/srcpkg-install.sh      # a clean container
```

The results are in `source-packages/<release>/`; `HM_WORK_DIR` names where it
builds (`/tmp/hm-srcpkg`) and `HM_BUILD_JOBS` how many compilers run (4).

Or the way a Debian maintainer would, from the tarball:

```
mkdir work && cd work
tar -xzf ../hmailserver-<v>.tar.gz hmailserver-<v>/hmailserver/source/Server/platform/packaging/debian-src/debian
mk-origtargz --package hmailserver --version <v> --repack-suffix +ds --compression gzip \
  --copyright-file hmailserver-<v>/hmailserver/source/Server/platform/packaging/debian-src/debian/copyright \
  --directory . ../hmailserver-<v>.tar.gz
rm -rf hmailserver-<v> && tar -xzf hmailserver_<v>+ds.orig.tar.gz
cp -a hmailserver-<v>/hmailserver/source/Server/platform/packaging/debian-src/debian hmailserver-<v>/debian
cd hmailserver-<v> && sudo apt-get build-dep ./ && dpkg-buildpackage -us -uc -j4
```

and for the spec, `rpmbuild -ba hmailserver.spec` with the tarball and
`hmailserver.sysusers` in `SOURCES`.

The watch file
--------------

`debian/watch` reads the project's releases from GitLab's API
(`https://gitlab.com/api/v4/projects/86730559/releases`), where each release links
its files in the project's package registry, the tarball among them, and takes
`hmailserver-<v>.tar.gz` from there: nothing reaches GitHub, and the project's id
stays when it moves group. uscan repacks it by `Files-Excluded` with the `+ds`
suffix. The tarball's signature is a Sigstore bundle, which uscan cannot check, so
the watch file names no signing key; verify the tarball with `cosign
verify-blob` and its bundle, as SECURITY.md's "Verifying a release" describes for
every release file. No release carries a tarball before 6.3.6, so until then uscan
finds nothing.

Stamping a release
------------------

The two formats need the version written out, and neither can take it from
`Version.h`: `debian/changelog`'s one entry (`<v>+ds-1`, with the date), and the
spec's `Version:` and its one `%changelog` entry (`- <v>-1`). Each keeps one entry,
the release's, rewritten when a release is stamped - an entry for an older release
would be a literal to go stale - and `build/check-linux-version-stamp.sh`, in the
`checkers` job and `linux-build`, fails when either disagrees with `Version.h` or
holds a second entry. A distribution's own changelog is the distribution's.

What has run
------------

On the laptop on 28 and 29 September 2026, before any pipeline, from the tarball
`build/ci/source-tarball.sh` made of the commit that added these files (Git for
Windows; `check-source-tarball.py --commit` passed it), with the build scripts CI
runs, in WSL Ubuntu 26.04 and its Docker, two compilers at a time:

- **Debian, Ubuntu 26.04** (`srcpkg-deb.sh` in WSL itself, root): `mk-origtargz`
  repacked the tarball as `hmailserver_6.3.5+ds.orig.tar.gz`, "deleting 5 files",
  3603 entries to 3597, the five exactly; `apt-get build-dep` took the release's
  Boost 1.90, GCC 15.2, CMake 4.2.3 and debhelper 13.31; `dpkg-buildpackage -us -uc
  -j2` under `offline.c` (a name lookup refused first) exited 0 after 3020 s, the
  tester programs passing in `dh_auto_test`. lintian 2.129 with Debian's profile:
  **no error**, 7 warnings - `initial-upload-closes-no-bugs` for both packages (no
  ITP yet), `no-manual-page` for `hmailserver`, `hmctl`, `tlds.txt` and
  `dh2048.pem`, and `uses-dpkg-database-directly` for the server, whose update
  check reads dpkg's record to know how it was installed - and 5 info tags (watch
  format 4, Standards-Version 4.7.2 where 4.7.3 is current, three runtime files
  counted as documentation outside `/usr/share/doc`). `Depends` names
  `libboost-chrono1.90.0`, `libboost-filesystem1.90.0` and `libboost-thread1.90.0`
  (Boost linked dynamically), the `sysusers.d` and `tmpfiles.d` providers, and no
  `hmailserver-update` is in the package.
- **The autopkgtest, in a clean `ubuntu:26.04` container** (`srcpkg-install.sh`,
  the digest `packages-deb.yml` pins): autopkgtest 5.x installed the two `.deb`
  files and PostgreSQL 18 from the archive, and `pg_virtualenv
  debian/tests/server-starts` created the database, set the administrator
  password, applied the first start's defaults and started the server as
  `hmailserver` with `CAP_NET_BIND_SERVICE`: `220` on port 25, `STARTTLS` among
  EHLO's extensions, `221` to QUIT, and `* OK IMAPrev1` on 143. **PASS**.

- **RPM, Fedora 44** (`srcpkg-rpm.sh` in `registry.fedoraproject.org/fedora:44`,
  the digest `packages-rpm.yml` pins, from the tarball of the commit that added
  the rpmlint filters): the BuildRequires from Fedora's repositories (Boost 1.90,
  GCC 16, CMake 4.3), `rpmbuild -ba` under `offline.c` exited 0 after 3859 s with
  `%check`'s four tester programs passing, and rpmlint 2.8, with the package's
  filters: **no error**, 21 warnings - `non-standard-uid`/`-gid` for the account's
  directories and the configuration, `non-executable-in-bin` for the two data files,
  `no-manual-page-for-binary` for `hmailserver` and `hmctl`, `no-documentation` for
  `hmailserver-cli`, `crypto-policy-non-compliance-openssl` (the server sets its own
  cipher list, which Fedora's crypto policy would set), `binary-or-shlib-calls-
  gethostbyname`, and the container's missing locale twice. `Requires` names
  `libboost_chrono.so.1.90.0`, `libboost_filesystem.so.1.90.0` and
  `libboost_thread.so.1.90.0`. Before that: a first build with Fedora's LTO failed
  its link (above), and a second passed `%check` but not rpmlint (16 errors: the
  logrotate dependency and two words of the description, since fixed, and the
  deliberate modes and data files, since filtered).
- **The Fedora install, in a clean `fedora:44` container** (`srcpkg-install.sh`):
  dnf installed both packages with their requirements; rpm made the `hmailserver`
  account from the `sysusers.d` file itself, and the directories and the
  configuration had the modes and owners the spec gives; PostgreSQL 18.6 came up.
  **The server did not start: `/usr/bin/hmailserver` - even `--version` - died of a
  segmentation fault before `main`**, and in one of the three Fedora builds that
  reached `%check` `script-binding-tests` had died the same way. The cause was not
  GCC 16, Fedora's flags or the server's code but the laptop: WSL's file system
  lost part of what the linker wrote ([The Fedora crash](#the-fedora-crash-of-29-september-2026)).
- **The spec again, Fedora 44, on 29 September**, from a tarball of the change
  that has `%check` run the server's `--version` before the testers, with the
  build's work directory on a loop-mounted ext4 (below): `rpmbuild -ba` under
  `offline.c` exited 0 after 2888 s, `%check` printing `hMailServer 6.3.5 build 44`
  and its four testers passing, and rpmlint 2.8 reported **no error** and the same
  21 warnings. The spec's `CMAKE_GENERATOR` line (EL 10, below) came after this
  build; Fedora's `%cmake` passes `-G Ninja`, which overrides it, and rpmlint with
  the package's filters finds nothing in the final spec.
- **Its install, in a clean `fedora:44` container**: dnf installed both packages,
  `/usr/bin/hmailserver --version` answered, and `server-starts` created the
  database on PostgreSQL 18.6 at schema version 6105, set the administrator
  password, applied the first start's defaults and started the server, which
  answered `220` with `STARTTLS` among EHLO's extensions, `221` to QUIT and
  `* OK IMAPrev1` on 143; the packages came off again. **PASS**, once
  `srcpkg-install.sh` made its cluster UTF8 (`initdb --encoding=UTF8
  --no-locale`): a container has no locale, so `initdb` had made it SQL_ASCII, and
  PostgreSQL refused the server's `CREATE DATABASE ... ENCODING 'UTF8'` - a step no
  earlier run had reached.
- **EL 10** (`srcpkg-rpm.sh` in `almalinux:10`, the digest `source-packages.yml`
  pins, AlmaLinux 10.2, with CodeReady Builder and EPEL; Boost 1.83, GCC 14.3,
  CMake 3.31, the work directory on the loop-mounted ext4): its `%cmake` wrote
  Makefiles, so the link pool did nothing, and the spec now sets
  `CMAKE_GENERATOR=Ninja` (above); with it, `rpmbuild -ba` under `offline.c` exited
  0 after 3971 s, `%check` printing the version and its four testers passing, and
  EPEL's rpmlint 2.8 reported **no error** and 85 warnings - Fedora's 21, the
  container's missing locale twice more, and `files-duplicate` 62 times for the
  database scripts that are the same for two databases.
- **Its install, in a clean `almalinux:10` container**: dnf installed both
  packages, rpm's `%sysusers_create_compat` made the account, and `server-starts`
  against PostgreSQL 16.15 created the database, started the server and had
  `220` with `STARTTLS`, `221` and `* OK IMAPrev1`; the packages came off again.
  **PASS**.

- **The re-check, 29 September.** The Debian package again, Ubuntu 26.04, from the
  tarball of the commit that turned Ubuntu's LTO off, added the server's `--version`
  to `dh_auto_test` and took the configuration out of `tmpfiles.d` (sha256
  `794b6e6f...`), in an `ubuntu:26.04` container limited as a linix job is (8 GB,
  six CPUs), two compilers, everything on a loop-mounted ext4: no `-flto` on any
  line of the build, `dpkg-buildpackage` under `offline.c` exited 0 after 3200 s,
  `dh_auto_test` printing `hMailServer 6.3.5 build 44` and its four testers
  passing; the largest `ld` sampled 0.9 GB and the largest compiler 1.5 GB, the
  machine never above 3.7 GB. lintian: no error, the same 7 warnings and 5 info
  tags. The autopkgtest in a clean container: **PASS** (`220`, `STARTTLS`, `221`,
  `* OK IMAPrev1`). Then in another clean container the two `.deb` files installed
  with apt gave `/etc/hmailserver` 0750 and the configuration 0640
  `root:hmailserver`, and an override of the configuration to 0600 `root:root` with
  `dpkg-statoverride` held across a reinstall and a `systemd-tmpfiles --create
  --boot`, where the `z` line the package used to carry put 0640 back.
- **What the laptop could check of the other four, without building them.** With
  their own archives, in the images the jobs pin: `apt-get build-dep -s` meets
  every Build-Depends on Debian 13 (debhelper 13.24, Boost 1.83, CMake 3.31) and
  Ubuntu 24.04 (debhelper 13.14, Boost 1.83, CMake 3.28, OpenSSL 3.0), where
  `dh-sequence-installsysusers` is debhelper's own, and the test's Depends
  install; `setpriv` and `runuser` are `util-linux`'s, which is Essential. On
  Leap 16.0 and Tumbleweed the spec expands with openSUSE's own macros as meant -
  `%cmake` configures `hmailserver/source/Server` in `_build` with Ninja and
  without `-flto`, `%check` changes into `_build`, `%pre` runs the generated
  `hmailserver.pre` and `%service_add_pre`, and the other scriptlets are
  openSUSE's - and zypper resolves every BuildRequires (Boost 1.86 on Leap, 1.92 on
  Tumbleweed, GCC 15 and 16, PostgreSQL 18). openSUSE's rpmlint over the spec and
  over Fedora's two binary packages, with the package's filters, reported no error
  but one that belongs to Fedora's scriptlets
  (`systemd-service-without-service_add_pre`, which the openSUSE build's `%pre`
  has) and warned that `LicenseRef-Fedora-UltraPermissive` is not a licence it
  knows. Leap 16.0's rpmlint died at its first check - its package neither
  requires nor recommends `python3-packaging`, which it imports - so
  `srcpkg-rpm.sh` installs that beside it on openSUSE; with it, rpmlint runs there.
  The first pipeline to run the openSUSE jobs (batch42's, 30 September 2026) found
  three things those runs had not: Leap 16.0's and Tumbleweed's images have no
  `find`, which the script used before it installs anything, so it reads the
  tarball's directory with a glob; on Tumbleweed four packages provide
  `pkgconfig(openssl)` - `libopenssl-3-devel`, `libopenssl-devel`,
  `libressl-devel` and `aws-lc-devel` - and zypper chose LibreSSL, which CMake
  refused, so on openSUSE the spec asks for `libopenssl-devel >= 3.0`; and
  Tumbleweed's rpmlint 2.10 reports two errors about the debug packages alone
  (`statically-linked-binary` for the separate debug-information file, `no-binary`
  for the debug sources), which the script filters, with its reason, and the spec's
  `hmailserver.rpmlintrc` does not.

A first laptop run of the Debian build at `-j4` beside another build was killed
when WSL ran out of memory in the links (above); every result here comes from a
run that finished.

The Fedora crash of 29 September 2026
-------------------------------------

The server that died jumped, from `__libc_start_main`, to the image's base plus
0x47: the `.init_array` slot of `IMAPCommandCreate.cpp`'s static constructor held
0, which its `DT_RELR` relocation turned into the load base, and the ELF magic
there (`7f 45`, `jg +0x45`) went on to base + 0x47. Nothing in the objects asked
for 0: a second link of the same objects with the same command, in a container of
the same image, made a server that starts, and the two files differ in their
allocated sections in nothing but eleven whole input sections from four objects,
each all zero in the one that crashed - the static constructors of
`IMAPCommandCreate.cpp` and `SpamTestDMARC.cpp`, that `.init_array` slot, the
typeinfo, typeinfo names and vtables of `IMAPCommandCREATE` and
`IMAPCommandDeleteAcl`, a `.gcc_except_table` and an `.eh_frame` part - with the
relocations those sections carry missing too, and everything else, `.relr.dyn`
included, identical bar the build ID. Of nine more links of the same objects, one
came out the same way, with other sections zeroed and another `.init_array` slot
0. A flag, the compiler or the server's code cannot change bytes between two
links of the same objects; a lost write can. And a program with no linker in it
showed it: GNU `ld`'s 45,170 writes of the server, replayed with `lseek` and
`write` - the same offsets and lengths, in order, with data that is a function of
the offset - and read back at once, lost writes that `write` had acknowledged, read
back as zeros, in 12 of 280 rounds on WSL's own ext4 (kernel
6.18.33.2-microsoft-standard-WSL2, WSL 2.7.13), directly and through Docker's bind
mount; the two losses checked were still zeros read from the disk after `fsync`
and dropping the cache. On an ext4 in a loop-mounted image it lost nothing in 120
rounds, nor on tmpfs in 40. The Debian build ran on the same file system; its
autopkgtest passing says only that no loss fell where the test looks. The LTO link
that failed with an `ltrans` object "missing" a section ran there too. What is
wrong in WSL's storage is not known; its or the kernel's maintainers would have
to say.

So the spec needs nothing for GCC 16. What it lacked was a check that would have
stopped the build: `%check` ran the four testers, which the losses had missed, and
not the server, so a server that could not start was packaged. `%check` now runs
`./hmailserver --version` first, and so does the Debian package's `dh_auto_test`,
which had the same gap; every static constructor runs before `main`, so a
server that cannot start fails the build. The rebuilds of 29 September kept their
work directory on a loop-mounted ext4; anything else built in that WSL - a build
tree, a package, an image - can hold a hole that no one has run into yet.

What is not done
----------------

- **linix's file system.** The laptop's WSL loses written data
  ([The Fedora crash](#the-fedora-crash-of-29-september-2026)); whether linix, the
  runner these jobs and every package job use, can lose writes too is not known. Its
  kernel is not WSL's, but no one has replayed a link's writes there. A loss in a
  CI build shows only where something runs the damaged bytes: `%check` and the
  install jobs run the server, and the testers, and nothing checks the rest.
- **Pipeline #1.** Four of the seven targets - Debian 13, Ubuntu 24.04, Leap 16 and
  Tumbleweed - have not been built anywhere yet (their dependencies resolve and the
  spec expands on each: above), and no job of the file has run on
  linix: that is `batch42`'s pipeline. Their lintian (2.117 on Ubuntu 24.04, 2.122
  on Debian 13) and rpmlint (openSUSE's configuration) may report what the laptop's
  did not, and the jobs build at `-j4` where the laptop built at `-j2`.
- **Manual pages** for `hmailserver` and `hmctl`, which lintian and rpmlint ask for.
- **The distributions' archives.** Debian's (Roadmap3 row 66), Fedora's (row 68)
  and openSUSE's `server:mail` (row 70) each need a named maintainer, who takes
  these packages to the archive's review. The questions they will meet first are
  below.
- **A signature uscan can check.** The release signs its files with Sigstore; a
  Debian maintainer's `debian/upstream/signing-key.asc` needs an OpenPGP signature
  the project does not make.

Questions for a Debian or Fedora maintainer
-------------------------------------------

- **The unlicensed test data.** Leaving five files out is this package's answer;
  asking their authors for a licence, or replacing them with messages the project
  writes, would let the repack go.
- **No file compiled into the server is left whose licence is unsettled.** There
  were three until 6.3.6, inherited from the original hMailServer under
  `hmailserver/source/Server/Common/Util`, and the project replaced each with its
  own code: the Blowfish code (Bruce Schneier's of 1994, made a C++ class by Jim
  Conger in 1996, which stated no licence) with `LegacyBlowfish.cpp`, written from
  the cipher's published description; XMLite (Cho Kyung Min, 2002-2004, from The
  Code Project), the backup index's XML reader and writer, which carried only its
  author's own notice, with `XmlTree.cpp`; and `VariantDateTime` (Mark A. Richman,
  1999, from CodeGuru), which stated no licence and whose helpers kept the names
  of MFC's `COleDateTime`, with `DateTime.cpp`, written from the calendar and the
  OLE Automation date's definition. Each keeps the old code's behaviour, held to
  it by a differential test over millions of cases. Nor is a file left that the
  server reads and whose licence is unsettled: `tlds.txt`, SURBL's list of
  two-level domains of 2006, which stated none, gave way in 6.3.6 to the Public
  Suffix List the server already compiled in.
- **The Public Suffix List.** The server compiles the list's rules in
  (`PublicSuffixListData.h`), and DMARC, the SURBL lookups and the MX-record
  test read them. Debian ships the list as `publicsuffix` and Fedora as
  `publicsuffix-list`; using the distribution's copy needs a change to how the
  server loads it.
- **A data file in `/usr/bin`.** `dh2048.pem` is installed beside the binary
  because the server reads it from its own directory. A distribution would want
  it under `/usr/share/hmailserver`, which needs the server to look there.
- **The DKIM corpus.** Its files are declared as Mail::DKIM's by their names and
  contents; the project has no record of when they were taken.
- **`CStdString`'s licence.** Its notice is its own ("This code is 100% free. Use it
  anywhere you want."). The spec calls it `LicenseRef-Fedora-UltraPermissive`;
  Fedora's legal review decides the name, and openSUSE's rpmlint warns that it
  knows no licence of that name.
- **The unit's start on an RPM system.** The presets leave the unit disabled, and
  the project's own package enables it; either way nothing starts it before an
  administrator has configured a database.
- **The embedded QuickJS.** Justified above; a distribution that later packages
  QuickJS as a library could have the server link it, which is upstream work.
- **LTO.** The spec and, for Ubuntu, `debian/rules` turn it off for the memory six
  whole-core links take (above); Fedora's reviewers ask for a reason, and that is
  the one. Whether GCC 16 links the
  server with LTO is untested: the one try ran on the laptop's lossy file system.
- **The rpmlint filters.** `rpm/hmailserver.rpmlintrc` keeps the mail store's and the
  configuration's modes and the data file in `/usr/bin` from being reported; a
  reviewer may prefer other modes, or the data file moved (above).
- **`uses-dpkg-database-directly`.** The server's update check reads dpkg's record
  to know that a package installed it; with the live update off it only names the
  update command, and could stop reading it in a distribution's build.
