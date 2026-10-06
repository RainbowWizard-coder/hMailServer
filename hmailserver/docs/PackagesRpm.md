The RPM packages: Fedora, RHEL and openSUSE
===========================================

GitLab CI builds an `.rpm` of the server for six RPM distribution releases,
Fedora 44 and 43, RHEL 10 and 9, and openSUSE Leap 16.0 and Tumbleweed, each for
x86-64 (`x86_64`) and AArch64 (`aarch64`). Each is built in its own release's
container, with that release's compiler and libraries, and each is then
installed in a clean container of the same release and architecture. The jobs
are in `.gitlab/ci/packages-rpm.yml`, which `.gitlab-ci.yml` includes. The
scripts they run are in `build/ci/packages/`.

The generic x86-64 package, `hmailserver-<v>-1.x86_64.rpm`, stays
`linux-build`'s, as before. This file adds its AArch64 twin and the twelve
packages whose names carry a release (see [The names](#the-names)). From 6.3.6
the release carries the AArch64 twin, `hmailserver-<v>-1.aarch64.rpm`, and the
twelve, each with its SBOM, a line in the sums, a bundle and its `.sigstore.json`
copy: `release-packages` (`.gitlab/ci/release-packages.yml`) gathers them once
each build's install job has passed.

**The build and install jobs have run on GitLab.** All twelve build jobs passed
in `batch41`'s pipeline 2887115157 (27 September 2026), four of them already in
2886955494; in 2887524317 (28 September) the twelve install jobs ran for the
first time, and all twelve passed. Every build and install job passed again in
`batch41`'s 2890818010 and 2891564033, in 6.3.5's tag pipeline 2892097590, and
in `batch42`'s 2896082335 and 2897966924 (30 September 2026), which built 6.3.6
and held each package to its licence files (see
[In GitLab's pipelines](#in-gitlabs-pipelines)). Before that, the scripts
were run by hand on linix on 26 September 2026, in the jobs' own images and by
the same digests, through `build/ci/packages/rpm-local.sh`, which runs each
pair of jobs the way the runner would. The times under [By hand on
linix](#by-hand-on-linix) are those runs, while other package builds and test
runs were queued for the same machine.

What has run
------------

### In GitLab's pipelines

The twelve build jobs of this file, on linix, for the branch `batch41`. Each
time is the job's own, from its `BUILT:` line.

| Job | 2886955494 (`4a0e53cc8`) | 2887115157 (`70eb52b94`) |
|---|---|---|
| `rpm-fedora44-x86_64` | passed, 466 s | passed, 475 s |
| `rpm-fedora44-aarch64` | passed, 506 s | passed, 500 s |
| `rpm-fedora43-x86_64` | cancelled with the pipeline after 373 s | passed, 522 s |
| `rpm-fedora43-aarch64` | passed, 547 s | passed, 530 s |
| `rpm-el10-x86_64` | cancelled before it started | passed, 571 s |
| `rpm-el10-aarch64` | cancelled before it started | passed, 601 s |
| `rpm-el9-x86_64` | cancelled before it started | passed, 589 s |
| `rpm-el9-aarch64` | cancelled before it started | passed, 607 s |
| `rpm-leap16-x86_64` | passed, 718 s | passed, 714 s |
| `rpm-leap16-aarch64` | cancelled before it started | passed, 708 s |
| `rpm-tumbleweed-x86_64` | cancelled before it started | passed, 626 s |
| `rpm-tumbleweed-aarch64` | cancelled before it started | passed, 680 s |

Every build that passed made 6.3.5 (`hmailserver-6.3.5-1.<dist>.<arch>.rpm`),
ran the four tester programs - the aarch64 ones under qemu-user - with all
four passing, and packaged a `/usr/bin/hmailserver` that is a PIE with full
RELRO; `rpm-fedora44-aarch64` also wrote the generic
`hmailserver-6.3.5-1.aarch64.rpm`. Among them are the six pairs that had not
run by hand with the current scripts (below). The twelve install jobs were in both pipelines, and in
neither did one start before the pipeline was cancelled.

In pipeline 2887524317 (`63d58f8a6`, the same branch, 28 September 2026) the
twelve builds passed again, in 496 to 819 s, and the twelve install jobs ran for
the first time: each package installed with its dependencies in a clean
container of its release and architecture, ran, carried every file and was
removed, in 3 to 66 s, the aarch64 ones under qemu-user. All twenty-four jobs
passed again in `batch41`'s 2890818010 - where the four openSUSE builds passed
only when started again, nine attempts having failed to connect to
archives.boost.io for their Boost tarball (`build/ci/packages/fetch-boost.sh`
has since kept it in the job's cache, retried, and gone on to SourceForge's
copy) - and 2891564033, in 6.3.5's tag
pipeline 2892097590 (29 September), and in `batch42`'s two pipelines of
30 September 2026, which built 6.3.6:

| Job | 2896082335 (`c26ce1e59`): build / install | 2897966924 (`cec2b151d`): build / install |
|---|---|---|
| `rpm-fedora44-x86_64` | 598 s / 13 s | 741 s / 13 s |
| `rpm-fedora44-aarch64` | 676 s / 83 s | 830 s / 81 s |
| `rpm-fedora43-x86_64` | 746 s / 33 s | 766 s / 15 s |
| `rpm-fedora43-aarch64` | 916 s / 72 s | 1027 s / 74 s |
| `rpm-el10-x86_64` | 875 s / 2 s | 956 s / 3 s |
| `rpm-el10-aarch64` | 760 s / 18 s | 895 s / 15 s |
| `rpm-el9-x86_64` | 660 s / 4 s | 788 s / 3 s |
| `rpm-el9-aarch64` | 694 s / 20 s | 815 s / 19 s |
| `rpm-leap16-x86_64` | 902 s / 16 s | 1085 s / 18 s |
| `rpm-leap16-aarch64` | 960 s / 74 s | 1121 s / 77 s |
| `rpm-tumbleweed-x86_64` | 955 s / 14 s | 974 s / 14 s |
| `rpm-tumbleweed-aarch64` | 1426 s / 312 s | 1147 s / 337 s |

Each time is the job's own line: `BUILT:` for the build, `PACKAGE PASSED:` for
the install. In both, every build checked its packages' licence files and every
install job the installed ones (`LICENCES PASSED`, [The licences](#the-licences)),
and `rpm-fedora44-aarch64-sbom` made the generic aarch64 package's SBOM, 8
components, from what the build recorded it was built against.

### By hand on linix

Every build below is of 6.3.4 build 43, made on linix by
`bash build/ci/packages/rpm-local.sh <target> <arch>` with six compilers
(`HM_BUILD_JOBS=6`, the jobs' value). Each passed its install test in a clean
container of its own release and architecture. "Build" is the build job's whole
time: the packages, the sysroot, Boost where built, CMake, ninja, the testers
and CPack. "Install" is the install job's time. In every build `hmailserver
--version` answered and the four tester programs passed
(`object-store-signature-tests`, `script-binding-tests`, `snmp-tests`,
`spf-tests`), on aarch64 under qemu-user, and every packaged
`/usr/bin/hmailserver` was a PIE with full RELRO.

| Release | Arch | Route | Compiler | Boost | OpenSSL | Build | Install |
|---|---|---|---|---|---|---|---|
| Fedora 44 | x86_64 | native | clang 22.1.8 | 1.90.0 (Fedora's) | 3.5.8 | 506 s | 8 s |
| Fedora 43 | x86_64 | native | clang 21.1.8 | 1.83.0 (Fedora's) | 3.5.8 | 514 s | 8 s |
| RHEL 10 (UBI 10.2) | x86_64 | native | clang 21.1.8 | 1.92.0 (boost.org, built in the job) | 3.5.8 | 561 s | 2 s |
| Leap 16.0 | aarch64 | cross | clang 19.1.7 | 1.92.0 (boost.org, built in the job) | 3.5.0 | 750 s | 58 s (qemu) |
| Tumbleweed | x86_64 | native | clang 23.1.1 | 1.92.0 (boost.org, built in the job) | 3.5.3 | 690 s | 12 s |
| Fedora 44 | aarch64 | cross | clang 22.1.8 | 1.90.0 (Fedora's) | 3.5.8 | 555 s | 62 s (qemu) |

The first five rows ran with the scripts as they were before four last changes:
the `Suggests` written into the spec where CPack leaves them out (the RHEL 10
and Leap 16.0 packages above carry none), the retry of a package manager's step
that could not reach its repository, zypper run without
`--gpg-auto-import-keys`, and the install test's service-account check
failing the test rather than printing a note (see
[The install test](#the-install-test)). The Fedora 44 aarch64 row ran with the
scripts as they are: its sysroot of Fedora 44's aarch64 packages took 23
seconds to install (1.3 GB, GCC 16), and it also made the generic
`hmailserver-6.3.4-1.aarch64.rpm`, which its install test installed, ran and
removed as well.

**The other six pairs did not run by hand with the current scripts**; they ran
in GitLab's pipelines instead, from 2887115157 and 2887524317 on (above). They
were queued on linix when this was written, first behind other work and then
behind the 6.3.4 release, which came first there: RHEL 9 x86_64 and aarch64,
Tumbleweed aarch64, RHEL 10 aarch64, Leap 16.0 x86_64 and Fedora 43 aarch64,
and after them RHEL 10 x86_64, Leap 16.0 aarch64 and the three x86_64 rows
above again. Earlier versions of the scripts had built and installed RHEL 9
x86_64 and aarch64 and Leap 16.0 x86_64; RHEL 10 aarch64, Fedora 43 aarch64 and
Tumbleweed aarch64 had not been built at all before the pipelines built them.
One RHEL 9 x86_64 run, with the first version of the `Suggests` change, built
and passed its testers and then failed to package; the `Suggests` paragraph
below says why and what changed.

**What the earlier trials found**, each fixed in the scripts above:

- Fedora's and RHEL's clang linked a non-PIE server (see
  [The packages the build passes to CPack](#the-packages-the-build-passes-to-cpack));
  `-pie` alone then failed CMake's own test programs, and CMake found no zlib.
- The first Fedora 44 aarch64 sysroots failed to install, twice ("Rpm
  transaction failed"), until the merged-`/usr` links were made first.
- The first aarch64 link failed on `Normaliz.lib` (see
  [The AArch64 route](#the-aarch64-route-and-why)).
- The first aarch64 build did not run the testers: it looked for the binfmt
  handler in `/proc/sys/fs/binfmt_misc`, which a container does not mount. It
  now asks the kernel by running the server.

Why each release has its own package
------------------------------------

An `.rpm`'s automatic `Requires` are the sonames and symbol versions the binary
was linked against, and they are only right for the release that built it. For
example:

- the Fedora and RHEL builds require `libpq.so.5(RHPG_9.6)(64bit)`, a symbol
  version only Red Hat's PostgreSQL packaging defines; the openSUSE builds,
  linked against openSUSE's `libpq`, require `libpq.so.5()(64bit)` alone;
- every build requires the `GLIBC_2.x` and `GLIBCXX_3.4.x` versions of its own
  release's glibc and libstdc++, which an older release does not have.

The generic `.rpm` that `linux-build` makes is built on Ubuntu 26.04, and
`linux-package-rpm` installs it on Fedora 44 with `--nodeps`
(`build/ci/linux-package-install.sh` says why). It cannot show that the package
installs with its dependencies anywhere. Each package here is installed by
`dnf` or `zypper`, which must resolve every `Requires` from that release's own
repositories.

Boost is not in `Requires` at all, because the server links it statically
(`hmailserver/source/Server/CMakeLists.txt`, "Boost is linked STATICALLY"). A
release's Boost matters only when the package is built:

- **Fedora** uses its own `boost-static`: 1.90.0 on Fedora 44, 1.83.0 on
  Fedora 43.
- **RHEL 10 and 9, and openSUSE**, build Boost 1.92.0 in the job, from
  boost.org's release tarball (`archives.boost.io`), checked against the SHA-256
  that `libraries/build-boost.ps1` pins for Windows.
  `build/ci/packages/fetch-boost.sh` takes it from the job's cache
  (`.cache/boost-tarball`, keyed by that SHA-256) when it is there, and
  otherwise from archives.boost.io or SourceForge's copy, with six attempts
  each and only with that SHA-256. The reasons:
  - RHEL 9's own Boost is 1.75, older than the 1.83 the build needs;
  - neither UBI 10 nor UBI 9 carries `boost-devel` or `boost-static`;
  - openSUSE ships no static Boost archives.

The jobs
--------

Each release has four jobs: a build and an install test, for each
architecture. That makes 24 jobs, and a 25th, `rpm-fedora44-aarch64-sbom`, for
the one package a release carries.

| Job | Runs on | Image | What it does |
|---|---|---|---|
| `rpm-<target>-x86_64` | linix | the release's x86_64 image | `build/ci/packages/rpm-build.sh`: the release's clang, lld, CMake, ninja, rpm-build and `-devel` packages from its own repositories; CMake and ninja, then `hmailserver --version` and the server's tester programs, then `build/check-linux-version-stamp.sh` and CPack's RPM generator |
| `rpm-<target>-aarch64` | linix | the release's **x86_64** image | the same script, cross-compiling with the release's clang and lld against a sysroot of the same release's aarch64 packages (see [The AArch64 route](#the-aarch64-route-and-why)). The testers run under qemu-user |
| `rpm-<target>-<arch>-install` | linix | the release's image **of that architecture** | `build/ci/packages/rpm-install-test.sh`, on a clean container (see [The install test](#the-install-test)). The aarch64 container runs under qemu-user |
| `rpm-fedora44-aarch64-sbom` | linix | the CI image | `build/ci/packages/package-sbom.sh`: the generic aarch64 package's own SBOM, `hmailserver-<v>-1.aarch64.rpm.cdx.json`, as `linux-build` makes the x86_64 one's, from what the build recorded it was built against |

`<target>` is `fedora44`, `fedora43`, `el10`, `el9`, `leap16` or `tumbleweed`.

| Target | Image | Package manager | clang | CMake | rpm | OpenSSL | PostgreSQL's libpq |
|---|---|---|---|---|---|---|---|
| `fedora44` | `registry.fedoraproject.org/fedora:44` | dnf 5 | 22.1.8 | 4.3.0 | 6.0.2 | 3.5.8 | 18.0 |
| `fedora43` | `registry.fedoraproject.org/fedora:43` | dnf 5 | 21.1.8 | 3.31.11 | 6.0.2 | 3.5.8 | 18.0 |
| `el10` | `registry.access.redhat.com/ubi10/ubi` (Red Hat's Universal Base Image 10, RHEL 10.2) | dnf 4 | 21.1.8 | 3.31.8 | 4.19.1.1 | 3.5.8 | 16.14 |
| `el9` | `registry.access.redhat.com/ubi9/ubi` (UBI 9, RHEL 9.8) | dnf 4 | 21.1.8 | 3.31.8 | 4.16.1.3 | 3.5.8 | 13.23 |
| `leap16` | `registry.opensuse.org/opensuse/leap:16.0` | zypper | 19.1.7 | 3.31.7 | 4.20.1 | 3.5.0 | 18.6 |
| `tumbleweed` | `registry.opensuse.org/opensuse/tumbleweed`, its 6 October 2026 snapshot mirrored in the project's registry | zypper | 23.1.2 | 4.4.3 | 4.20.1 | 3.5.3 | 18.6 |

The versions are what each release's repositories gave the builds of 26
September 2026, and for Tumbleweed, a rolling release, those of its 6 October
2026 snapshot (the build logs; UBI's also from `dnf repoquery` in the pinned
images).

**Images.** Every image is the distribution's own. Each is pinned by the digest
of its architecture's own manifest, not by the multi-architecture index. The
x86_64 and aarch64 images are then two separate references in the runner's
image store, so `pull_policy: if-not-present` can never give an aarch64 job the
x86_64 image held under the same name. The CI file lists each tag's index
digest beside them, as read on 26 September 2026. All but Tumbleweed's come
from the distribution's own registry. openSUSE's registry drops a Tumbleweed
snapshot within weeks (the pin of 26 September was gone by 6 October), so its
two images come from the project's own registry,
`registry.gitlab.com/hmailserver/hmailserver/mirror/opensuse-tumbleweed`, a
copy whose digests are openSUSE's own; `build/ci/mirror-tumbleweed.sh` takes
a new snapshot, and the two pins are then changed by hand.

**When the jobs run** (as `.gitlab/ci/packages-deb.yml`'s do):

- for every `v*` tag and every `batch*` branch, automatically;
- on any other protected branch (`master`), only when started by hand. Press a
  build job's play button, and its install job follows: the install jobs are
  never manual themselves (this is how the rules are written; no pipeline has
  shown it yet);
- never for a merge request, an unprotected ref, or a scheduled, fuzz or feed
  pipeline. That is the same refusal every job on the project's own runners
  begins with: a tag's and a batch's builds, and every install job, are made by
  `.own-runners`' own rules in `.gitlab-ci.yml` (taken by `!reference`), so
  they follow any change to them. Only the by-hand rule for `master`, and the
  refusal before it, are the CI file's own, and that rule asks for a protected
  ref itself, so a check that reads one rule at a time, as
  `build/ci/repo-hygiene.py`'s Token-Permissions check does, sees that no
  unprotected ref can start one.

**Serialisation.** The build jobs name `resource_group: linix-package-build`,
the group the Debian and Ubuntu builds use, so only one package build of the
two files runs at a time, across pipelines and across the two runners with the
tag `linix`, linix and server2. Each build runs six
compilers at once (`HM_BUILD_JOBS`); linix had 16 cores and 11 GB of memory on
26 September 2026. On a `v*` tag or a `batch*` branch the twelve RPM builds
queue behind each other and behind the Debian and Ubuntu ones: at the times
measured above (about 500 to 700 seconds for an x86_64 build, 750 for an
aarch64 one), the twelve take about two and a half hours of linix.

**Artifacts.** Each build job keeps `packages-rpm/<target>-<arch>/` for 14
days:

- the package;
- for `fedora44-aarch64`, `generic/hmailserver-<v>-1.aarch64.rpm` too;
- `SHA256SUMS.txt`;
- `requires.txt`: the package's `Requires`, `Requires(post)` and `Suggests`;
- `build-info.txt`: the release, the compiler, Boost, the commit and the time;
- `built-against/`: `rpm.txt`, one line for each thing each package of the
  build's rpm database provides (the aarch64 sysroot's in a cross build; on
  openSUSE, whose aarch64 sysroot is unpacked rather than installed, each
  package as it was unpacked), and `boost-version.hpp`, the Boost header the
  build compiled against, for the package's SBOM;
- the logs of the packages, the sysroot, Boost, the configure, the build, the
  packaging and each tester.

`rpm-fedora44-aarch64-sbom` keeps `generic/hmailserver-<v>-1.aarch64.rpm.cdx.json`
beside the generic package: its SBOM, which the release carries. It runs
`build/ci/packages/package-sbom.sh` in the CI image, because the build's
container has neither PowerShell nor Python: `build/native-dependencies.ps1`'s
component list, then `build/asset-sbom.py make --rpm-list
built-against/rpm.txt`, which names for each library the package that
provides its `-devel` in the sysroot (Fedora's `zlib-devel` is
`zlib-ng-compat-devel`'s). Every build records it; only the package a release
carries gets an SBOM job.

The names
---------

| Target | File | Release |
|---|---|---|
| Fedora 44 | `hmailserver-<v>-1.fc44.<arch>.rpm` | `1.fc44` |
| Fedora 43 | `hmailserver-<v>-1.fc43.<arch>.rpm` | `1.fc43` |
| RHEL 10 | `hmailserver-<v>-1.el10.<arch>.rpm` | `1.el10` |
| RHEL 9 | `hmailserver-<v>-1.el9.<arch>.rpm` | `1.el9` |
| Leap 16.0 | `hmailserver-<v>-1.suse.lp160.<arch>.rpm` | `1.suse.lp160` |
| Tumbleweed | `hmailserver-<v>-1.suse.tw.<arch>.rpm` | `1.suse.tw` |

**How the suffix is made.** The name is CPack's own, for
`CPACK_RPM_PACKAGE_RELEASE=1.<disttag>`, which the build passes to CMake;
nothing is renamed afterwards, so the file name and the header's `Release` are
always the same value. `rpm-build.sh` fails when CPack makes no file of that
name, and when the container is not the release the file would be named after.

The generic names
-----------------

The update checker (`UpdateChecker.cpp`, `PlatformInstallerName_`) and
`installer_names()` in `build/generate-update-feed.py` look for
`hmailserver-<v>-1.x86_64.rpm` and `hmailserver-<v>-1.aarch64.rpm`.

- **`hmailserver-<v>-1.x86_64.rpm` stays `linux-build`'s**, the clang build in
  the CI image (Ubuntu 26.04): the release collects it, and `linux-package-rpm`
  installs it on Fedora 44. No job here makes a second file of that name, so a
  release has one file of that name to take.
- **`hmailserver-<v>-1.aarch64.rpm` comes from `rpm-fedora44-aarch64`**, the
  only job that makes it: the same build as `hmailserver-<v>-1.fc44.aarch64.rpm`,
  with CPack run a second time for release `1`. Fedora 44 is the release the
  generic x86_64 package is tested on. `rpm-fedora44-aarch64-install` installs
  both files. From 6.3.6 the release collects it with its SBOM, holding it to
  the `SHA256SUMS.txt` the build wrote and adding its line to the release's
  sums, and `release` waits for the build, its install job and
  `rpm-fedora44-aarch64-sbom`. It goes in no dnf repository: the `.fc44` file
  is the same build, and that is the one `fc44`'s repository takes once a
  release carries it (`build/generate-package-repos.py`).

**The update checker was not changed in this work.** A server installed from any
`.rpm` looks for the generic name of its architecture, so a server installed
from, say, `hmailserver-<v>-1.el9.x86_64.rpm` is offered `linux-build`'s
Ubuntu-built package on update. `hmailserver-update` installs an `.rpm` with
`dnf install` where there is a `dnf` command, and `rpm -Uvh` otherwise
(openSUSE has none, unless one has been installed beside zypper), and both
check `Requires`: where the offered package's `Requires` do not resolve, the
update is refused and the installed server stays as it was.
Choosing a package by distribution is not done; the row *Per-distribution
`.deb` and `.rpm` packages* of [Roadmap3.md](../../Roadmap3.md) carries it.

The live update
---------------

A server installed from the x86-64 `.rpm` updates itself the same way as the
`.deb` ([PackagesDeb.md](PackagesDeb.md), "The live update"), with `dnf
install` where there is dnf and `rpm -Uvh` otherwise; `%post` upgrades the
schema. It was tried from 6.3.4 to 6.3.5 on Fedora 44 on 29 September 2026,
through `POST /api/v1/update/check`, `.../download` and `.../install`: the
server detected its rpm installation from inside its hardened unit, found
`hmailserver-6.3.5-1.x86_64.rpm`, verified it, and the helper installed it
with `dnf` and wrote `ok 6.3.5` (schema 6104 to 6105, about six seconds
without listeners). The unattended route, the backup destination and the
trust settings are as for the `.deb`. Not tried: a machine with SELinux
enforcing, since the test ran in a container, and `rpm -Uvh` on a machine
without dnf.

From 6.3.6 the package requires `ca-certificates` beside the automatic
`Requires`: every HTTPS request the server makes, the update check among them,
is checked against the distribution's bundle of certificate authorities
([PackagesDeb.md](PackagesDeb.md), "Why each release has its own package").
It asks for glibc's character-set converters too ([The character
sets](#the-character-sets)): it requires them where dnf is installed on Fedora
or EL, where dnf installs them with the package, and only recommends them
elsewhere. On openSUSE the update is `rpm -Uvh`, which installs nothing beside
the package and refuses one whose requirements are not met - or openSUSE's own
dnf, where one has been installed, which has no repositories to find them in -
so a plain requirement would keep every openSUSE server without the converters
on 6.3.5: its helper's update to 6.3.6 is refused (measured, below). As it is,
the server is updated, and names what it cannot convert and the package that
has it at every start. And
its `%changelog` is one entry of its own, dated by `SOURCE_DATE_EPOCH` so that
two builds of one commit make the same header: for a released version, the
version, its date and its page from its heading in `CHANGELOG.md`; for a build
made before its version was released, a line saying so. Up to 6.3.5 `rpm -q
--changelog hmailserver` showed CPack's placeholder, "Sun Jul 04 2010 Eric
Noulard ... Generated by CPack RPM".

The character sets
------------------

The server converts mail's and IMAP's text with the C library's iconv: the
code page table's 24 character sets (`Common/Util/Unicode.cpp`) and the PST
import's (`Common/Pst/PstCodePages.cpp`). Debian's and Ubuntu's `libc6`
carries every converter. Fedora's and EL's glibc keeps all but a few in
`glibc-gconv-extra`, openSUSE's in `glibc-gconv-modules-extra`, and glibc
only recommends that package. dnf and zypper install recommended packages by
default, but a container image does not, nor an installation made without
them. In the pinned images of 1 October 2026 - `fedora:44`, `ubi9/ubi`,
`ubi10/ubi`, `leap:16.0` and `tumbleweed` - `/usr/lib64/gconv` held CP1252,
ISO8859-1, ISO8859-15, UNICODE, UTF-16, UTF-32 and UTF-7 (and openSUSE's
ANSI_X3.110), glibc having US-ASCII and UTF-8 built in. There every `.rpm` up
to 6.3.5 read and wrote no text in BIG5, ISO-2022-JP, the other Windows and
ISO-8859 code pages, or the PST import's KOI8 and code pages 932, 936 and 949:
a header or a body in them came out empty.

From 6.3.6 every `.rpm` here and `linux-build`'s require

    ((glibc-gconv-extra(x86-64) or glibc-gconv-modules-extra(x86-64)) if (dnf and system-release))

and recommend

    (glibc-gconv-extra(x86-64) or glibc-gconv-modules-extra(x86-64))

(`aarch-64` for AArch64), from `CPACK_RPM_PACKAGE_REQUIRES` and
`CPACK_RPM_PACKAGE_RECOMMENDS` in `CMakeLists.txt`. One rich dependency, which
each distribution meets with its own package: the generic `.rpm` is installed
on all of them and offered to all of them by the live update. Required where
dnf can install them, and only recommended elsewhere, because of how each is
installed. The live update installs an `.rpm` with `dnf install` where there
is a `dnf` command and with `rpm -Uvh` where there is not - openSUSE, and a
UBI minimal image, which has microdnf - and `rpm -Uvh` refuses a package whose
requirements are not met: a plain requirement had 6.3.5's helper refuse the
update to 6.3.6 on every openSUSE server without the converters. A plain
recommendation does not do for Fedora and EL instead: glibc recommends the
same package, and dnf does not install a recommendation that an installed
package's recommendation already left out (`exclude_from_weak_autodetect`, on
by default), so `dnf install` of the package never brought them where they
were missing. The condition is dnf and `system-release`, which the release
package of Fedora and of every EL provides (`fedora-release`,
`redhat-release`, `almalinux-release` and the rest) and openSUSE's does not:
there dnf installs them with the package as a requirement, and `rpm -Uvh`
refuses the package as before, where the live update does not use it. dnf
then needs a repository that has them: a Fedora or EL server without them
whose dnf has no repository enabled - an unregistered RHEL, say - has the
update refused, and stays as it was, until `glibc-gconv-extra` is installed
(measured in Fedora 44 and UBI 10 with their repositories taken away, 3
October 2026), the same refusal openSUSE's dnf made below.
Everywhere else the requirement is void: on openSUSE `rpm -Uvh` installs the
package, and zypper installs the recommendation unless `solver.onlyRequires`
is on (it is off by default, and on in the container images). `if dnf` alone
was not enough: openSUSE packages dnf (Leap 16.0's dnf 4.23, Tumbleweed's
dnf5), and where it has been installed beside zypper the live update installs
with it - and that dnf, as openSUSE installs it, has no repositories, so it
refused the package for the converters it could not find, as it refused the
plain requirement. rpm takes rich dependencies from 4.13, and adds
`rpmlib(RichDependencies)`; UBI 9's 4.16 is the oldest here. It names the
architecture's own build (`%{_isa}`), since on x86-64 the i686 build of the
package would meet the bare name and convert nothing for this server. The RPM
spec requires its distribution's package by name, the same way, and on every
distribution ([SourcePackages.md](SourcePackages.md)): its packages have no
live update, and dnf or zypper installs them.

The server says what it cannot convert: `hmailserver --check-config` ends
with `Character sets:`, which says every one converts or names those that do
not and the package that has them, and a start with any missing writes the
same to standard error and the application log. The install test reads that
line ([The install test](#the-install-test)).

What ran, on linix on 2 and 3 October 2026, for the condition:

- **A miniature of the package** at version 1 with 6.3.5's lines
  (`ca-certificates` alone) and at version 2 with this requirement and the
  recommendation, made with Fedora 44's rpmbuild; version 1 installed without
  the converters, then updated to 2 as `hmailserver-update` does it (`dnf
  install -y` where there is a `dnf` command, `rpm -Uvh` otherwise). The update
  went through in all eleven: Fedora 44 and 43, UBI 10 and 9 (dnf bringing
  `glibc-gconv-extra`), UBI 10 and 9 minimal (microdnf, so `rpm -Uvh`, without
  them), Leap 16.0 and Tumbleweed (`rpm -Uvh`, without them), Leap 16.0 and
  Tumbleweed with dnf installed beside zypper (`dnf install -y`, without
  them), and Fedora 44 with zypper installed beside dnf (dnf bringing them).
  With `if dnf` alone, both openSUSE images with dnf refused the update ("none
  of the providers can be installed"); with the plain requirement of 1 October
  every image but those with dnf on Fedora and EL refused it.
- **The release's own `.rpm`**, `linux-build`'s, through `rpm-install-test.sh`
  in the pinned `fedora:44`, `ubi10/ubi`, `leap:16.0` and `tumbleweed` images
  (The install test, step 3).

And on 1 October 2026, in the pinned images, with the requirement then
unconditional:

- **A miniature of the package** - CPack with these two `Requires` lines, and
  with `ca-certificates` alone, built by each release's own CMake and rpmbuild
  (Fedora 44's CMake 4.3 and rpm 6.0.2, UBI 9's 3.31.8 and 4.16.1.3, UBI 10's
  3.31.8 and 4.19.1.1, Leap 16.0's 3.31.7 and 4.20.1, Tumbleweed's 4.4.3 and
  4.20.1), then installed in a fresh container of the same image with weak
  dependencies off. On all five, CPack wrote the rich dependency as it is
  (`rpm -qp --requires` prints it, with `rpmlib(RichDependencies)`), the
  release's own package came with it, and a BIG5 and an ISO-2022-JP-2 sample
  converted; with `ca-certificates` alone, nothing came and neither converted.
- **The generic x86-64 `.rpm`**, `linux-build`'s, from this change, whose
  newest glibc symbol is `GLIBC_2.38`, so that it installs on every release
  here but EL 9: `rpm-install-test.sh <release> x86_64` passed on it in
  `fedora:44`, `fedora:43`, `ubi10/ubi`, `leap:16.0` and `tumbleweed`. Each
  release's package manager brought its own converters (`glibc-gconv-extra`
  2.43-9.fc44, 2.42-16.fc43 and 2.39-128.el10_2, `glibc-gconv-modules-extra`
  2.40-160000.6.1 and 2.44-2.1), the server named nothing it could not
  convert, and both samples converted; without the package the server named
  34 of its 41 character sets (all but US-ASCII, ASCII, UTF-8, UTF-7, CP1252,
  ISO-8859-1 and ISO-8859-15) and BIG5 did not convert. On Fedora 44 a start
  of the server without the package wrote the same 34 to standard error.
  `linux-package-install.sh rpm` passed and read the requirement. In the CI
  image (Ubuntu 26.04) the `.deb`'s server named none.
- **RHEL 9's own package**, which the generic one cannot stand in for (EL 9's
  glibc is 2.34): `rpm-build.sh el9 x86_64` (at four compilers, 2435 s) made
  `hmailserver-6.3.6-1.el9.x86_64.rpm` with the requirement, every tester
  passing, and `rpm-install-test.sh el9 x86_64` passed on it, dnf bringing
  `glibc-gconv-extra` 2.34-275.el9_8, with the same 34 named without it.
- **The spec**: `rpmspec -q --requires` gives `glibc-gconv-extra(x86-64)` on
  Fedora 44 and UBI 10 and `glibc-gconv-modules-extra(x86-64)` on Leap 16.0
  and Tumbleweed.

The AArch64 route, and why
--------------------------

The aarch64 packages are **cross-compiled** on linix, which is x86-64, in the
release's own x86_64 container. Three routes were weighed:

1. **A cross sysroot (chosen).** The release's own clang, lld and rpmbuild run
   natively and target `aarch64-<vendor>-linux`, with `--sysroot` set to a
   directory holding the same release's aarch64 packages: glibc, libstdc++,
   GCC's start files and every `-devel` package the build needs.
   - On Fedora and RHEL, `dnf --installroot --forcearch=aarch64` installs them
     from the image's own repositories, with `$basearch` set to aarch64 and the
     scriptlets off. The merged-`/usr` links (`/lib64 -> usr/lib64` and the
     rest), which the `filesystem` package's scriptlet would make, are copied
     from the container, which is the same release.
   - On openSUSE, libzypp is given `arch = aarch64` through `ZYPP_CONF`. The
     package closure is downloaded, with signatures checked against the keys the
     image already trusts, and unpacked. Leap 16.0 keeps aarch64 under
     `distribution/leap/16.0/repo/oss/aarch64`, and Tumbleweed under
     `ports/aarch64/tumbleweed`.
   - The result takes nothing from the x86_64 side except the compiler's code
     generation, and it builds at native speed plus the sysroot's setup.
   - rpmbuild on the x86_64 side reads the aarch64 binary's dynamic section, so
     the automatic `Requires` are the aarch64 sonames.
   - **The testers run too.** linix's kernel hands aarch64 ELF files to
     qemu-user (binfmt_misc, with the `F` flag, so it works in a container), and
     the build runs `hmailserver --version` and every `*-tests` program with
     `QEMU_LD_PREFIX` set to the sysroot, and with `HM_TEST_TIME_FACTOR=10`:
     emulated, a program runs several times slower than the machine, and the
     testers' timing checks, which hold a pass to a bound well under what a
     quadratic one takes, take ten times the bound (from 6.3.6; el9's
     `imap-parse-tests` took 2.7 s under qemu for a check of 2 s). Where a runner
     cannot run aarch64 programs, the build job says it did not run them and goes
     on; the install test then runs the packaged server.
2. **The whole build in an aarch64 container under qemu-user.** Every compiler
   process would be emulated, and a C++ build under qemu-user is commonly 5 to
   10 times slower: about 1 to 2 hours per package, for six packages per
   pipeline, on the runner the regression suite needs. It was not tried.
3. **GitLab's hosted arm64 runners.** They build natively on arm64 hardware,
   but every minute is a compute minute. The small one
   (`saas-linux-small-arm64`) has 2 vCPUs, so a build that takes linix's six
   compilers 10 to 15 minutes would take roughly three times as long there:
   about 30 to 45 minutes per package, three to four and a half hours for the
   six on every tag and `batch*` branch, before GitLab's cost factor for the
   runner size (not checked here). They are also not the project's own
   machines. It was not used.

**One source change the cross build needs, made in its linker flags.**
`Parsing/StringParser.cpp` carries an unguarded `#pragma comment(lib,
"Normaliz.lib")`. clang records it in the ELF `.deplibs` section, and lld obeys
that section while GNU ld ignores it, so the first aarch64 link failed with
"unable to find library from dependent library specifier: Normaliz.lib". The
cross toolchain passes `-Wl,--no-dependent-libraries`. The pragma itself is
Windows-only and belongs behind `_WIN32`; that is a change to the server's
source, outside this file.

RHEL: why UBI, and not AlmaLinux or Rocky
-----------------------------------------

Red Hat's Universal Base Image is RHEL's own packages, free to use and
redistribute. On 26 September 2026 its repositories (BaseOS, AppStream and
CodeReady Builder, all enabled in the image) carried everything the build needs
except Boost, as `dnf repoquery` in the pinned images showed:

| | UBI 9 (RHEL 9.8) | UBI 10 (RHEL 10.2) |
|---|---|---|
| clang, lld | 21.1.8 | 21.1.8 |
| cmake | 3.31.8 | 3.31.8 |
| ninja-build (CodeReady Builder) | 1.10.2 | 1.11.1 |
| rpm-build | 4.16.1.3 | 4.19.1.1 |
| openssl-devel | 3.5.8 | 3.5.8 |
| libpq-devel | 13.23 | 16.14 |
| openldap-devel | 2.6.8 | 2.6.10 |
| gcc-c++, libstdc++-devel | 11.5.0 | 14.3.1 |
| boost-devel, boost-static | not available | not available |

So RHEL 10 and 9 are built on RHEL itself rather than on a rebuild, and the
install test resolves `Requires` against RHEL's own repositories: it proves the
package installs where the UBI repositories are enabled, and a subscribed RHEL
machine has those packages and more. AlmaLinux 10's repositories were checked
too: they carry `boost-devel` and `boost-static` 1.83.0, the latter in CRB.
They are the fallback if UBI ever drops a package the build needs.

openSUSE
--------

- **Leap 16.0** names OpenLDAP's headers `openldap2_6-devel`; it has no
  `openldap2-devel`.
- **Tumbleweed** is rolling. The pinned digest fixes the base image only, and
  the packages come from the repository as it is on the day of the build.
- **Leap 15.6 is not built.** It reached its end of life on 30 April 2026
  (<https://news.opensuse.org/2026/04/01/leap-15-eol/>).
- **No key is auto-imported.** Both scripts run zypper without
  `--gpg-auto-import-keys`: the images already trust openSUSE's keys (a
  `zypper refresh` in each pinned image passed without it on 26 September
  2026), and a package built for a release must not come from a repository that
  presented a key the image did not already trust.

The packages the build passes to CPack
--------------------------------------

The packages differ from the generic one in five ways, each passed to CMake on
the command line, so `CMakeLists.txt` is unchanged:

- **The release is `1.<disttag>`**, which gives the names above.
- **`Requires(post)` names the account tools:** `shadow-utils` on Fedora and
  RHEL, `shadow` on openSUSE. `%post` runs `useradd` and `groupadd`, and a
  minimal installation without them would fail the scriptlet.
- **Fedora's and RHEL's shebang rewriting is switched off** (`%undefine
  __brp_mangle_shebangs`). Otherwise `/usr/bin/hmctl`'s `#!/usr/bin/env python3`
  would become `#!/usr/bin/python3`, and python3 a hard `Requires`;
  `CMakeLists.txt` makes it a suggestion on purpose.
- **Every package suggests what `CMakeLists.txt` suggests** (`p7zip python3`,
  for the backup and for `hmctl`). CPack leaves the `Suggests` tag out when its
  own test decides the release's rpmbuild cannot take it. The CMake 3.31 of UBI
  9, UBI 10 and Leap 16.0 decided so ("SUGGESTS not supported in provided
  rpmbuild"), although their rpm, 4.16 to 4.20, takes the tag; Fedora 43's
  CMake 3.31 with rpm 6, Fedora 44's CMake 4.3 and Tumbleweed's CMake 4.4 kept
  it. Where CPack leaves it out, `rpm-build.sh` runs CPack again on the same
  build with `-D CPACK_RPM_SPEC_MORE_DEFINE=...`, whose second line is the
  `Suggests` tag, and CPack writes it into the spec's preamble. It is given to
  cpack, not to CMake, and with a real newline: CMake 3.31 writes the variable
  into the spec as it is, so a CMake list's `;` made rpmbuild fail with
  "Unknown tag: ;Suggests" (UBI 9, the first run of this change), and a newline
  does not go through CMake's cache. The build fails any package whose
  `Suggests` are not `CMakeLists.txt`'s.
- **The executables are hardened** with the core of what the distributions
  build their own packages with: position-independent (`-fPIE`, linked
  `-pie`), full RELRO (`-z relro -z now`), `-fstack-protector-strong`,
  `-fstack-clash-protection` and `_FORTIFY_SOURCE=2`, for the server, the
  testers and the Boost built in the job. Fedora's own packages now take
  `_FORTIFY_SOURCE=3` (`redhat-rpm-config`'s `%_fortify_level`) and add more,
  such as `_GLIBCXX_ASSERTIONS`; neither was tried here, and the packages in
  [What has run](#what-has-run) were built at level 2. CMake is not run from a
  spec here, so nothing adds them unless the build does, and Fedora's and
  RHEL's clang link a non-PIE executable by default: the first trials of 26
  September made `LSB executable` binaries on Fedora 44 and UBI 9, and
  `LSB pie executable` only on Leap 16.0.
  `rpm-build.sh` fails when the packaged `/usr/bin/hmailserver` is not a PIE,
  has no `GNU_RELRO` segment or is not linked with `BIND_NOW`. `-fPIE` goes to
  every compile, as `redhat-rpm-config`'s hardening does: CMake's own test
  programs do not get `CMAKE_POSITION_INDEPENDENT_CODE`, and the first run with
  `-pie` alone failed CMake's compiler ABI check and then its search for zlib.

The licences
------------

From 6.3.6 every `.rpm` here, and `linux-build`'s, carries the licences it goes
out under, each marked `%license`, so that `rpm -qL hmailserver` lists them.
Before it, of the Linux packages only the Arch package carried hMailServer's own
licence.

- `/usr/share/licenses/hmailserver/copyright`: the Debian source package's
  copyright file (`platform/packaging/debian-src/debian/copyright`), which is
  plain text: who holds the copyright of what the server is built from and
  under which licence, with the AGPL-3.0-or-later in full and the terms of what
  is compiled into the binary - QuickJS (MIT), the MIME coder (BSD-1-Clause),
  CStdString (a permissive notice of its own), the Public Suffix List's rules
  (MPL-2.0) and the rest it names.
- `/usr/share/hmailserver/licenses/AGPL-3.0.txt`, hMailServer's own licence, the
  FSF's file byte for byte (the repository's `LICENSE` without its carriage
  returns), and `QuickJS.txt` beside it, as the `.deb` and the AppImage carry
  them.

The package's `License` names every licence of what it holds that an identifier
can name: `AGPL-3.0-or-later AND MIT AND BSD-1-Clause AND MPL-2.0 AND
LicenseRef-Fedora-UltraPermissive AND BSL-1.0`, the last for Boost, which the
server links statically; until 6.3.6 it named the AGPL alone. It is the source
package's spec's, plus Boost, and like the spec's it cannot name a notice
neither SPDX nor Fedora names yet: the copyright file declares those. Boost's
licence asks for its notice only with its source, and OpenSSL, libpq, OpenLDAP
and zlib are the distribution's shared libraries, whose packages carry their own
licence files: no other licence text is owed.

The install rules put the two texts in, for every install; CPack alone puts the
copyright file in (`platform/packaging/cpack-licenses.cmake`), so the source
package's spec, which installs with CMake, marks its own. The package owns
`/usr/share/licenses/hmailserver` and not `/usr/share/licenses`, which is the
`filesystem` package's. `build/ci/packages/check-licenses.sh` holds the three
files to the tree, by SHA-256, mode, owner and flag: `rpm-build.sh` and
`linux-build` fail a package without them, and the install test checks them as
installed.

**Run in `batch42`'s pipelines 2896082335 and 2897966924 (30 September 2026).**
In both, `check-licenses.sh` passed (`LICENCES PASSED`) over every package each
of the twelve builds made, the generic `hmailserver-6.3.6-1.aarch64.rpm`
included, over the files every install job installed, and over `linux-build`'s
generic `hmailserver-6.3.6-1.x86_64.rpm` and what `linux-package-rpm` installed
of it. Before that, on the laptop on 29 September 2026:
`linux-build`'s route (Ubuntu 26.04, clang, CPack) made the `.rpm` with the three
files marked; in `fedora:44`, `rpm-install-test.sh` (dnf resolving every
`Requires`) and `linux-package-install.sh rpm` passed with it, and the latter
failed at this step with the package made before the change. Fedora 44's rpmlint
2.8.0 reports the same 8 errors and 79 warnings before and after, none about a
licence, and accepts the new `License`. A miniature of the tree with the same
install rules and CPack settings made and installed the `.rpm` with UBI 9's
(rpm 4.16), Leap 16's and Fedora 44's own CMake, rpm and package manager. No
per-release build (`rpm-build.sh`) was run there.

The install test
----------------

`rpm-install-test.sh` runs in a clean container of the release and the
package's architecture:

1. `dnf install ./<the .rpm>` or `zypper install ./<the .rpm>` resolves every
   `Requires` from that release's repositories. Nothing is installed with
   `--nodeps`.
2. `hmailserver --version` answers `hMailServer <v> build <n>`, under qemu on
   aarch64, as root, and as the service account where the image has `runuser`.
   Fedora's images have only `util-linux-core`, without `runuser`, `setpriv` or
   `su`, so on Fedora it runs as root only, and the test says so. Where
   `runuser` is there, a failure fails the test. (Until the last change the
   test printed the same note for a failure as for a missing `runuser`; in the
   five runs above, the RHEL 10, Leap 16.0 and Tumbleweed containers showed the
   service account running it.)
3. The character sets ([The character sets](#the-character-sets)). Before the
   install the package must require the converters where dnf is on Fedora or
   EL and recommend them, and the image must lack them, or the test says it
   cannot show they came with the package. dnf installs it without weak
   dependencies, zypper with them, as a system does (the images set
   `solver.onlyRequires`). After it, `glibc-gconv-extra` or
   `glibc-gconv-modules-extra` of the package's architecture is installed,
   `hmailserver --check-config` says every character set the server converts
   text in converts, and a BIG5 and an ISO-2022-JP-2 sample convert to their
   UTF-8 through the C library's iconv. Then the converters are taken away
   with `rpm -e --nodeps`, and the server must name BIG5 and ISO-2022-JP-2
   among those it cannot convert, and neither sample may convert: the check
   seeing what it checks. Then the live update's own install, without them:
   the package removed and installed again with `rpm -Uvh` must be installed
   on openSUSE, where the live update installs that way, the server naming
   what it cannot convert, and then, with dnf installed beside zypper, by `dnf
   install -y`, which the live update uses wherever there is a `dnf` command;
   and must be refused for the converters on Fedora and EL, where dnf then
   installs it with them.
4. Every file an installation needs is present: the binary, `hmctl`, the data
   files, the database scripts, the Control Deck (checked byte for byte against
   the repository's), the three systemd units, the update helper, the auto-ban
   hook, the configuration and the logrotate rule.
5. The licences are installed as the tree has them, under the image's
   `tsflags=nodocs` where it sets one, and `rpm -qL` names each: the copyright
   file, `AGPL-3.0.txt` and `QuickJS.txt` (`check-licenses.sh --installed rpm`;
   [The licences](#the-licences)).
6. `%post` made the `hmailserver` account and its directories, and left
   `/etc/hmailserver/hMailServer.ini` as `root:hmailserver 640` and a `%config`
   file.
7. `rpm -e` takes the package off again.
8. For Fedora 44 aarch64, the generic `hmailserver-<v>-1.aarch64.rpm` is then
   installed the same way, which brings the converters back, answers
   `--version` with release `1`, passes step 3's first half, and is removed.

**What the test does not do.** It does not start the server against a database,
and it has no systemd to start the unit. `build/ci/linux-package-install.sh`
does the first for `linux-build`'s packages in the `linux-package-*` jobs, and
`package-systemd-fedora44` does both for Fedora 44's x86_64 package
([Started under systemd](#started-under-systemd)).

**When the repository cannot be reached.** Both scripts try a package
manager's step up to three times when its output shows it could not reach the
repository (a curl or download error, a name that did not resolve), 15 seconds
apart, and fail at once on anything else: a `Requires` the release cannot meet
is what the test is for. The Debian jobs met such failures on linix, most often
under qemu, where apt's fall-back from an unreachable IPv6 address often gave up
(`docs/PackagesDeb.md`), and force IPv4. dnf and zypper fetch through libcurl,
which tries IPv6 and IPv4 side by side, and none of the RPM runs met a failure
to connect, so they do not force IPv4.

Started under systemd
---------------------

`package-systemd-fedora44` (`.gitlab/ci/package-systemd.yml`) installs
`rpm-fedora44-x86_64`'s `hmailserver-<v>-1.fc44.x86_64.rpm` with `dnf` in Fedora 44
with systemd as PID 1. It starts the unit, reads the sandbox from the running
server, asks SMTP, POP3, IMAP and the REST API whether they answer, then reloads,
stops, starts and removes it. `package-systemd-ubuntu26.04` does the same for
the `.deb`, and [PackagesDeb.md](PackagesDeb.md), "Started under systemd", says
what each step holds the package to and what has run. What the Fedora job adds:

- `%post`'s and `%preun`'s systemd branches run with systemd up. The install
  must leave `hmailserver.service` enabled and not started, and
  `hmailserver-update.path` enabled and started. `dnf remove` with the server
  running must stop it and disable both units.
- Fedora's image has no `runuser` ([The install test](#the-install-test)), so the
  server's verbs run as `hmailserver` through `podman exec --user`. Its systemd
  needs `dbus-broker`, or `systemd-logind` fails and the system is "degraded".
  Both the image's systemd and D-Bus are installed with weak dependencies off.
- It runs on Fedora 44's systemd (259 on 2 October 2026), with Fedora's own drop-in
  for every service (`10-timeout-abort.conf`) in force.

Running a job by hand
---------------------

On any x86-64 machine with docker, from a copy of a checkout:

    bash build/ci/packages/rpm-local.sh el10 aarch64

It runs the build, then the install test, in the images the CI file names (read
from `.gitlab/ci/packages-rpm.yml`), and prints each one's time. For aarch64 it
first refuses unless the host has the `qemu-aarch64` binfmt handler.

What is left
------------

1. **The binfmt handler on linix: done by the owner.** The aarch64 install jobs,
   and the testers in the aarch64 builds, need the `qemu-aarch64` handler, with
   the `F` flag, in linix's kernel. Since 6.3.6 an aarch64 build refuses at once,
   by name, when this machine cannot run an aarch64 program
   (`build/ci/arm64-emulation.sh`; PackagesDeb.md, "What is left", 1), where it
   used to build without running its testers and leave its install job to fail
   with "exec format error". The first trials registered it with
   `docker run --privileged --rm tonistiigi/binfmt --install arm64`, which did
   not survive linix's reboot at 04:37 UTC on 26 September. The owner then
   installed Ubuntu's `qemu-user-binfmt` (10.2.1) on linix, whose
   `/usr/lib/binfmt.d/qemu-aarch64.conf` `systemd-binfmt` loads at every boot
   (flags `POF`). The runs in [What has run](#what-has-run) used that handler.
   It came back after linix's next reboot, on 28 September 2026 at 18:08 UTC,
   when `systemd-binfmt` registered it at boot: the aarch64 install jobs of
   `batch41`'s 2890818010 and 2891564033 and of 6.3.5's tag pipeline 2892097590
   ran under that registration, and all passed.
2. **Wiring into the release: done for the generic aarch64 package, for
   6.3.6.** A release can add the thirteen files these jobs make:
   `hmailserver-<v>-1.aarch64.rpm` and the twelve whose names carry a release.
   `hmailserver-<v>-1.x86_64.rpm` stays `linux-build`'s.
   `hmailserver-<v>-1.aarch64.rpm` is in (batch42, 28 September 2026):
   `release` needs `rpm-fedora44-aarch64`, `rpm-fedora44-aarch64-sbom` and
   `rpm-fedora44-aarch64-install`, and `build/ci/release-chain.py collect`
   takes the package and its SBOM. The twelve are in too (batch42, W2.1):
   `release-packages` (`.gitlab/ci/release-packages.yml`) needs each build with
   its artifacts and its install job, gathers each package held to its build's
   sums and makes its SBOM, and `release` needs `release-packages`.
3. **The checks that read `.gitlab-ci.yml` alone: the integrator's change.**
   `build/check-release-chain.py` reads the jobs of an included file for the
   packages a release carries - the generic AArch64 one (`CARRIED_PACKAGES`:
   the build, the SBOM job and the install job) and, through `release-packages`,
   every build and install job here and every job those wait for - and for the
   old routes. `build/ci/repo-hygiene.py`'s Pinned-Dependencies check read the
   images of `.gitlab-ci.yml` only, and the scripts of `build/*.sh` and
   `build/ci/*.sh` only, not `.gitlab/ci/*.yml` or `build/ci/packages/`; since
   6.3.6 it reads the images and installs of every file `.gitlab-ci.yml`
   includes and of every shell script under `build/`, and finds every image
   (91 in 23 files) pinned by digest and no piped download. Its
   Token-Permissions check read `.gitlab-ci.yml` alone too; it reads the
   included files now and judges each rule that makes a job on its own, and run
   over this file it found all 24 jobs it held then asking for a protected ref
   and refusing a merge request.
4. **`docs/ContinuousIntegration.md`: the integrator's change.** Its table of
   jobs does not list these 25, its `master` row says every job but the release
   jobs, `linux-fuzz` and `installer-smoke` runs there, where these builds wait
   for a press, and it says that linix builds x86-64 only, where these jobs
   cross-compile aarch64 packages there (the generic one carried by a release
   from 6.3.6, item 2).
   The Debian and Ubuntu package jobs need the same lines, so the two are best
   written into it together.
5. **Keep the digests current.** Renovate (`renovate.json`) reads
   `.gitlab-ci.yml`'s images; whether it follows this included file, and keeps
   a per-architecture digest per-architecture when it moves one, has not been
   checked. Tumbleweed's digest in particular goes stale quickly.
6. **The first pipeline that runs the install jobs: done.** The install jobs
   first ran in pipeline 2887524317 on 28 September 2026, and every build and
   install job has passed in each pipeline since that ran them all to an end,
   6.3.6's two included (see [In GitLab's pipelines](#in-gitlabs-pipelines)).
