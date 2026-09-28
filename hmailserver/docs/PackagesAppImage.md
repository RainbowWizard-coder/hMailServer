The AppImages on GitLab CI
==========================

This page covers the AppImage part of putting every hMailServer build on the
project's GitLab CI. It is written to be folded into
[ContinuousIntegration.md](ContinuousIntegration.md) and the README. It says:

- what each job makes and where it runs;
- where every tool comes from (nothing from GitHub);
- what has actually been run, and with what result;
- what the owner still has to do.

An AppImage of hMailServer is a way to try the server out, not a way to run mail.
The header of `hmailserver/source/Server/platform/packaging/build-appimage.sh`
explains why: it runs as the invoking user, keeps its store under `$HOME`, cannot
bind port 25, and still needs a database.

What changed, and why
---------------------

Until 6.3.3, the AppImages were made on GitHub by `build-appimage.sh` with
linuxdeploy and its AppImage plugin. Both were downloaded from their GitHub
releases' moving `continuous` tag, and every AppImage carried the AppImage
project's runtime, also from GitHub. 6.3.4, the first release built off GitHub,
has no AppImages.

An AppImage (type 2) is two things one after the other:

1. **The runtime**, a small ELF program. It finds where its own ELF ends, mounts
   everything after that point read-only with FUSE, and runs the `AppRun` inside.
2. **A SquashFS image** of an AppDir.

Every existing source for the runtime is on GitHub. On 26 September 2026 I
searched the distributions' own package indexes for `appimagetool`,
`appimage-runtime`, `type2-runtime` and AppImageKit:

| Distribution | What it carries | Usable here? |
|---|---|---|
| Debian, Ubuntu | `libappimage` | No: a library for reading AppImages, with no runtime |
| Fedora | `obs-service-appimage` | No: an OBS source service |
| openSUSE Tumbleweed | `appimaged` | No: a desktop daemon |
| Alpine (testing) | `appimagetool` 1.9.1 | No: 74 KiB installed, and it downloads the runtime from GitHub when it runs |

No distribution packages the runtime, and there is no distribution source
package to build it from.

So the project now has a runtime of its own:
`hmailserver/source/Server/platform/packaging/appimage/runtime.c`.

- **Where the code comes from:** it is written against the published AppImage
  type 2 format. Nothing is copied from the AppImage project's `type2-runtime`.
- **How it is linked:** statically, on Alpine, from Alpine's own packages:
  - squashfuse's `libsquashfuse.a` and `libsquashfuse_ll.a` (the SquashFS reader
    and its FUSE operations);
  - `fuse3-static`;
  - the static zstd, lz4, xz, zlib and lzo that Alpine's squashfuse is built
    with.
- **What it provides:**
  - the format's magic bytes, `AI\x02` at offset 8;
  - an empty `.upd_info` section;
  - `--appimage-extract`, `--appimage-extract-and-run`, `--appimage-mount`,
    `--appimage-offset`, `--appimage-version` and `--appimage-help`;
  - `APPIMAGE_EXTRACT_AND_RUN=1`;
  - the environment every AppImage promises: `APPIMAGE`, `APPDIR`, `OWD` and
    `ARGV0`. `APPIMAGE` is what `UpdateChecker::DetectInstallKind` reads to know
    that it is an AppImage.
- **Two differences from the reference runtime:**
  - **No FUSE:** where FUSE is missing (most containers, or no `fusermount3`),
    it says so once and runs from an extracted copy instead of failing.
  - **The runtime does not stay between the supervisor and the server:** it
    execs `AppRun`, so the process a supervisor started is the server itself,
    with its PID, its exit status and its signals. A small forked helper keeps
    the mount, or deletes the extracted copy, once the server and everything it
    started have exited. The helper ignores SIGINT, SIGTERM and SIGHUP, so
    stopping a unit cannot take the files away from a server that is still
    shutting down.
- **What it leaves out:** signatures (`.sha256_sig`) and desktop integration.

`build-appimage.sh` no longer uses linuxdeploy. It now does the same work itself
with distribution tools:

- **Libraries:** it copies the libraries that `ldd` resolves for the server into
  `usr/lib`, except the ones it leaves to the host. It then sets the RUNPATH with
  `patchelf`: `$ORIGIN/../lib` on the server and `$ORIGIN` on each library. It
  then resolves the image's own server again and fails if any library comes from
  outside the image when it was not meant to.
- **The AppDir's root:** it writes `AppRun`, the desktop file, the icon and
  `.DirIcon`.
- **Packing:** it runs `mksquashfs -root-owned -comp zstd` and writes the runtime
  followed by the image.

Its header lists what is left to the host, and why:

- **The AppImage excludelist convention:** glibc and its loader and NSS modules,
  `libgcc_s` and `libz`.
- **`libstdc++`:** the glibc floor already guarantees a newer one on the host.
- **OpenSSL 3, the project's own decision:** its ABI is stable across 3.x, and
  the host's copy is the one its distribution patches. It also reads the host's
  configuration, crypto policy and certificate store.
- **`libmariadb.so.3`:** not linked at all. It is opened with `dlopen` only when
  a MySQL or MariaDB database is configured, exactly as before.

Where it runs
-------------

Both AppImages are built on **Ubuntu 24.04**. That is the oldest release with the
Boost the server needs (1.83), and it means **glibc 2.39**. Debian 12, Ubuntu
22.04, RHEL 9 and openSUSE Leap 15 have an older Boost, so they cannot build the
server, and an older glibc, so they cannot run this AppImage.

By glibc version alone, it should run on:

- Ubuntu 24.04 and later;
- Debian 13;
- Fedora 40 and later;
- RHEL 10 and its rebuilds;
- openSUSE Leap 16 and Tumbleweed.

Both AppImages were run on Ubuntu 24.04, Fedora 44, Debian 13, RHEL 10.2 (the
UBI image), openSUSE Leap 16.0 and openSUSE Tumbleweed, the aarch64 one under
qemu-user, and the x86_64 one also on Ubuntu 26.04 (below). The other releases
have not been tried.

The host also needs `libssl.so.3` and `libcrypto.so.3`. Every one of those
releases ships them in its base system.

**LDAP over TLS off Debian and Ubuntu.** The image bundles Ubuntu 24.04's
`libldap` and the GnuTLS it is built with. Read from the bundled files with
`strings`, they look for `/etc/ldap/ldap.conf` and for the certificates to trust
in `/etc/ssl/certs/ca-certificates.crt`, which are Debian's and Ubuntu's paths.
Fedora 44 has that certificate file, as a link to its own bundle. The RHEL 10
(UBI) and openSUSE Leap 16 and Tumbleweed images have neither file. There, a
directory reached over TLS with `VerifyCertificate=1` is expected to be refused,
because the bundled library finds no certificate to trust. Naming the host's
bundle in the environment should fix it, because OpenLDAP reads `LDAPTLS_CACERT`:
for example `LDAPTLS_CACERT=/etc/pki/tls/certs/ca-bundle.crt` on RHEL, or
`/etc/ssl/ca-bundle.pem` on openSUSE. Neither the refusal nor the fix has been
tried; no LDAP connection has been made from an AppImage.

The jobs
--------

All the jobs are in `.gitlab/ci/packages-appimage.yml`, which `.gitlab-ci.yml`
includes. They all run on linix, the project's own runner, and only for a
protected ref. No job uses GitLab's shared runners, so the jobs cost no compute
minutes.

| Job | Image | Makes |
|---|---|---|
| `appimage-runtime-x86_64` | `alpine:3.24` amd64 | `appimage/runtime-x86_64` and `.txt` (what it was built from, and the licences), after `build/ci/packages/appimage-runtime.sh` proves it on a small AppImage of its own |
| `appimage-runtime-aarch64` | `alpine:3.24` arm64, under qemu-user | `appimage/runtime-aarch64` and `.txt`, proved the same way |
| `appimage-x86_64` | `ubuntu:24.04` amd64 | Builds the server with GCC 13 and makes `hMailServer-<v>-x86_64.AppImage`, then runs it (`appimage-build.sh`, then `appimage-test.sh`) |
| `appimage-aarch64-compile` | `ubuntu:24.04` amd64 | `appimage/hmailserver-aarch64`, cross-compiled against Ubuntu 24.04's own arm64 libraries from ports.ubuntu.com (`appimage-cross.sh`) |
| `appimage-aarch64` | `ubuntu:24.04` arm64, under qemu-user | `hMailServer-<v>-aarch64.AppImage` from that binary (`appimage-build.sh` with `HM_PREBUILT`), then runs it |
| `appimage-test-x86_64`, `appimage-test-aarch64` | `fedora:44` from registry.fedoraproject.org | Nothing. Runs the AppImage on a distribution it was not built on, with nothing that the build installed (`appimage-test.sh`) |

**When the jobs run:**

- **For a protected `v*` tag:** every job runs.
- **For another protected ref (master, `batch*`):** the two runtime jobs and the
  compile job wait to be started by hand, and the rest follow them.
  - For the x86_64 AppImage, start `appimage-runtime-x86_64`.
  - For the aarch64 AppImage, start both `appimage-runtime-aarch64` and
    `appimage-aarch64-compile`: `appimage-aarch64` needs the two.
  - Until then GitLab shows the jobs that follow as skipped, and is expected to
    run them once what they need has been started and has passed. `batch41`'s
    pipelines 2886955494, 2887115157 and 2887524317 (27 and 28 September 2026)
    showed the first half: the three jobs waiting and the rest skipped. None has
    shown the second, because nobody started the three.

**How the images are pinned:** each image is pinned by the digest of its
per-platform manifest, not by the index. The runner pulls `if-not-present`, and
a per-platform digest cannot be satisfied by the other architecture's copy.

**Why the aarch64 server is cross-compiled:** built natively in an arm64
container under qemu-user, it would take hours. Cross-compiled, it takes what the
x86_64 build takes. Only the packing runs under emulation, because it runs `ldd`
and needs the target's loader.

**What `appimage-test.sh` checks:**

1. The runtime's options work, and the SquashFS begins where the runtime ends.
2. Extracted and run, the server prints the version in the file name.
3. `AppRun` has made the per-user tree and a 0600 configuration whose
   `ProgramFolder` points into the running image, and the extracted copy is gone
   afterwards.
4. The image carries the Control Deck and every one of its catalogues, byte for
   byte, as well as:
   - the schema scripts;
   - `dh2048.pem` and `tlds.txt`;
   - the live update's apply step;
   - the licences;
   - `AppRun`, the desktop file and the icon.
5. Where FUSE can be had, the mounted run works too. In a container it usually
   cannot, and the script says so rather than failing.

Under qemu-user: the magic bytes
--------------------------------

An arm64 AppImage cannot be started under qemu-user on an x86-64 host.

- **The cause:** binfmt_misc's registration for `qemu-aarch64` requires bytes 8
  to 15 of the ELF header to be zero, and the AppImage magic is in bytes 8 to 10.
  The kernel does not recognise the file, and the shell tries it as a script.
- **On real hardware:** an arm64 machine ignores those bytes.
- **What the scripts do:** `appimage-runtime.sh` and `appimage-test.sh` detect
  this case: the file will not start, but a copy with those three bytes cleared
  does. They then run their checks on that copy and say so. Every other byte is
  the AppImage's.

What was run, and with what result
----------------------------------

Everything below was run by hand on linix, on 26 September 2026, in containers
from the same images and digests as the jobs, and not by GitLab. **No GitLab
pipeline has run any of these jobs yet.** They were in `batch41`'s pipelines
2886955494 and 2887115157 (27 September 2026), where nobody started the runtime
and compile jobs, which were cancelled with the pipeline without having started,
and GitLab skipped the rest; in 2887524317 the three were still waiting to be
started when this was written, on 28 September 2026 at 00:40 UTC.

### The first runs

These are the runs made while the jobs were written, from this branch's tree.
Three of them may have run an earlier state of the scripts than the one
committed, or ran on an earlier AppImage, as their rows say. The check's runs
below repeat each of the three.

| What | Where | Result |
|---|---|---|
| `appimage-runtime.sh`, x86_64 | `alpine:3.24` amd64 (Alpine 3.24.2) | **Passed.** Links statically. It is 871,016 bytes, and its SHA-256 is `3bfa7753…`. The self-test passed: `--appimage-offset` equals the runtime's size; `--appimage-extract` restores every file, the symbolic link and the modes; `APPIMAGE_EXTRACT_AND_RUN=1` and `--appimage-extract-and-run` pass `APPIMAGE`, `APPDIR`, `ARGV0`, `OWD`, the arguments and `AppRun`'s exit status (7) through, and the copy is removed; the bare runtime refuses. |
| `appimage-runtime.sh`, aarch64 | `alpine:3.24` arm64 under qemu-user | **Passed**, on the copy with the magic cleared (see above). It is 854,408 bytes, and its SHA-256 is `e6a829c5…`. This run, 03:15 to 03:42 UTC, came before the tree was last copied to linix (04:14 UTC), so it may not have run the committed scripts. |
| `appimage-build.sh`, x86_64 | `ubuntu:24.04` amd64 | **Built.** The server compiled with GCC 13.3 in 719 to 757 s at `-j6`. `hMailServer-6.3.4-x86_64.AppImage` is 18,709,096 bytes, with 19 libraries bundled and 8 left to the host. The first two runs found two faults in the scripts, and both are fixed. The final run, with the scripts as committed, passed end to end, including the build's own run of `appimage-test.sh` on Ubuntu 24.04 (glibc 2.39). The image's SHA-256 is `4cf2eb5c…`. Built again from the same packages, the runtime came out byte for byte the same (`3bfa7753…`). |
| `appimage-test.sh` on the x86_64 AppImage | linix itself, Ubuntu 26.04, glibc 2.43, with FUSE | **Passed**, including the mounted run: the server said its version from a FUSE mount. `--appimage-mount` mounted the image, and on SIGTERM the helper unmounted it and removed the mount point. |
| `appimage-test.sh` on the x86_64 AppImage | `fedora:44` amd64, glibc 2.43 | **Passed** (no FUSE in the container). The first run found that a minimal Fedora has no `cmp`, and the script now compares by SHA-256. The passing run, at 04:42 UTC, was on an x86_64 AppImage made before the final one (`4cf2eb5c…`, 07:38 UTC), which it did not run. |
| `appimage-cross.sh` | `ubuntu:24.04` amd64 | **Passed.** Found OpenSSL 3.0.13, Boost 1.83 and libpq 16.15 under `/usr/lib/aarch64-linux-gnu`, and linked an aarch64 PIE with GCC 13.3. The first run was spoiled by the files being rewritten under the compiler, and the second finished in 382 s, reusing most of the first's objects. |
| `appimage-build.sh` with `HM_PREBUILT`, aarch64 | `ubuntu:24.04` arm64 under qemu-user | **Passed.** `hMailServer-6.3.4-aarch64.AppImage` is 17,930,632 bytes, with the same 19 libraries bundled, and its own test passed on the copy with the magic cleared. It ran an earlier `appimage-build.sh` than the one committed: its summary says "built on" where the committed script writes "packed on". |
| `appimage-test.sh` on the aarch64 AppImage | `fedora:44` arm64 under qemu-user | **Passed**, on the copy with the magic cleared. |
| The aarch64 AppImage mounted with FUSE | linix itself, under qemu-user, with the magic cleared | **Partly.** The runtime mounted the image and `AppRun` ran from the mount. The server then could not start, because an x86-64 host has no `/lib/ld-linux-aarch64.so.1`. That is expected, and it is not a fault of the AppImage. |

### The check's runs

The check of this branch ran these on linix on 26 September 2026, from 12:58 UTC.
It worked from a fresh `git archive` of 098bd00f6, with this check's
`appimage-test.sh` (1be44252b) copied over it from 13:33 UTC. Each job's `appimage/`
directory was its own, seeded with the artifacts of the jobs it needs. Every
container had six CPUs and 8 GB, as the runner gives, and ran as root. Every
build took linix's shared lock.

| What | Where | Result |
|---|---|---|
| `appimage-runtime.sh`, x86_64 | `alpine:3.24` amd64 | **Passed.** 871,016 bytes, SHA-256 `3bfa7753…`: the same bytes a third time. |
| `appimage-runtime.sh`, aarch64 | `alpine:3.24` arm64 under qemu-user | **Passed,** on the copy with the magic cleared. 854,408 bytes, SHA-256 `e6a829c5…`: the same bytes as the first run, so that run's runtime was the committed source's. The container ran for 32 s. |
| `appimage-build.sh` with `HM_PREBUILT`, aarch64, with the committed scripts | `ubuntu:24.04` arm64 under qemu-user, with that runtime and the first runs' cross-compiled server, and GitLab's `CI_COMMIT_TIMESTAMP`, `CI_COMMIT_SHA` and `CI_COMMIT_SHORT_SHA` set | **Passed,** including its own run of `appimage-test.sh`. `hMailServer-6.3.4-aarch64.AppImage` is 17,930,632 bytes, as before, with the same 19 libraries bundled and 8 left to the host; SHA-256 `e80068a0…`. That differs from the first run's: the first run had no `SOURCE_DATE_EPOCH`, so `mksquashfs` dated the image with the time it ran, and this one dates it with the commit's. The container ran for 80 s. |
| `appimage-test.sh` on that aarch64 AppImage | `fedora:44` arm64 (`c61b46d9…`); `debian:13`, RHEL 10.2 (UBI 10), openSUSE Tumbleweed and openSUSE Leap 16.0, each the arm64 image of the same index as in the next row, all under qemu-user | **Passed on all five,** on the copy with the magic cleared, without FUSE. |
| `appimage-test.sh` on the final x86_64 AppImage (`4cf2eb5c…`) | `fedora:44` amd64; `debian:13` (index `9cc08002…`); RHEL 10.2, UBI 10 (`223f8b83…`); openSUSE Tumbleweed (`f1c043e8…`, the image built 22 June 2026); openSUSE Leap 16.0 (`5bc9e124…`, built 26 August 2025) | **Passed on all five,** without FUSE. |
| The negative controls | `fedora:44` amd64 with `libssl.so.3` removed, then with `libstdc++.so.6` removed | **Failed, as they must.** The loader could not find the library, and the test stopped with exit 127. |
| `appimage-test.sh` on the final x86_64 AppImage | linix itself, Ubuntu 26.04, with FUSE | **Passed,** including the mounted run. |
| `--appimage-mount`, then SIGTERM | linix itself | **Passed.** It mounted the image and printed the mount point. On SIGTERM the runtime exited (143), and the mount and the mount point were gone. |
| The runtime's `.upd_info` section | `readelf -S` on the final x86_64 AppImage | Present, 1,024 bytes. |
| ShellCheck 0.11.0 (`koalaman/shellcheck:stable`, Docker Hub) | the four scripts and `build-appimage.sh` | **One warning,** `ls \| grep` in `appimage-test.sh`, which now counts with a glob. The rest are notes: `. /etc/os-release` not followed, the intended `'$ORIGIN'`, and two `A && B \|\| C` tests that fail as meant. |
| editorconfig-checker 3.0.3 | the whole tree, as the `editorconfig` job runs it | **Passed.** |

From 16:10 UTC the check went on from a `git archive` of 1be44252b, with
GitLab's `CI_COMMIT_*` variables for that commit, and started nothing while the
release had linix:

| What | Where | Result |
|---|---|---|
| `appimage-cross.sh`, from a clean build directory | `ubuntu:24.04` amd64 | **Passed.** The first run of the compile job that reused no objects. ninja built the server in 825 s at `-j6`, and the container ran for 14 min 18 s, inside the job's hour. It found OpenSSL 3.0.13, Boost 1.83 and libpq 16.15 under `/usr/lib/aarch64-linux-gnu`, and made an aarch64 PIE (SHA-256 `7189eee2…`). |
| The runtime's version with GitLab's short SHA | the laptop, `sh` | **Checked by hand, not built.** With `CI_COMMIT_SHORT_SHA` set, `appimage-runtime.sh` passes `-DHM_RUNTIME_VERSION="1 (1be44252)"` to the compiler as one argument, a string literal. No runtime has been built with that variable set. |

**Not re-run by the check:** the packing of that aarch64 server, the runtime
jobs with GitLab's variables set, and the x86_64 build from a clean build
directory. They were queued behind the release, which took linix at 17:51 UTC,
and had not run when this was written. The x86_64 build of the committed
scripts is the first runs' (07:38 UTC), and the packing with GitLab's variables
is the aarch64 row of the check's runs above.

**Not run:**

- **Any job in a GitLab pipeline** (above). GitLab has created the jobs from
  the definitions in three pipelines, and `build/check-release-chain.py` and
  `build/check-ci-image.py` pass with the include in place.
- **Either AppImage on a real arm64 machine,** including the unmodified aarch64
  file with its magic bytes. The project has no arm64 machine.
- **A server connected to a database or run under a unit.** No step has run the
  server from an AppImage that way.

The names, and the update checker
---------------------------------

The AppImages keep the names that installed servers already look for:
`hMailServer-<version>-x86_64.AppImage` and `hMailServer-<version>-aarch64.AppImage`.

- **Where those names come from:** `UpdateChecker.cpp`'s `PlatformInstallerName_`
  and `generate-update-feed.py`'s `installer_names()` expect them.
- **Why one per architecture is enough:** the AppImage is not per distribution,
  so there is only one.
- **The update checker itself:** unchanged in this work. A distribution-aware
  checker, one that would pick `hmailserver_<v>-1~debian13_arm64.deb` over the
  generic `.deb`, is a later row.

What the owner must do
----------------------

1. **Decide whether the project ships its own AppImage runtime.** No distribution
   packages one, and the AppImage project publishes its own only on GitHub.
   - **To accept:** there is nothing else to do.
   - **To refuse:** the AppImages cannot be built without GitHub, and the jobs
     should be removed. The only other route is a copy of `type2-runtime` pulled
     from GitHub once, by hand, reviewed and hosted in the project's package
     registry. That is a decision about GitHub, not about code.
2. **Keep qemu-user registered on linix across reboots.** Done. The aarch64
   runtime, packing and test jobs need `qemu-aarch64` registered with
   binfmt_misc, and when this page was first written it was registered only by a
   `tonistiigi/binfmt` container, which does not survive a reboot. At 08:13 UTC
   on 26 September 2026, `qemu-user` and `qemu-user-binfmt`
   1:10.2.1+ds-1ubuntu3.2 were installed on linix. Their
   `/usr/lib/binfmt.d/qemu-aarch64.conf` registers `/usr/bin/qemu-aarch64` with
   the flags `OPF`, and `systemd-binfmt` applies it at every boot. The `F` flag
   is what lets a container use the host's interpreter. Without the
   registration, those jobs fail at their first step with "exec format error".
   linix has not been rebooted since, so the registration at boot has not been
   seen.
3. **Let the release collect the AppImages** (the integrator's part):
   - `release` needs `appimage-x86_64` and `appimage-aarch64`, and waits for the
     two test jobs;
   - `build/ci/release-chain.py collect` takes `appimage/hMailServer-*.AppImage`
     from them;
   - the note in `build/ci/release-verify.sh` that no release carries AppImages
     is removed once one does.
4. **Check the licences:** `runtime-<arch>.txt`, which is shipped inside every
   AppImage as `usr/share/hmailserver/licenses/appimage-runtime.txt`, names each
   library linked into the runtime and its licence. They include
   libfuse3 (LGPL-2.1) and lzo (GPL-2.0-or-later), which squashfuse is built
   with on Alpine. The runtime's source and the script that links it are in this
   repository, which is what the LGPL asks for a relink.

For the integrator
------------------

This branch was made from 6399e08a5. By the time it was checked, batch40
(4a7340c98) had changed the same files and documents in the GitLab sweep:

- **`build-appimage.sh` conflicts.** batch40 (37740058e) keeps linuxdeploy but
  refuses to download it. This branch removes linuxdeploy altogether. Take this
  branch's whole file.
- **`packaging/README.md`** changed on both sides. Merge the two: this branch
  rewrites only "The AppImage" paragraph.
- **Documents that describe the AppImage as it was** become wrong once this
  branch is folded. On batch40 they are:
  - `ContinuousIntegration.md`: the "`linux-build.yml` build, the AppImage | not
    ported" row of the port table; "The AppImage. Its build script runs
    linuxdeploy…" in the list of what is not ported; and "What a release
    carries", which says the AppImage tool is published only on GitHub;
  - `ThirdPartyBinaries.md`: "The AppImage tools" under "Not pinned, and why".
    The runtime is now built from `runtime.c` and Alpine's packages, and
    `runtime-<arch>.txt` records each package's version. The Alpine packages
    are not pinned by version, and the page should say so;
  - `CONTRIBUTING.md`: the sentence that `build-appimage.sh` uses linuxdeploy
    from `TOOLS_DIR`.
  They were not changed here, because batch40 had already rewritten each of
  those lines and an edit on 6399e08a5 would only conflict with it.
  `HelpPagesData.cpp` has to be regenerated after the merge in any case.
- **`.gitattributes`.** pk-deb adds the same `build/ci/packages/** text eol=lf`
  line, with a comment; keep one. It also makes pk-windows' `.ps1` files in that
  directory LF, which PowerShell reads the same.
- **`HelpMap.cs`.** This branch adds `PackagesAppImage.md` to `ReferenceOnly`;
  pk-deb and pk-windows add their own pages beside it.
- **Checkers that read `.gitlab-ci.yml` alone.** `build/ci/repo-hygiene.py`'s
  Pinned-Dependencies check and `build/check-release-chain.py` do not follow
  `include:`, so neither sees the images or the jobs in `.gitlab/ci/*.yml`. Every
  image in `packages-appimage.yml` is pinned by digest, but no checker holds it
  to that.
