The Debian and Ubuntu packages
==============================

GitLab CI builds a `.deb` of the server for each supported Debian and Ubuntu
release, for x86-64 (`amd64`) and AArch64 (`arm64`). Each is built in its own
release's container, with that release's compiler and libraries, and each is
then installed in a clean container of the same release and architecture. The
jobs are in `.gitlab/ci/packages-deb.yml`, which `.gitlab-ci.yml` includes. The
scripts they run are in `build/ci/packages/`.

The one exception is Ubuntu 26.04 on x86-64. Its package,
`hmailserver_<v>_amd64.deb`, is `linux-build`'s, as before: the release
collects it, and `linux-package-deb` installs and starts it. This file adds
its arm64 twin and the other releases (see [The names](#the-names)). From
6.3.6 the release carries the arm64 twin, `hmailserver_<v>_arm64.deb`, with
its SBOM; the other releases' packages are CI artifacts that no release
carries yet.

**The build jobs have run on GitLab; the install jobs have not.** All seven
build jobs passed in each of `batch41`'s pipelines 2886955494 and 2887115157
(27 September 2026), and again in 2887524317; no install job has started in any
pipeline yet (see [In GitLab's pipelines](#in-gitlabs-pipelines)). Before
that, the scripts were run by hand on linix on 26 September 2026, in the jobs'
own images and by the same digests, through `build/ci/packages/deb-local.sh`,
which runs each pair of jobs the way the runner would. The times under
[By hand on linix](#by-hand-on-linix) are those runs, while other package
builds were queued for the same machine.

What has run
------------

### In GitLab's pipelines

The seven build jobs of this file, on linix, for the branch `batch41`. Each
time is the job's own, from its `BUILT:` line: apt, CMake, ninja, the testers
and CPack.

| Job | 2886955494 (`4a0e53cc8`) | 2887115157 (`70eb52b94`) |
|---|---|---|
| `deb-debian12-amd64` | passed, 876 s | passed, 747 s |
| `deb-debian12-arm64` | passed, 840 s | passed, 796 s |
| `deb-debian13-amd64` | passed, 834 s | passed, 757 s |
| `deb-debian13-arm64` | passed, 924 s | passed, 870 s |
| `deb-ubuntu24.04-amd64` | passed, 719 s | passed, 721 s |
| `deb-ubuntu24.04-arm64` | passed, 826 s | passed, 830 s |
| `deb-ubuntu26.04-arm64` | passed, 802 s | passed, 816 s |

Each built 6.3.5 and wrote the file its name says,
`hmailserver_6.3.5-1~<release>_<arch>.deb`, and `hmailserver_6.3.5_arm64.deb`
for Ubuntu 26.04. The amd64 builds ran the four tester programs, and all four
passed; the arm64 builds do not run them (see [The jobs](#the-jobs)). The
seven install jobs were in both pipelines, and in neither did one start before
the pipeline was cancelled. Pipeline 2887524317 (`63d58f8a6`, the same branch)
was running when this was written, on 28 September 2026 at 00:40 UTC: its seven
builds had passed again, in 746 to 931 s, and its install jobs were waiting for
linix. Until one has run, the install test is proved only by the runs by hand
below.

### By hand on linix

Every build below is of 6.3.4 build 43, made on linix by
`bash build/ci/packages/deb-local.sh <distro> <arch>`. Each passed its install
test in a clean container of its own release and architecture. "Build" is the
build job's whole time: apt, Boost where built, CMake, ninja, the testers and
CPack. "Install" is the install job's time.

| Release | Arch | Route | Compiler | Boost | OpenSSL | Build | Install |
|---|---|---|---|---|---|---|---|
| Debian 13 | amd64 | native | GCC 14.2.0 | 1.83 (Debian's) | 3.5.7 | 795 s | 5 s |
| Debian 13 | arm64 | cross | GCC 14.2.0 | 1.83 (Debian's) | 3.5.7 | 898 s | 35 s (qemu) |
| Debian 12 | amd64 | native | GCC 12.2.0 | 1.83.0 (boost.org, built in 46 s) | 3.0.22 | 831 s | 4 s |
| Debian 12 | arm64 | cross | GCC 12.2.0 | 1.83.0 (boost.org, built in 48 s by the first attempt; this run took it from the cache) | 3.0.22 | 914 s | 30 s (qemu) |
| Ubuntu 24.04 | arm64 | cross | GCC 13.3.0 | 1.83 (Ubuntu's) | 3.0.13 | 879 s | 42 s (qemu) |
| Ubuntu 26.04 | arm64 | cross | GCC 15.2.0 | 1.90 (Ubuntu's) | 3.5.5 | 894 s | 46 s (qemu) |
| Ubuntu 24.04 | amd64 | native | GCC 13.3.0 | 1.83 (Ubuntu's) | 3.0.13 | 827 s | 8 s |
| Ubuntu 26.04 | amd64 | native | GCC 15.2.0 | 1.90 (Ubuntu's) | 3.5.5 | 1045 s | 8 s |

The Ubuntu 26.04 amd64 row was the trial of a pair of jobs the check then
removed, because `linux-build` already makes that file (see
[The names](#the-names)). `deb-local.sh ubuntu26.04 amd64` still builds it by
hand, with GCC; no job does.

**Tester programs.** On each native build, the server's tester programs
(`object-store-signature-tests`, `script-binding-tests`, `snmp-tests`,
`spf-tests`) passed with that release's GCC. The CI's own `linux-build` job uses
clang. The Ubuntu 26.04 amd64 build was the slowest (ninja took 990 s, against
750 to 790 s for the other native builds). It ran last, while other writers'
jobs were queued on linix; nothing else was measured about why.

**Dependencies.** The `Depends` lines the builds wrote show why each release
needs its own package:

- Debian 12: `libc6 (>= 2.35)`, `libssl3`, `libldap-2.5-0`, `libstdc++6 (>= 12)`;
- Debian 13 and Ubuntu 26.04: `libc6 (>= 2.38)`, `libssl3t64`, `libldap2`,
  `libstdc++6 (>= 14)`;
- Ubuntu 24.04: the same, with `libstdc++6 (>= 13.1)`.

None names Boost.

**Two defects the trials found, both fixed in the scripts before they were
committed:**

1. **The first Debian 13 amd64 run failed its own package check.**
   `dpkg-deb -c | grep -q` ended the pipe early, and under `pipefail` the check
   failed a package that was correct. The listing is now read first and
   searched afterwards.
2. **The first Debian 12 arm64 run failed in CPack.** `dpkg-shlibdeps` could not
   find the arm64 `libstdc++.so.6`. The cause: Debian 12's
   `crossbuild-essential-arm64` brings only the cross toolchain's own copy
   (under `/usr/aarch64-linux-gnu`), and the Boost `-dev` packages that pull in
   `libstdc++6:arm64` elsewhere are not installed there. Every cross build now
   installs `libstdc++6:arm64` and `libgcc-s1:arm64` itself.

The writer's rows were measured with the scripts as the writer committed them,
except for two small differences. (All of them also ran before the check's
apt changes, described in the check's run 3 below.)

- Every row but Debian 12 arm64 ran before the explicit
  `libstdc++6:arm64 libgcc-s1:arm64` (each run's apt line shows it). That line
  changes nothing for a native build. On the other three cross builds, the
  Boost `-dev` packages had already pulled the same two packages in.
- The Debian 13, Debian 12 amd64 and Ubuntu 24.04 arm64 rows also ran before a
  cosmetic change that stopped `g++` being listed twice on Debian 12.

**linix rebooted during the trials.** At 04:37 UTC linix was shut down and
restarted by systemd; these trials did not start that. The reboot took the
`qemu-aarch64` binfmt handler with it. The owner has since installed the
persistent one (see [What is left](#what-is-left)).

**The check's runs (26 September 2026, on linix).** The table above is the
writer's. The check then ran the following itself:

1. **The scripts as the writer's check committed them (4872135cc).** From an
   archive of that commit, through `deb-local.sh`, under the package lock:
   - Debian 13 amd64: build job 846 s (ninja 809 s), the four tester programs
     passed; install job 5 s. Passed.
   - Debian 13 arm64: build job 948 s (ninja 909 s); install job 27 s under
     qemu, through the `qemu-user-binfmt` handler. Passed.
2. **A negative control of the install test.** The Debian 13 amd64 package
   from run 1 was repacked with `libpq5` taken out of its `Depends`. In a clean
   `debian:13` container, `deb-install.sh` then failed (exit 1) with
   `libpq.so.5 => not found`. So a missing dependency does fail the test.
3. **A defect in the arm64 install test.** Run on the writer's Debian 13 arm64
   package, the install test failed once with "none of the choices are
   installable" for `libldap2`, and passed when repeated. The cause, found by
   repeating `apt-get update` in clean `debian:13` arm64 containers under qemu:
   - apt sometimes could not connect to `deb.debian.org` at all ("Unable to
     connect to deb.debian.org:80:"). That happened in 5 of 21 containers,
     with or without `Acquire::Retries=3`.
   - With `Acquire::ForceIPv4=true` it happened in none of 24.
   - In `debian:13` amd64 containers, which run without emulation, it
     happened too, but rarely: in 1 of 40, and in 1 of 90 with IPv4 forced.
   - `apt-get update` still exits 0 then, so the install that follows fails on
     a `Depends` it has no candidate for, as if the package were wrong.

   The fixes:
   - `deb-install.sh` runs apt over IPv4 only; the job containers on linix
     have no IPv6 route.
   - Both scripts treat a failed index fetch as a failed update, and try the
     update three times.
   - Both scripts try an install that failed only to download up to three
     times. An install that failed for any other reason fails at once.

   What the fixed scripts did (the working tree over 4872135cc):
   - the Debian 13 arm64 install test passed 8 times in 8, before the install
     retries were added;
   - the Ubuntu 24.04 and Ubuntu 26.04 arm64 packages passed it twice each,
     and the Debian 13 amd64 package once, also before the install retries;
   - in a container with no network, `deb-install.sh` tried the update three
     times and failed as an update failure (exit 1), and so did `deb-build.sh`
     (exit 3);
   - against a stand-in `apt-get` whose install failed as a download failure
     first, `deb-install.sh` tried again and passed. When the install failed
     as a missing package, it failed at once. `deb-build.sh` tried again after
     the download failure, then stopped at a missing package (exit 3);
   - the final `deb-install.sh` passed on the writer's Debian 12 amd64,
     Ubuntu 24.04 amd64 and arm64, and Ubuntu 26.04 arm64 packages, and on
     Debian 13 arm64 again. The writer's Debian 12 arm64 package was no
     longer on linix, so that one has not been tested with it;
   - the final `deb-build.sh` ran through its apt steps with the real
     `apt-get`, natively and on the cross route (Debian 13), each update and
     install on its first try, and was stopped at configure by a stand-in
     `cmake`. Everything after the apt steps is unchanged from 4872135cc,
     which run 1 built in full.

   A full Debian 13 arm64 pair with the final scripts was queued for linix's
   package lock, and had not started when this was written. So no complete
   build with the final `deb-build.sh` has run.
4. **shellcheck** 0.10.0 (Debian 13's) on the three scripts, as of 4872135cc
   and again as fixed, found two notes at the info level and nothing more: SC2012 (`ls` of two fixed paths) and
   SC2015 (`[ ] && [ ] || refuse`, which is meant).
5. **GitLab's own reading of the file.** `.gitlab-ci.yml` with this file
   inlined was sent to GitLab's CI lint API. It was valid, with no errors and no
   warnings. Its dry runs, which evaluate the rules and create nothing, gave:
   - for `master`, the seven builds manual (`allow_failure`) and the seven
     install tests `on_success`;
   - for the tag `v6.3.3`, all fourteen `on_success`.

   That is GitLab evaluating the rules, not a pipeline. None of these jobs had
   run on GitLab then ([In GitLab's pipelines](#in-gitlabs-pipelines) has what
   has run since).

Why each release has its own package
------------------------------------

`dpkg-shlibdeps` writes a package's `Depends` line from the libraries the binary
was linked against, with the package names and minimum versions of the machine
that built it. Examples:

- `libc6 (>=` the builder's glibc`)`;
- `libssl3` on one release and `libssl3t64` on the next;
- `libldap-2.5-0` on Debian 12 and `libldap2` on later releases.

A package built on Ubuntu 26.04 therefore names packages, or versions of them,
that Debian 12 or Ubuntu 24.04 does not have, and apt refuses it there. Boost is
not in `Depends` at all, because the server links it statically
(`hmailserver/source/Server/CMakeLists.txt`, "Boost is linked STATICALLY").

The jobs
--------

Debian 12, Debian 13 and Ubuntu 24.04 have four jobs each: a build and an
install test, for each architecture. Ubuntu 26.04 has the arm64 jobs only (its
amd64 package is `linux-build`'s), and a third beside them,
`deb-ubuntu26.04-arm64-sbom`, since the release carries that package. That
makes 15 jobs.

| Job | Runs on | Image | What it does |
|---|---|---|---|
| `deb-<distro>-amd64` | linix | the release's amd64 image | `build/ci/packages/deb-build.sh`: the release's `g++`, `cmake`, `ninja` and `-dev` packages from its own archive; CMake, then the server's tester programs, then CPack |
| `deb-<distro>-arm64` | linix | the release's **amd64** image | the same script, cross-compiling. It uses the release's `crossbuild-essential-arm64`, its `:arm64` `-dev` packages (multiarch) and `build/ci/packages/aarch64-linux-gnu.cmake`. The tester programs are not run, because they are arm64 programs |
| `deb-<distro>-<arch>-install` | linix | the release's image **of that architecture** | `build/ci/packages/deb-install.sh`, on a clean container (see [The install test](#the-install-test)). The arm64 container runs under qemu-user |
| `deb-ubuntu26.04-arm64-sbom` | linix | the CI image | `build/ci/packages/package-sbom.sh`: the package's own SBOM, `hmailserver_<v>_arm64.deb.cdx.json`, as `linux-build` makes the amd64 one's, from what the build recorded it was built against (see [The SBOM](#the-sbom)) |

`<distro>` is `debian12`, `debian13`, `ubuntu24.04` or, for arm64 only,
`ubuntu26.04`.

**Images.** Every image is the distribution's own on Docker Hub (`debian:12`,
`debian:13`, `ubuntu:24.04`, `ubuntu:26.04`). Each is pinned by the digest of
its architecture's own manifest, not by the multi-architecture index. The amd64
and arm64 images are then two separate references in the runner's image store,
so `pull_policy: if-not-present` can never give an arm64 job the amd64 image
held under the same name. The Ubuntu 26.04 digests are the two manifests of the
index that the CI image and `linux-package-deb` already pin (`513c0741…`).

**When the jobs run:**

- for every `v*` tag and every `batch*` branch, automatically;
- on any other protected branch (`master`), only when started by hand. Press a
  build job's play button, and its install job follows: the install jobs are
  never manual themselves, and GitLab shows one as skipped until its build has
  been played (this is how the rules are written; no pipeline has shown it
  yet);
- never for a merge request, an unprotected ref, or a scheduled, fuzz or feed
  pipeline. That is the same refusal every job on the project's own runners
  begins with.

**Serialisation.** The build jobs share `resource_group: linix-package-build`,
so only one of these builds runs on linix at a time, across pipelines. Each
build runs six compilers at once (`HM_BUILD_JOBS`); linix had 16 cores and
11 GB of memory on 26 September 2026. The group holds only the jobs that name
it: the RPM and AppImage builds and `linux-build` are outside it unless their
files name it too.

**Artifacts.** Each build job keeps `packages-deb/<distro>-<arch>/` for 14 days:

- the `.deb`;
- `SHA256SUMS.txt`;
- `build-info.txt`: the release, the route, the compiler, CMake, Boost, OpenSSL,
  libpq, OpenLDAP and zlib versions, the control fields, the hash and the times;
- `built-against/`: `dpkg.txt`, every package the container had installed with
  its architecture and version (`dpkg-query -W`), and `boost-version.hpp`, the
  Boost header the build compiled against, for the package's SBOM;
- the logs.

Each install job adds `install-<distro>-<arch>.txt`, and
`deb-ubuntu26.04-arm64-sbom` keeps `hmailserver_<v>_arm64.deb.cdx.json` beside
the package.

### The SBOM

Every compiled file a release carries goes out with its own CycloneDX SBOM,
`<file>.cdx.json` (SECURITY.md, "Software bills of materials"). `linux-build`
makes the amd64 package's in the CI image: `build/native-dependencies.ps1`'s
component list, then `build/asset-sbom.py`, which asks the container's own dpkg
what each library was built against. The release's containers here have neither
PowerShell nor Python, so the build job records instead what the container had
installed (`built-against/`), and `deb-ubuntu26.04-arm64-sbom` makes the arm64
package's SBOM from that in the CI image (`asset-sbom.py make --dpkg-list
built-against/dpkg.txt --target-arch arm64`). In a cross build the C and C++
libraries are the cross toolchain's (`libc6-dev-arm64-cross`,
`libstdc++-<n>-dev-arm64-cross`), and every other library is the `:arm64`
`-dev` package the build installed, never the amd64 host's package of the same
name. Every build records it; only the package a release carries gets an SBOM
job.

The names
---------

| Release | File | `Version:` |
|---|---|---|
| Ubuntu 26.04 | `hmailserver_<v>_arm64.deb` (this file); `hmailserver_<v>_amd64.deb` (`linux-build`) | `<v>` |
| Ubuntu 24.04 | `hmailserver_<v>-1~ubuntu24.04_<arch>.deb` | `<v>-1~ubuntu24.04` |
| Debian 13 | `hmailserver_<v>-1~debian13_<arch>.deb` | `<v>-1~debian13` |
| Debian 12 | `hmailserver_<v>-1~debian12_<arch>.deb` | `<v>-1~debian12` |

**How the suffix is made.** The suffixed names are CPack's own `DEB-DEFAULT`
names, for `CPACK_DEBIAN_PACKAGE_RELEASE=1~<distro>`. The build passes that
value to `cpack -D`, and nothing is renamed afterwards, so the file name and the
control file's `Version` are always the same value. `deb-build.sh` checks both,
and fails if either differs. It also fails when:

- the `Architecture` field is not the target's;
- `Depends` is empty or names Boost;
- the binary is not an executable for the target;
- the container is not the release the file would be named after.

**The generic names are unchanged.** The Ubuntu 26.04 builds keep the names that
installed servers' update checker asks for (`UpdateChecker.cpp`,
`PlatformInstallerName_`, and `installer_names()` in
`build/generate-update-feed.py`).

**One job per generic name.** `hmailserver_<v>_amd64.deb` stays
`linux-build`'s clang build in the CI image, which is Ubuntu 26.04:

- the release collects it, with `linux-build`'s `SHA256SUMS.txt`
  (`sources()` in `build/ci/release-chain.py`);
- `linux-package-deb` installs it and starts it against PostgreSQL.

This file therefore has no `deb-ubuntu26.04-amd64` job. The writer's first
version had one, a GCC build of the same name. The check removed it, so a
release has only one file of that name to take. `hmailserver_<v>_arm64.deb`
comes from `deb-ubuntu26.04-arm64`, the only job that makes it. From 6.3.6 the
release collects it with its SBOM, holding it to the `SHA256SUMS.txt` that job
wrote and adding its line to the release's sums, and `release` waits for the
build, its install job and `deb-ubuntu26.04-arm64-sbom`.

**The update checker was not changed in this work.** A server installed from
any `.deb` looks for `hmailserver_<tag>_<arch>.deb`, which is the Ubuntu 26.04
build. A server installed from a Debian 12, Debian 13 or Ubuntu 24.04 package is
therefore offered the Ubuntu 26.04 package on update. The 6.3.4 Ubuntu 26.04
package needs `libc6 (>= 2.38)` and `libssl3t64` (the same `Depends` line in
`linux-build`'s clang package, read from a 6.3.4 `linux-build.sh` run on linix,
and in the GCC trial build):

- **Debian 12** has glibc 2.36 and `libssl3`, so apt refuses the package there,
  and the installed server stays as it was.
- **Debian 13 and Ubuntu 24.04** happen to meet those `Depends` for this build.
  Nothing guarantees that for the next one. Choosing a package by distribution is not done; the
  row *Per-distribution `.deb` and `.rpm` packages* of [Roadmap3.md](../../Roadmap3.md) carries it.

The arm64 route, and why
------------------------

The arm64 packages are **cross-compiled** on linix, which is x86-64, in the
release's own amd64 container. Three routes were possible:

1. **Multiarch cross-compilation (chosen).** `dpkg --add-architecture arm64`,
   the release's `crossbuild-essential-arm64`, and its `:arm64` `-dev` packages.
   - The compiler runs natively, so the build takes about as long as an amd64
     build (the measured times are above).
   - Every library the binary links is still the release's own arm64 package.
     `dpkg-shlibdeps` reads their `shlibs` files and writes an arm64 `Depends`
     line (`DEB_HOST_ARCH=arm64` makes it use the target's `objdump`).
   - The CMake build runs nothing it has built, so nothing needs emulating.
   - On Ubuntu, arm64 packages come from `ports.ubuntu.com` only, so the build
     limits the stock sources to amd64 and adds the ports for arm64. Debian
     serves every architecture from `deb.debian.org`.
2. **A native arm64 container under qemu-user.** Every compiler process would
   be emulated, and a C++ build under qemu-user is commonly 5 to 10 times
   slower. This route was not tried.
3. **GitLab's hosted arm64 runner.** It would build natively on real arm64
   hardware, but it uses compute minutes. `saas-linux-small-arm64` has 2 vCPUs
   and a cost factor of 1 (medium: 4 vCPUs, factor 2). A build that takes
   linix's six cores about the time shown above would take roughly three times
   as long on the small runner, and there are four releases per pipeline.
   GitLab's free tier includes 400 compute minutes a month, and that estimate
   uses a large share of them on every tag. It was not used.

**Emulation is used only for the install test.** A package that was
cross-compiled is proved to run by installing it in an arm64 container and
running it there (`--version`, `ldd`), under qemu-user. This needs the
`qemu-aarch64` binfmt handler, with the `F` flag, registered in linix's kernel.
See [What is left](#what-is-left).

Debian 12 and Boost
-------------------

Debian 12's own Boost is 1.74, and the server needs 1.83 or later. So
`deb-build.sh` builds Boost 1.83.0 in the job:

- **Source:** boost.org's release tarball,
  `https://archives.boost.io/release/1.83.0/source/boost_1_83_0.tar.bz2`,
  checked against the SHA-256 that boost.org publishes beside it
  (`6478edfe…3b8e`).
- **Build:** b2 builds only the three compiled libraries the server links
  (thread, chrono and filesystem), statically, with `-fPIC`. The arm64 build
  uses a `gcc-arm64` toolset that points at `aarch64-linux-gnu-g++`.
- **Cache:** the result is kept in the runner's cache (`.cache/boost`, key
  `boost-1.83.0-debian12-<arch>`), so later pipelines skip the Boost build.

1.83.0 is the oldest Boost the server is built against anywhere else (Ubuntu
24.04 and Debian 13). Everything else comes from Debian 12's archive: GCC 12,
CMake 3.25 and OpenSSL 3.0.

The install test
----------------

`deb-install.sh` runs in a clean container of the release and the package's
architecture:

1. `apt-get install ./<the .deb>` resolves `Depends` from that release's
   archive, and `dpkg` shows the file's `Version` and `Architecture` as
   installed. apt runs over IPv4 only. An index that cannot be fetched fails
   the update, not the install, and the update is tried three times. An
   install that failed only to download is tried up to three times (see
   [What has run](#what-has-run), the check's run 3).
2. `ldd /usr/bin/hmailserver` finds every library.
3. `hmailserver --version` runs, under qemu on arm64, and answers
   `hMailServer <v> build <n>`.
4. Every file an installation needs is present: the binary, `hmctl`, the data
   files, the database scripts, the Control Deck (checked byte for byte against
   the repository's), the three systemd units, the update helper, the auto-ban
   hook, the configuration and the logrotate rule.
5. `postinst` made the `hmailserver` user and enabled `hmailserver.service` and
   `hmailserver-update.path` through `deb-systemd-helper`.
6. `apt-get remove` takes the package off again.

**What the test does not do.** It does not start the server against a database,
and it has no systemd to start the unit.
`build/ci/linux-package-install.sh` does the first for linux-build's amd64
package in the `linux-package-deb` job, and says what a container cannot prove.

Running a job by hand
---------------------

On any x86-64 machine with docker, from a copy of a checkout:

    bash build/ci/packages/deb-local.sh debian13 arm64

It runs the build, then the install test, in the images the CI file names (read
from `.gitlab/ci/packages-deb.yml`), and prints each one's time. For arm64 it
first refuses unless the host has the `qemu-aarch64` binfmt handler.

What is left
------------

1. **The binfmt handler on linix: done by the owner.** The arm64 install jobs
   need the `qemu-aarch64` handler, with the `F` flag, in linix's kernel.
   - The trials registered it with
     `docker run --privileged --rm tonistiigi/binfmt --install arm64` (flags
     `POCF`). That registration did not survive the reboot on 26 September.
   - The owner then installed Ubuntu's `qemu-user-binfmt` package on linix
     (26 September 2026). Its `/usr/lib/binfmt.d/qemu-aarch64.conf` is loaded
     by `systemd-binfmt` at every boot, with the flags `POF`.
   - The check's own arm64 install test ran under that handler (see
     [What has run](#what-has-run)). Nobody has yet seen it come back after a
     reboot.
2. **Wiring into the release: done for the arm64 package, for 6.3.6.** The
   check decided what a release carries:
   - `hmailserver_<v>_amd64.deb` stays `linux-build`'s, as now (see
     [The names](#the-names));
   - the release can add the seven files from these jobs:
     `hmailserver_<v>_arm64.deb` and the six whose names carry a release.

   `hmailserver_<v>_arm64.deb` is in (batch42, 28 September 2026): `release`
   needs `deb-ubuntu26.04-arm64`, `deb-ubuntu26.04-arm64-sbom` and
   `deb-ubuntu26.04-arm64-install`, and `build/ci/release-chain.py collect`
   takes the package and its SBOM. `build/check-release-chain.py` holds
   `release` to those three jobs, and `collect` to the paths they leave the
   files at (`CARRIED_PACKAGES`). The six whose names carry a release are not
   in yet:
   - a tag's `release` job does not wait for their jobs. They run on every
     `v*` tag and can fail its pipeline, but `release` can be started before
     they finish;
   - `build/check-release-chain.py` reads no other job of an included file as
     a gate of a release.
3. **Watch the first pipeline that runs the install jobs.** The build jobs
   have passed in pipelines (see [In GitLab's pipelines](#in-gitlabs-pipelines));
   no install job has run to an end in one yet.
