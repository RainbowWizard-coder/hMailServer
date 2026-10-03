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
6.3.6 the release carries the arm64 twin, `hmailserver_<v>_arm64.deb`, and
every other release's package for both architectures, each with its SBOM, a
line in the sums, a bundle and its `.sigstore.json` copy: `release-packages`
(`.gitlab/ci/release-packages.yml`) gathers them once each build's install job
has passed.

**The build and install jobs have run on GitLab.** All seven build jobs passed
in each of `batch41`'s pipelines 2886955494 and 2887115157 (27 September 2026);
in 2887524317 (28 September) the seven install jobs ran for the first time, and
all seven passed. Every build and install job passed again in `batch41`'s
2890818010 and 2891564033, in 6.3.5's tag pipeline 2892097590, and in
`batch42`'s 2896082335 and 2897966924 (30 September 2026), which built 6.3.6
and held each package to its licence files (see
[In GitLab's pipelines](#in-gitlabs-pipelines)). Before that, the scripts were
run by hand on linix on 26 September 2026, in the jobs'
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
the pipeline was cancelled.

In pipeline 2887524317 (`63d58f8a6`, the same branch, 28 September 2026) the
seven builds passed again, in 746 to 931 s, and the seven install jobs ran for
the first time: each package installed in a clean container of its release and
architecture, ran, carried its units and came off again, in 4 to 34 s, the arm64
ones under qemu-user. All fourteen jobs passed again in `batch41`'s 2890818010
and 2891564033 and in 6.3.5's tag pipeline 2892097590 (29 September), and in
`batch42`'s two pipelines of 30 September 2026, which built 6.3.6:

| Job | 2896082335 (`c26ce1e59`): build / install | 2897966924 (`cec2b151d`): build / install |
|---|---|---|
| `deb-debian12-amd64` | 907 s / 9 s | 979 s / 8 s |
| `deb-debian12-arm64` | 1191 s / 75 s | 1012 s / 71 s |
| `deb-debian13-amd64` | 929 s / 10 s | 1031 s / 11 s |
| `deb-debian13-arm64` | 1070 s / 71 s | 1270 s / 88 s |
| `deb-ubuntu24.04-amd64` | 885 s / 14 s | 933 s / 12 s |
| `deb-ubuntu24.04-arm64` | 940 s / 69 s | 990 s / 79 s |
| `deb-ubuntu26.04-arm64` | 906 s / 93 s | 991 s / 96 s |

Each time is the job's own line: `BUILT:` for the build, `PACKAGE PASSED:` for
the install. In both, every build checked its package's licence files and every
install job the installed ones (`LICENCES PASSED`, [The licences](#the-licences)),
and `deb-ubuntu26.04-arm64-sbom` made the generic arm64 package's SBOM, 8
components, from what the build recorded it was built against.

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

One name is not the linker's: `ca-certificates`, which `CMakeLists.txt` puts
first on the line from 6.3.6 (`CPACK_DEBIAN_PACKAGE_DEPENDS`, and `Requires:` in
the `.rpm`). OpenSSL has no certificate authorities of its own, and every HTTPS
request the server makes - the update check and its download, a JWKS, an object
store, a webhook - is checked against the distribution's bundle in
`/etc/ssl/certs`. A real Ubuntu or Debian server has it already; a minimal image
does not, and there the update check would fail on the feed's certificate. The
first Linux live update found the package did not ask for it: PostgreSQL had
pulled it in.

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
- `Depends` names no library (`libc6`, which `dpkg-shlibdeps` always writes),
  does not name `ca-certificates`, or names Boost;
- the licence files are missing or not the tree's ([The licences](#the-licences));
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
  (`6478edfe…3b8e`). `build/ci/packages/fetch-boost.sh` fetches it: from the
  job's cache first (`.cache/boost-tarball`, keyed by that SHA-256), then
  from archives.boost.io, then from SourceForge's copy
  (`downloads.sourceforge.net/project/boost`), each with six attempts, and
  from any of them only with that SHA-256.
- **Build:** b2 builds only the three compiled libraries the server links
  (thread, chrono and filesystem), statically, with `-fPIC`. The arm64 build
  uses a `gcc-arm64` toolset that points at `aarch64-linux-gnu-g++`.
- **Cache:** the result is kept in the runner's cache (`.cache/boost`, key
  `boost-1.83.0-debian12-<arch>`), so later pipelines skip the Boost build.

1.83.0 is the oldest Boost the server is built against anywhere else (Ubuntu
24.04 and Debian 13). Everything else comes from Debian 12's archive: GCC 12,
CMake 3.25 and OpenSSL 3.0.

The licences
------------

From 6.3.6 every `.deb` here, and `linux-build`'s, carries the licences it goes
out under, where Debian looks for them. Before it, of the Linux packages only
the Arch package carried hMailServer's own licence.

- `/usr/share/doc/hmailserver/copyright`, the copyright file Debian Policy 12.5
  asks every package for. It is the Debian source package's own,
  `platform/packaging/debian-src/debian/copyright`, in Debian's machine-readable
  format: who holds the copyright of what the server is built from and under
  which licence, with the AGPL-3.0-or-later in full and the terms of what is
  compiled into the binary - QuickJS (MIT), the MIME coder (BSD-1-Clause),
  CStdString (a permissive notice of its own), the Public Suffix List's rules
  (MPL-2.0) and the rest it names. Ubuntu's container images keep it where
  their `dpkg` leaves out the rest of `/usr/share/doc`.
- `/usr/share/hmailserver/licenses/AGPL-3.0.txt`, hMailServer's own licence, the
  FSF's file byte for byte (the repository's `LICENSE` without its carriage
  returns), beside `QuickJS.txt`, as the AppImage carries them.

The install rules put the two texts in, for every install; CPack alone puts the
copyright file in (`platform/packaging/cpack-licenses.cmake`), so the source
package, which installs with CMake, places its own with debhelper. No other
licence text is owed: Boost, which the server links statically, asks for its
notice only with its source, and OpenSSL, libpq, OpenLDAP and zlib are the
distribution's shared libraries, whose packages carry their own copyright files.
`build/ci/packages/check-licenses.sh` holds the three files to the tree, by
SHA-256, mode and owner: `deb-build.sh` and `linux-build` fail a package
without them, and the install test checks them as installed.

**Run in `batch42`'s pipelines 2896082335 and 2897966924 (30 September 2026).**
In both, `check-licenses.sh` passed (`LICENCES PASSED`) over the package each of
the seven builds made, over the files every install job installed, and over
`linux-build`'s `hmailserver_6.3.6_amd64.deb` and what `linux-package-deb`
installed of it. Before that, on the laptop on 29 September 2026:
`linux-build`'s route (Ubuntu 26.04, clang, CPack) made the `.deb` with the
three files; in `ubuntu:26.04`, `deb-install.sh` and `linux-package-install.sh
deb` (the server started against PostgreSQL) passed with it, and failed at this
step with the package made before the change. Lintian's Debian profile no longer
reports `no-copyright-file` and reports nothing new. A miniature of the tree with
the same install rules and CPack settings made and installed the `.deb` with
Debian 12's, Ubuntu 24.04's and Ubuntu 26.04's own CMake and dpkg.

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
5. The licences are installed as the tree has them, and `dpkg -L` names each:
   the copyright file, `AGPL-3.0.txt` and `QuickJS.txt` (`check-licenses.sh
   --installed deb`; [The licences](#the-licences)).
6. `postinst` made the `hmailserver` user and enabled `hmailserver.service` and
   `hmailserver-update.path` through `deb-systemd-helper`.
7. `apt-get remove` takes the package off again.

**What the test does not do.** It does not start the server against a database,
and it has no systemd to start the unit.
`build/ci/linux-package-install.sh` does the first for linux-build's amd64
package in the `linux-package-deb` job, and `package-systemd-ubuntu26.04` does
the second ([Started under systemd](#started-under-systemd)).

Started under systemd
---------------------

`package-systemd-ubuntu26.04` starts `linux-build`'s `hmailserver_<v>_amd64.deb`
under its own unit, with systemd as PID 1, and `package-systemd-fedora44` does the
same for `rpm-fedora44-x86_64`'s `hmailserver-<v>-1.fc44.x86_64.rpm`
([PackagesRpm.md](PackagesRpm.md), "Started under systemd"). No other job does: the
install tests and `linux-package-deb` run in containers without systemd, and
`linux-package-install.sh` starts the server the way the unit would, not under it.
So the unit's sandbox, its first-start step and the package scripts' systemd
branches had run on no machine of the project's. The jobs are in
`.gitlab/ci/package-systemd.yml`, and both run `build/ci/package-systemd-test.sh`
on linix's privileged runner entry, `linix-systemd`
([ContinuousIntegration.md](ContinuousIntegration.md), "linix"), in a `batch*`
branch's pipeline and on master, never on a `v*` tag: they are checks, like
`linux-live-update`, and the release does not wait for them.

1. podman, from Ubuntu's archive, in the job's container. With it, an image of the
   distribution's own, pinned by digest, with systemd added and `systemd-binfmt`
   masked. systemd runs as PID 1 in it (`podman run --systemd=always`), in the
   job's network namespace, beside the job's PostgreSQL service.
2. The package installed with `apt-get` or `dnf`, which resolve its dependencies
   from the distribution's archive. The package's scripts then run with systemd
   up: `hmailserver.service` must be enabled and not started, and
   `hmailserver-update.path` enabled and started. `systemd-analyze verify` must
   pass the three units on the distribution's own systemd. Each step that fetches
   (podman, the image, the package's dependencies) is tried up to three times,
   15 seconds apart, when its output shows only that the archive or registry could
   not be reached, and fails at once otherwise, as the install tests' steps do.
3. The configuration, as `packaging/README.md` tells an administrator: the INI
   pointed at the database, `--check-config` and `--create-database` as
   `hmailserver`, the administrator's password and `RestApiPort` set.
4. `systemctl start`. The unit's journal must not hold a line in which systemd
   says it is ignoring part of the sandbox, as it does where a container forbids
   it ("ProtectHostname=yes is configured, but UTS namespace setup is prohibited
   (container manager?), ignoring namespace setup"). The sandbox is read from the
   running process: it must run as `hmailserver`, hold `CAP_NET_BIND_SERVICE`
   and nothing else (effective, permitted, bounding and ambient), have
   `NoNewPrivs` and a seccomp filter, and have a mount namespace of its own. In
   that namespace `/`, `/usr`, `/etc` and
   `/var` must be read-only, `/var/lib/hmailserver` and `/var/log/hmailserver`
   writable, `/tmp` the unit's private one and `/dev` its own tmpfs: another
   tmpfs than PID 1's, since podman gives the container a tmpfs `/dev` as well.
   `systemd-analyze security` is kept with the job's artifacts.
5. SMTP on 25 and 587 (a 220, and EHLO offering STARTTLS), POP3 on 110, and IMAP
   on 143 (CAPABILITY offering STARTTLS). STARTTLS is there only because the
   unit's first start ran `--apply-new-install-defaults --if-marked`, which made
   the certificate. That step must have exited 0 and removed `.new-installation`.
   The REST API answers `GET /api/v1/status` with the administrator's password,
   refuses a wrong one with 401, and serves the Control Deck at `GET /`.
   `hmailserver-update check` must then read the database's schema version - what
   the live update's rollback rests on - "asked as hmailserver, in
   hmailserver.service's sandbox": the helper has systemd run the server's
   `--schema-version` as a transient service with the unit's own sandboxing
   properties, as `systemctl show` reports them, and reads nothing where systemd
   will not start it so. It must find nothing missing, and the journal must not
   hold a line in which systemd says it is ignoring part of that sandbox.
6. `systemctl reload` must reinitialise the server in the same process. The
   server answers the unit's SIGHUP by stopping its servers and starting them
   again, so the socket listening on 25 must be a new one within a minute, and
   SMTP and the REST API must answer again. `systemctl stop` must end with the
   result `success` and nothing listening.
7. Started again, the package is removed with the server running. Its scripts
   must stop the server and disable both units, leaving nothing running, listening
   or linked.
8. The container stops as a system does (`SIGRTMIN+3`), and it and its image are
   removed.

**Why the container systemd runs in is not privileged.** linix's privileged
runner entry is there because podman needs privilege to run a container inside
the job's. The container systemd runs in has `CAP_SYS_ADMIN`, which systemd needs
to build the unit's mount namespace. Without it, Ubuntu 26.04's systemd 259 does
not start the unit at all: it fails at the step USER ("Failed to keep
CAP_SYS_ADMIN: Operation not permitted", status 217). Nothing else is granted: `/proc/sys` and
`/sys` stay read-only, as in any unprivileged container. This matters because a
privileged container shares the host's kernel settings, and its systemd treats
them as its own. On 29 September 2026 a systemd in a privileged container on
linix removed every binfmt_misc handler of the host when it stopped, and the
arm64 jobs then failed with "exec format error". The image masks
`systemd-binfmt` as well, and the script counts the host's handlers through a
read-only mount, before and after; if any went, the job fails and says how to
restore them (`sudo systemctl restart systemd-binfmt` on linix).

**What has run.** Both jobs' script, by hand on linix on 2 October 2026, as the
runner runs it: a privileged `ubuntu:26.04` job container by the job's digest,
with six CPUs and 8 GB, beside a `postgres:18` service (18.6) by the job's digest
and with `.no-github`'s hosts entries. It ran with `batch42`'s packages from
pipeline 2897966924 (`cec2b151d`: `linux-build` job 16842363185,
`rpm-fedora44-x86_64` job 16842363163), and both passed, with systemd 259 on each
distribution and an exposure of 1.7 ("OK") from `systemd-analyze security`. Each
run took under two minutes, from installing podman to removing the image: the last
ones, with the script as it is now, 95 s for Ubuntu 26.04 and 101 s for Fedora 44,
and 100 s for Ubuntu with a drop-in that changes nothing, which the script put in
and took out again. linix's 32 binfmt_misc handlers, the 31 of qemu among them,
were there before and after every run. Each check was shown to fail when what it
checks is not so:

| Negative control | Where it failed |
|---|---|
| a drop-in adding `CAP_SYS_ADMIN` to the unit's capabilities | "CapEff is 0000000000200400, not CAP_NET_BIND_SERVICE alone" |
| a drop-in setting `ProtectSystem=no` | "/ is not read-only to the server (ProtectSystem=strict): 'rw,relatime'" |
| a drop-in setting `PrivateDevices=no` | "/dev is not the unit's own tmpfs (PrivateDevices): the server's is tmpfs 0:150, PID 1's 0:150" |
| a drop-in clearing `ExecStartPre` | "25 does not offer STARTTLS: the first start did not apply the new installation's defaults" |
| a drop-in making `ExecReload` `/bin/true` | "the reload did not reinitialise the server: 60 seconds after it the socket listening on 25 is '1072541', and was 1072541" |
| a drop-in making the stop signal `SIGCONT`, with five seconds to stop | "hmailserver.service is 'failed' (is-active), not inactive" |
| a drop-in whose `ExecStartPre` writes systemd's own "is configured, but ... ignoring namespace setup" line to the journal | "systemd left part of the unit's sandbox out" |
| the systemd container without `CAP_SYS_ADMIN` | "the REST API did not answer on 8046: the unit is 'activating', and a unit that stays active is given 90 seconds" (it failed at step USER, status 217, and was waiting to restart) |
| the job container not privileged | "REFUSED: binfmt_misc cannot be mounted here: this is not the privileged runner" |
| `systemd-binfmt` not masked | `systemctl is-enabled` answers "static", not "masked" |

The drop-ins go in through `HM_SYSTEMD_TEST_DROPIN`, which no job sets; the
script takes its own drop-in out again before it looks for what the removal left.
The control with the journal line writes the line itself: a real one comes only
from a container that forbids part of the sandbox, where the unit then either
starts without it or, as without `CAP_SYS_ADMIN`, does not start. The binfmt
guard itself, the count falling during a run, was not provoked: that would take
linix's handlers away. The jobs have not yet run in a pipeline: the first is
`batch42`'s next.

The re-check of 2 October found two checks that held without what they check,
and the third and fifth rows are what replaced them. podman gives the container a
tmpfs `/dev` of its own, so a `/dev` of type tmpfs proved nothing about
`PrivateDevices`; and a reload was taken as done when the same process answered
afterwards, which it also does when the signal changes nothing. With
`PrivateDevices=no` and `ExecReload=/bin/true` together, the first version of the
script passed both checks and every one after them but the last, which failed only
on the drop-in's own directory: the reason the script now takes it out, so that a
control which changes nothing passes. The same day a run failed
when Fedora's mirror list timed out from linix, which is why the steps that fetch
are now tried again.

The live update
---------------

A server installed from the Ubuntu 26.04 `.deb` updates itself.
`hmailserver-update.path`, which `postinst` enables and starts, takes the
server's request and runs `/usr/lib/hmailserver/hmailserver-update` as root,
which verifies the package again, installs it with `apt-get` - whose `postinst`
upgrades the schema - and starts the unit. It was tried from 6.3.4 to 6.3.5 on
29 September 2026, unattended ([UpdateFeed.md](UpdateFeed.md), "From 6.3.4 on,
by the live update"). `sudo /usr/lib/hmailserver/hmailserver-update check`
says whether a machine can apply one. To have it apply updates by itself:

```sh
sudo hmailserver --set-setting UpdateCheckEnabled=1
sudo hmailserver --set-setting UpdateAutoDownload=1
sudo hmailserver --set-setting 'UpdateWindow=Sun 03:00'
sudo systemctl restart hmailserver
```

and a backup destination, since `UpdateBackupBeforeApply=1`, the default, backs
up before every unattended apply and applies nothing without one - or
`UpdateBackupBeforeApply=0`. Put the destination outside `/var/lib/hmailserver`
and give it to the unit with a `ReadWritePaths` drop-in:

```sh
sudo install -d -o hmailserver -g hmailserver -m 0750 /var/backups/hmailserver
sudo systemctl edit hmailserver      # [Service]  ReadWritePaths=/var/backups/hmailserver
sudo systemctl restart hmailserver
```

then set it on the Deck's Backup page or with `PUT /api/v1/settings/backup`. A
destination under `/var/lib/hmailserver` - the one place the unit lets the
service write without a drop-in - is inside the data directory: up to 6.3.5 a
backup there copied itself into itself and failed every time, and so did every
unattended apply; from 6.3.6 it works, but the backup is then on the disk it
backs up ([Backup.md](Backup.md)). The update looks for the generic name,
`hmailserver_<version>_amd64.deb`; see "The update checker was not changed in
this work" above for the distribution packages.

The trust the package is checked against - `UpdateSigningIdentity` and the four
settings beside it - is read from `/etc/hmailserver/hMailServer.ini` alone, by
the server and by the helper's check as root alike, and set with `sudo
hmailserver --set-setting`; the REST API and the Deck refuse it
([UpdateFeed.md](UpdateFeed.md), "Where the trust is read from"). The helper
runs `apt-get` quietly (`-q`, `Dpkg::Use-Pty=0`), so its journal holds what was
installed and not dpkg's progress, which journald stored as "[614B blob data]"
up to 6.3.5.

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
     [What has run](#what-has-run)). It came back after linix's next reboot,
     on 28 September 2026 at 18:08 UTC, when `systemd-binfmt` registered it at
     boot: the arm64 install jobs of `batch41`'s 2890818010 and 2891564033 and
     of 6.3.5's tag pipeline 2892097590 ran under that registration, and all
     passed.
   - On 29 September 2026 a privileged container whose first process was
     systemd unregistered every handler on linix as it stopped, and each arm64
     job after it failed with "exec format error". Since 6.3.6 each arm64 build
     asks first: `build/ci/arm64-emulation.sh` writes a 132-byte aarch64
     program that exits with 42 and runs it, and refuses the job by name, with
     what to do (`systemctl restart systemd-binfmt` as root on linix), when
     anything else comes back. The install job cannot ask for itself, since its
     container's shell is an aarch64 program that does not start without the
     handler, so the build before it asks. `rpm-build.sh` (aarch64),
     `appimage-cross.sh` and `linux-image.sh` (arm64) ask the same, and so does
     `container-image-start-arm64`, from its own container, before the test of
     its arm64 service.
     `appimage-runtime-aarch64` has no job before it: without the handler it
     still fails with "exec format error", beside `appimage-aarch64-compile`'s
     refusal, which names the cause.
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
   files at (`CARRIED_PACKAGES`). The six whose names carry a release are in
   too (batch42, W2.1): `release-packages` (`.gitlab/ci/release-packages.yml`)
   needs each build with its artifacts and its install job, gathers each
   package held to its build's sums and makes its SBOM, and `release` needs
   `release-packages`. `build/check-release-chain.py` holds that job to every
   build and install job here, and none of them may fail or wait for a press
   in a `v*` tag's pipeline.
3. **The first pipeline that runs the install jobs: done.** The install jobs
   first ran in pipeline 2887524317 on 28 September 2026, and every build and
   install job has passed in each pipeline since that ran them all to an end,
   6.3.6's two included (see [In GitLab's pipelines](#in-gitlabs-pipelines)).
