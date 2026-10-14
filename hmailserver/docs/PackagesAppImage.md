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
- **Its licences:** libfuse3 is under the LGPL-2.1 and lzo under the
  GPL-2.0-or-later, and both ask for their own source to go with the binary. The
  release carries it, as `hmailserver-<version>-appimage-runtime-sources.tar.gz`
  ([Licences](#licences)).
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
  outside the image when it was not meant to. Beside them it puts each library's
  package's copyright file and a list of what it carries ([Licences](#licences)).
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

**Character sets on Fedora, RHEL and openSUSE.** The image carries no glibc -
`build-appimage.sh` leaves it to the host, as it must - so the server converts
mail's text with the host's iconv, and that loads each character set's
converter from the host's glibc. Fedora's and RHEL's glibc keeps all but a few
in `glibc-gconv-extra`, openSUSE's in `glibc-gconv-modules-extra`, which glibc
only recommends: a host without it, as every container image of those
releases is, has UTF-8, US-ASCII, UTF-7, ISO-8859-1, ISO-8859-15 and
Windows-1252, and the server reads and writes no text in BIG5, ISO-2022-JP or
the other Windows and ISO-8859 code pages (PackagesRpm.md, "The character
sets"). The `.rpm` asks for that package; an AppImage cannot ask for anything,
so there install it by hand:

    sudo dnf install glibc-gconv-extra                 # Fedora, RHEL
    sudo zypper install glibc-gconv-modules-extra      # openSUSE

From 6.3.6 the server says when it is missing: `--check-config` ends with a
`Character sets:` line naming what does not convert, and a start writes the
same to standard error. Debian's and Ubuntu's `libc6` has every converter.

The jobs
--------

All the jobs are in `.gitlab/ci/packages-appimage.yml`, which `.gitlab-ci.yml`
includes. They all run on the project's own Linux runners, the three with the tag
`linix` (linix, server2 since 5 October 2026 and s1-linux since 13 October 2026),
and only for a
protected ref. No job uses GitLab's shared runners, so the jobs cost no compute
minutes.

| Job | Image | Makes |
|---|---|---|
| `appimage-runtime-x86_64` | `alpine:3.24` amd64 | `appimage/runtime-x86_64` and `.txt` (what it was built from and links, the licences, and where the source is), after `build/ci/packages/appimage-runtime.sh` proves it on a small AppImage of its own; and `runtime-x86_64-linked.txt`, every file the link took with its Alpine package, version, aports commit and licence ([Licences](#licences)) |
| `appimage-runtime-aarch64` | `alpine:3.24` arm64, under qemu-user | `appimage/runtime-aarch64`, `.txt` and `-linked.txt`, proved the same way |
| `appimage-runtime-sources` | `alpine:3.24` amd64 | `appimage-runtime-sources/hmailserver-<v>-appimage-runtime-sources.tar.gz`, the source of every library the two runtimes link as Alpine built it, made twice and compared, with `SHA256SUMS.txt`; and `licenses-<arch>/`, each library's licence files for that architecture's AppImage (`build/ci/packages/appimage-runtime-sources.sh`) |
| `appimage-x86_64` | `ubuntu:24.04` amd64 | Builds the server with GCC 13 and makes `hMailServer-<v>-x86_64.AppImage`, with the runtime's libraries' licence files, the copyright files of the libraries it bundles, their list and hMailServer's own licence, then runs it (`appimage-build.sh`, then `appimage-test.sh`); and `built-against-x86_64/`, what its SBOM and the bundled libraries' sources are made from |
| `appimage-aarch64-compile` | `ubuntu:24.04` amd64 | `appimage/hmailserver-aarch64`, cross-compiled against Ubuntu 24.04's own arm64 libraries from ports.ubuntu.com (`appimage-cross.sh`) |
| `appimage-aarch64` | `ubuntu:24.04` arm64, under qemu-user | `hMailServer-<v>-aarch64.AppImage` from that binary (`appimage-build.sh` with `HM_PREBUILT`), then runs it |
| `appimage-bundled-sources` | `ubuntu:24.04` amd64 | `appimage-bundled-sources/hmailserver-<v>-appimage-bundled-sources.tar.gz`, the source of every library the two images carry in `usr/lib`, as Ubuntu built it, made twice and compared, with `SHA256SUMS.txt` (`build/ci/packages/appimage-bundled-sources.sh`; [Licences](#licences)) |
| `appimage-test-x86_64`, `appimage-test-aarch64` | `fedora:44` from registry.fedoraproject.org | Nothing. Runs the AppImage on a distribution it was not built on, with nothing that the build installed (`appimage-test.sh`) |

**When the jobs run:**

- **For a protected `v*` tag and a `batch*` branch:** every job runs at once, as
  the release and its rehearsal need them.
- **For `master`:** the two runtime jobs and the compile job wait to be started
  by hand, and the rest follow them.
  - For the x86_64 AppImage, start both runtime jobs: `appimage-x86_64` needs
    `appimage-runtime-sources`, which needs the two.
  - For the aarch64 AppImage, start both runtime jobs and
    `appimage-aarch64-compile`.
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
   - `dh2048.pem`;
   - the live update's apply step;
   - the licences, among them `appimage-runtime.txt`, which must name the
     release file with the runtime's libraries' source, the licence files of
     every library it lists (libfuse3's and lzo's at least), and the GPL-3.0's
     text; hMailServer's own AGPL-3.0 text; the notices of what is compiled into
     the server, `QuickJS.txt`, `Unicode-data.txt` and
     `HTML-named-character-references.txt`, each held to the tree's file by
     SHA-256 ([Licences](#licences)); and `bundled.txt`, which must list
     every library in `usr/lib` and nothing else, and name the release file with
     their source, with the copyright file of each library's package and every
     licence text those files name;
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

**In GitLab's pipelines.** The jobs first ran to an end in 6.3.5's tag pipeline
2892097590 (29 September 2026): both runtimes, the aarch64 compile, both
AppImages and both tests on Fedora 44 passed, though the 6.3.5 release did not
collect the images. In `batch42`'s pipelines 2896082335 and 2897966924
(30 September), where the jobs start by themselves, every one of them passed,
with `appimage-runtime-sources` and `appimage-bundled-sources` among them, each
making its tarball twice to the same bytes; each image carried its 19 bundled
libraries with their licences, and each test found the Control Deck and its 20
catalogues byte for byte, the AGPL-3.0 text, `bundled.txt` and the copyright
files and 13 licence texts it names, and ran the image on Fedora 44, the aarch64
one under qemu-user. In 2897966924 the release's rehearsal collected both images
and both sources tarballs with the rest. Before that, `batch41`'s pipelines
2886955494 and 2887115157 (27 September) carried the jobs, where nobody started
the runtime and compile jobs, which were cancelled with the pipeline without
having started, and GitLab skipped the rest.

Everything below was run by hand on linix, on 26 September 2026 (the runtime's
sources on the laptop, on 28 September, and the bundled libraries' licences and
sources there on 29 September), in containers from the same images and
digests as the jobs, and not by GitLab.

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

### The runtime's sources

Run on the laptop on 28 September 2026, in WSL's Docker, from this branch's tree,
in one `alpine:3.24` amd64 container at a time, of the digest
`appimage-runtime-x86_64` and `appimage-runtime-sources` name (Alpine 3.24.2),
each removed afterwards:

| What | Result |
|---|---|
| `appimage-runtime.sh`, x86_64, with the link's map and its record | **Passed.** The runtime linked and passed its self-test as before: 871,016 bytes, SHA-256 `3c004d2c…` (Alpine's packages have moved since 26 September). The record names 13 files: musl's `libc.a`, `rcrt1.o`, `crti.o` and `crtn.o`; `libfuse3.a`; `libsquashfuse_ll.a`; `libzstd.a`, `liblz4.a`, `liblzma.a`, `libz.a` and `liblzo2.a`; and GCC's `crtbeginS.o` and `crtendS.o`, each with the package, version, aports commit and licence of the table above. `libsquashfuse.a`, `libgcc.a` and `libgcc_eh.a` were opened and gave the link nothing. |
| The same script without `-Wl,-Map` and the record | **The same runtime, byte for byte:** the map changes nothing the link writes. |
| `appimage-runtime-sources.sh pack`, twice, the second from nothing in another directory under umask 0077 | **The same bytes:** 10,737,997 bytes, SHA-256 `bbf3e265…`, for Version.h's 6.3.5. Eight libraries; the aports directories fetched from gitlab.alpinelinux.org by their commits and the eight upstream archives from `distfiles.alpinelinux.org/distfiles/v3.24/`. The aarch64 record was the x86_64 one with the architecture changed: the v3.24 aarch64 `APKINDEX` gave the same versions and commits for all nine packages that day. |
| The tarball, read back with Python's `tarfile` on Windows | **As it should be:** 41 entries in order, each owned by 0:0, dated `SOURCE_DATE_EPOCH`, 0644 or 0755; every one of the 20 sources the eight `APKBUILD`s list present with the SHA-512 its `APKBUILD` gives, and no other file but the `APKBUILD`s; `SHA512SUMS` right for every other file; five `APKBUILD`s compared with the git blob ids `ls-tree` gave for their commits, all the same. |
| `pack` given a record it must refuse | **Refused,** each naming why: a library it does not know (origin `lzo2`), an origin that would leave the archive's directory (`../lzo`), and a version (2.10-r4) that aports' `APKBUILD` at the recorded commit does not build. |
| gcc's source, which `README.txt` says is on the same mirror | `gcc-15.2.0.tar.xz`, the archive aports' `main/gcc` names at the recorded commit, answers 200 at `distfiles.alpinelinux.org/distfiles/v3.24/`. |
| `build-appimage.sh`'s `--runtime-licenses` and `appimage-test.sh`'s new checks, each block run on its own under WSL's bash, on the licences and notice of the rows above | **Passed:** the eleven licence files installed under `usr/share/hmailserver/licenses/appimage-runtime/`, and the checks passed on that tree; an empty licence directory was refused, and so was a tree without lzo's or zstd's licence files or with a notice that does not name the release file. |

**Not run there for the sources:** the aarch64 runtime job's own record (no
qemu-user on the laptop), a whole AppImage build with `--runtime-licenses` and
`appimage-test.sh` on a real image, and the jobs in a pipeline. All three have
run since, in `batch42`'s pipelines of 30 September 2026 ([In GitLab's
pipelines](#what-was-run-and-with-what-result)).

### The bundled libraries' licences and sources

Run on the laptop on 29 September 2026, in WSL's Docker, from this branch's tree
(Version.h's 6.3.5), in one container at a time: `ubuntu:24.04` amd64 of the digest
`appimage-x86_64` and `appimage-bundled-sources` name (`49675449…`, Ubuntu 24.04.5),
then `fedora:44` amd64 of the digest `appimage-test-x86_64` names. The runtime and
its licence files were those the runtime's sources' check made on 28 September
(`3c004d2c…`), from the same `runtime.c` and scripts.

| What | Result |
|---|---|
| The server, built in that container as `appimage-build.sh` builds it (GCC 13.3, ninja at `-j2`) | **Built** in 2,698 s. |
| `appimage-build.sh` on that tree | **Passed end to end,** its own `appimage-test.sh` included. `hMailServer-6.3.5-x86_64.AppImage` is 19,303,016 bytes, SHA-256 `874b7c3e…`, with the same 19 libraries bundled and 8 left to the host; in `usr/share/hmailserver/licenses/`: `AGPL-3.0.txt` (`0d96a4ff…`, the FSF's file), `bundled.txt` listing the 19 with their 18 packages and 14 source packages, 18 copyright files, and 12 licence texts (Apache-2.0, Artistic, GFDL-1.2 and 1.3, GPL-1, 2 and 3, LGPL-2, 2.1 and 3, and the links `GPL` and `LGPL`). `built-against-x86_64/` holds `bundled.txt` with the source packages, `runtime-linked.txt` and `archive.txt`; `appimage-bundled-sources.sh check` passed on it. |
| `build-appimage.sh` again, from another build directory | **The same image, byte for byte** (`874b7c3e…`). |
| `appimage-test.sh` on that image in `fedora:44` | **Passed,** the new checks with it. |
| `appimage-test.sh` on nine copies of the image, each changed one way and packed again with the same runtime | **Each refused, naming it:** a package's copyright file gone; a library missing from `bundled.txt`; `bundled.txt` naming a library the image lacks; `bundled.txt` naming another release's sources; a licence text a copyright file names gone; the LGPL-3.0 without the GPL-3.0; `AGPL-3.0.txt` gone; the GPL-3.0's text in its place; `bundled.txt` gone. The unchanged copy passed. |
| `build-appimage.sh` with the container changed for one run each | **As meant:** without `libffi8`'s copyright file, refused naming it; without `/usr/share/common-licenses/Artistic`, which a copyright file names, refused naming it; without `dpkg-query`, made with a warning, and `appimage-test.sh` refused that image for its missing `bundled.txt`. |
| The image's SBOM, `asset-sbom.py make --kind appimage` on Windows with `native-dependencies.ps1`'s list and the image's `built-against-x86_64/` | **Made and verified:** 36 components, the 8 of the server's own list, the 19 bundled libraries by Ubuntu package with their source package, and the 9 Alpine packages the runtime links (`fuse3-static`, `lzo-dev`, `musl-dev`, `squashfuse-dev`, `zstd-static`, `lz4-static`, `xz-static`, `zlib-static` and `libgcc-static`) with commit, licence and files. |
| `appimage-bundled-sources.sh pack` on that record and the same record relabelled `aarch64` (ports.ubuntu.com's arm64 indexes gave every one of the 18 packages at the same version that day), twice, the second under umask 0077 in another directory | **The same bytes:** 75,319,738 bytes, SHA-256 `40df2d53…`; 14 source packages, 48 files, every one from `archive.ubuntu.com` and held to the `Sources` index of a signed `InRelease` and to its `.dsc`; about 25 s a run. Read back with Python on Windows: 69 entries in order, 0:0, dated `SOURCE_DATE_EPOCH`, 0644 and 0755; `SHA256SUMS` right for all 53 other files; the 34 files the 14 `.dsc`s list held to them; and all 48 source files equal to what the archive's `Sources` indexes, fetched again there, give. |
| `pack` given what it must refuse, or cannot get | **Refused,** each naming why: a source package whose licence it does not name (`libffi-ng`), a line without its source package, two records of one architecture, a version neither the archive nor its snapshot indexes, an `InRelease` checked against another keyring (Ubuntu's cloud image key), a `.dsc` with one byte more than its index gives, the archive out of reach. `gpgv` refused an `InRelease` with one line of its text changed. |
| `pack` given a version the archive has dropped (`gnutls28` 3.8.3-1.1ubuntu3.4) with a recorded time of 1 January 2026 | **Packed** from `snapshot.ubuntu.com/ubuntu/20260101T000000Z/`. The snapshot service answered a burst of requests with 503 once; the fetch now retries with a growing wait. |
| GitHub from that container | Unreachable, as `.no-github` makes it in the job. |

**Not run there for the bundled libraries:** the aarch64 AppImage (no qemu-user
on the laptop: its record was the x86_64 one relabelled), and the jobs in a
pipeline. Both have run since, in `batch42`'s pipelines of 30 September 2026,
from each architecture's own record ([In GitLab's
pipelines](#what-was-run-and-with-what-result)).

### The re-check of the bundled libraries' licences and sources

Run on the laptop on 29 September 2026, after the above, in the same images by
digest, one container at a time. What it found and changed:

| What | Result |
|---|---|
| The pinned `ubuntu:24.04` as pulled (its build of 11 September 2026) against what `appimage-build.sh` installs | Nine of the fourteen source packages - `gnutls28`, `nettle`, `gmp`, `libidn2`, `libunistring`, `libtasn1-6`, `p11-kit`, `libffi` and `e2fsprogs` - come from the image as it was built: `apt-get install` does not change a package it already has. The records named no time the archive's snapshot still indexed such a version once the archive superseded it, so the first security update of any of them after the image's build would have failed the job until the image was bumped. **Fixed:** `archive.txt` records when dpkg installed each package, and `pack` looks there first. |
| `pack` on a record of `gnutls28` 3.8.3-1.1ubuntu3.4, which the archive has dropped, with dpkg's time of 1 January 2026 and the job's times of 29 September | **Packed** from the snapshot of 1 January, and `README.txt` names that snapshot as the place. The script as it was, on the same record: refused, having looked only at 29 September's snapshots. Without the `installed` line: refused, naming it. |
| `appimage-build.sh` end to end with `HM_PREBUILT` (the server compiled above), with the fixes | **Passed,** its `appimage-test.sh` included: `archive.txt` gives each of the 18 packages its time (the ten the base had, of those nine source packages, 11 September; the others that minute), and the image carries 13 licence texts, MPL-1.1 now among them. |
| `appimage-test.sh` in `fedora:44` | The new image **passed**; the image made before the fix **refused** (libffi8's copyright file names MPL-1.1 in braces, and that image does not carry it); the new image repacked without MPL-1.1, or with a copyright file naming a text in braces the image lacks, **refused**; repacked unchanged, **passed**. |
| `pack` on the new records (the x86_64 one, and it relabelled `aarch64`), twice, the second under umask 0077 elsewhere | **The same bytes,** 75,320,239; all fourteen from the archive. Read back: 69 entries in name order, 0:0, one date, 0644 and 0755, gzip with no name or time. |
| `gpgv --keyring <Ubuntu's> --output` as `pack` calls it, on the archive's `InRelease` and on changed copies | The output is exactly the signed text. Unsigned lines before the armour or after the signature: accepted, and left out of the output, which is what `pack` reads. A second message after the first, altered, with the first's signature: refused. With keys of the test's own: a key not in the keyring, refused; two messages, both signed by a key of the keyring, refused; a key of the keyring since expired, accepted for a signature made before (Ubuntu's three archive keys in 24.04's `ubuntu-keyring` never expire). |
| The README's changed-library route, on the image | **Works:** `libkeyutils` built from the packed `keyutils` source with one change (a constructor that says so), put at `usr/lib/libkeyutils.so.1` of the extracted image with `patchelf --set-rpath '$ORIGIN'`: `AppRun --version` loads it (`LD_DEBUG=libs`: from `usr/lib`); packed again behind the release's runtime, the new AppImage runs it too, extracted and with `--appimage-extract-and-run`. |
| The licences, from the copyright files and the packed sources | As the table above says, with three corrections: GnuTLS's library carries LGPL-3.0-or-later OR GPL-2.0-or-later code; Debian's `libidn2` copyright file gives `lib/*` the GPL-3.0-or-later where `lib/*.c` say LGPL-3.0-or-later OR GPL-2.0-or-later; `libcom_err`'s `lib/et` is MIT-style by its files (`com_right.c` BSD, `vfprintf.c` not in its build). `libidn2`'s own gnulib holds the GPL-3.0-or-later `progname` module, which its programs use: its message is in no library in `usr/lib`. Nothing bundled is GPL-only. |
| The image's SBOM (`asset-sbom.py make --kind appimage` on the builder's image and records) | **Valid** against CycloneDX 1.6's JSON schema, strict (`cyclonedx-python-lib` 11.12.0); every purl parses (`packageurl-python`): `pkg:apk/alpine/<package>@<version>?arch=...&distro=alpine-3.24.2` for the runtime's nine. |

**Not run:**

- **The jobs in a release that carries the images.** Every job has passed in
  pipelines - 6.3.5's tag pipeline, which did not collect the images, and
  `batch42`'s two of 30 September, whose rehearsal did (above) - but no
  published release carries them yet.
- **Either AppImage on a real arm64 machine,** including the unmodified aarch64
  file with its magic bytes. The project has no arm64 machine.
- **A server connected to a database or run under a unit.** No step has run the
  server from an AppImage that way.

Licences
--------

### The runtime

The runtime is linked statically, so what the link takes from Alpine's packages
goes out inside every AppImage, in the runtime in front of the image.
`appimage-runtime.sh` records what the link took, from the linker's map: each
archive a member was taken from and each object file loaded, with the Alpine
package that owns it and that package's version, origin, aports commit and
licence from apk's own database (`appimage/runtime-<arch>-linked.txt`, written by
`build/ci/packages/appimage-runtime-sources.sh record`). A file no package owns,
other than the compiler's own objects of `runtime.c`, or a library the script does
not name, fails the job, so a library the runtime starts to link is looked at
before an AppImage carries it. On Alpine 3.24.2, x86_64, on 28 September 2026:

| What the runtime links | Alpine package | Licence | What that licence asks of a binary |
|---|---|---|---|
| `libfuse3.a`, libfuse 3.18.3 | `fuse3-static` 3.18.3-r0 | LGPL-2.1-only (the library; its programs, not linked, are GPL-2.0) | The library's source and what it takes to link the runtime again with a changed copy (section 6); the licence's text |
| `liblzo2.a`, lzo 2.10 | `lzo-dev` 2.10-r5 | GPL-2.0-or-later | The complete source of the work it is in (section 3); the licence's text |
| `libsquashfuse_ll.a`, squashfuse 0.6.1 | `squashfuse-dev` 0.6.1-r1 | BSD-2-Clause | The notice |
| `libzstd.a`, zstd 1.5.7 | `zstd-static` 1.5.7-r2 | BSD-3-Clause OR GPL-2.0, taken under BSD-3-Clause | The notice |
| `liblz4.a`, lz4 1.10.0 | `lz4-static` 1.10.0-r1 | BSD-2-Clause (the library; its programs, not linked, are GPL-2.0) | The notice |
| `liblzma.a`, xz 5.8.4 | `xz-static` 5.8.4-r0 | 0BSD | Nothing |
| `libz.a`, zlib 1.3.2 | `zlib-static` 1.3.2-r0 | Zlib | Nothing, for a binary |
| `libc.a`, `rcrt1.o`, `crti.o`, `crtn.o`, musl 1.2.6 | `musl-dev` 1.2.6-r2 | MIT | The notice |
| `crtbeginS.o`, `crtendS.o`, GCC 15.2.0 | `libgcc-static` 15.2.0-r5 | GPL-3.0-or-later WITH GCC-exception-3.1 | Nothing: the exception lets a program GCC compiled be conveyed on terms of its own |

The aarch64 runtime's record is its own job's; any file it adds is recorded the
same way, and one from a library the script does not name fails that job.

**Which ask for their source.** By their own terms, two: libfuse3 (LGPL-2.1,
section 6) and lzo (GPL-2.0-or-later, section 3). Because lzo is under the GPL, the
runtime is a work based on it. It is conveyed under the GPL-3.0 that lzo's "or
later" allows, since `runtime.c` is AGPL-3.0-or-later (section 13 of each lets the
two be combined), and that licence's Corresponding Source is the source of the
whole runtime: `runtime.c`, the script that links it, and every library in it but
the System Libraries (section 1). So squashfuse, zstd, lz4, xz and zlib, whose own
licences ask only for their notice or for nothing, go with it too, and so does
musl, which the runtime carries onto systems whose C library is glibc. GCC's
start-up files and `libgcc` are the compiler's System Libraries, and the GCC
Runtime Library Exception asks nothing of a program compiled with GCC: their source
is not in the release, and its `README.txt` says where Alpine's is.

**Where each is:**

- **`runtime.c` and `appimage-runtime.sh`:** in this repository and in the
  release's source tarball, `hmailserver-<version>.tar.gz`.
- **Every library linked into the runtime, exactly as Alpine built it:** the
  release file `hmailserver-<version>-appimage-runtime-sources.tar.gz`, made by
  the `appimage-runtime-sources` job (`appimage-runtime-sources.sh pack`) from both
  runtime jobs' records. For each library at each version and commit they name:
  - Alpine's aports directory at that commit, with its `APKBUILD` and patches,
    fetched from Alpine's GitLab by the commit's id, so that git checks every
    file against the commit;
  - every upstream archive the `APKBUILD` lists, from Alpine's distfiles mirror
    (`distfiles.alpinelinux.org/distfiles/<branch>/`, or `edge/` when the
    branch's has none), never from the address the `APKBUILD` names, which for
    libfuse, squashfuse, zstd, lz4 and xz is GitHub's; no redirect is followed,
    and the job keeps GitHub out of itself (`.no-github`). A download whose
    connection fails, times out or is dropped, or that is answered 408, 429 or
    5xx, is tried again with a growing wait for up to ten minutes (a 404 is
    not: it sends the job to `edge/`), and the aports fetch and `apk add` are
    tried three times over a minute and a half, so that a short outage of
    Alpine's hosts no longer fails a tag's pipeline;
  - each source file held to the SHA-512 the `APKBUILD` gives it, the `APKBUILD`
    read as `abuild` reads it for each architecture that linked the library, and
    its version held to the package installed.

  With them go `README.txt`, which names each library, its version, licence and
  commit and where each file came from, says what each licence asks, how to build
  a library again as Alpine did, and how to link the runtime again with a changed
  library and put it in front of a release's image; the two records, as
  `linked-<arch>.txt`; and `SHA512SUMS`. It is packed reproducibly: sorted, every
  entry dated `SOURCE_DATE_EPOCH` (the commit's time), owned by 0:0, modes 0644
  and 0755, GNU tar's own format, `gzip -n -9`; the job makes it again from
  nothing, in another directory under another umask, and compares the bytes. The
  release carries it with a line in the sums, a bundle and a `.sigstore.json`
  copy, named in the provenance, and without an SBOM, being source.
- **The licence texts and notices:** every AppImage carries each library's licence
  files, from its upstream archive, in
  `usr/share/hmailserver/licenses/appimage-runtime/<library>/` (the job's
  `licenses-<arch>/`, which `appimage-<arch>` takes and
  `build-appimage.sh --runtime-licenses` installs), beside
  `usr/share/hmailserver/licenses/appimage-runtime.txt` (`runtime-<arch>.txt`),
  which lists what the runtime links, and names the release file, its address in
  the package registry and on the release's page; and the GPL-3.0's own text, as
  `usr/share/hmailserver/licenses/appimage-runtime/GPL-3.0.txt`, which the
  runtime is conveyed under with lzo (GPLv3 section 4 asks for a copy to go with
  the program; lzo's own `COPYING` is the GPL-2.0's). `build-appimage.sh`
  installs it from `platform/packaging/appimage/GPL-3.0.txt`, the FSF's text
  as Ubuntu's `base-files` and xz's upstream archive carry it (SHA-256
  `3972dc97…`). `appimage-test.sh` requires all three.

The source goes out beside the binaries, from the same place, which is what the
GPL-3.0's section 6(d) asks, and must stay there as long as the AppImages do:
never delete it from the package `hmailserver/<version>` while that release's
AppImages are offered.

### The libraries in `usr/lib`

`build-appimage.sh` copies into the image's `usr/lib` every library `ldd`
resolves for the server but those it leaves to the host (above), from the Ubuntu
24.04 packages the job's container installed: libpq and libldap, which the server
links, and what they need. Each goes out under its own licence. On 29 September
2026, x86_64 (ports.ubuntu.com had the same versions for arm64 that day):

| Libraries | Ubuntu packages | Source package and version | Licence of the library, as its copyright file and sources give it | What it asks of a binary |
|---|---|---|---|---|
| `libgnutls.so.30` | `libgnutls30t64` | `gnutls28` 3.8.3-1.1ubuntu3.6 | LGPL-2.1-or-later, with parts under LGPL-3.0-or-later OR GPL-2.0-or-later (`lib/nettle`'s backported, GOST and FIPS code, by the copyright file's own stanzas), so the library as built goes out under the LGPL-3.0-or-later, as upstream's README says of its binaries, which link GMP; CRYPTOGAMS (BSD-style, or the GPL) for `lib/accelerated/x86` (the programs and `libgnutls-openssl`, not bundled, are GPL-3.0-or-later) | The library's source, offered from the same place as the binary (the LGPL-2.1's sections 4 and 6, the GPL-3.0's section 6); the LGPL-2.1's, the LGPL-3.0's and the GPL-3.0's texts; that a changed copy can be used (section 6, 4(d)) |
| `libtasn1.so.6` | `libtasn1-6` | `libtasn1-6` 4.19.0-3ubuntu0.24.04.2 | LGPL-2.1-or-later (its programs are GPL-3.0-or-later) | The same |
| `libkeyutils.so.1` | `libkeyutils1` | `keyutils` 1.6.3-3build1 | LGPL-2.0-or-later (its programs are GPL-2.0-or-later) | The same |
| `libnettle.so.8`, `libhogweed.so.6` | `libnettle8t64`, `libhogweed6t64` | `nettle` 3.9.1-2.2build1.1 | LGPL-3.0-or-later OR GPL-2.0-or-later, taken under the LGPL-3.0 | The source (the GPL-3.0's section 6, which the LGPL-3.0 applies); the LGPL-3.0's and the GPL-3.0's texts (section 4(b)); that a changed copy can be used (4(d)) |
| `libgmp.so.10` | `libgmp10` | `gmp` 2:6.3.0+dfsg-2ubuntu6.1 | The same | The same |
| `libidn2.so.0` | `libidn2-0` | `libidn2` 2.3.7-2build1.1 | The same, by `lib/*.c`'s own notices and upstream's README (Debian's copyright file has a stanza giving `lib/*` the GPL-3.0-or-later, which those notices do not bear out; its programs are GPL-3.0-or-later) | The same |
| `libunistring.so.5` | `libunistring5` | `libunistring` 1.1-2build1.1 | The same | The same |
| `libpq.so.5` | `libpq5` | `postgresql-16` 16.15-0ubuntu0.24.04.1 | PostgreSQL | The notice |
| `libldap.so.2`, `liblber.so.2` | `libldap2` | `openldap` 2.6.10+dfsg-0ubuntu0.24.04.1 | OLDAP-2.8 | The notice |
| `libgssapi_krb5.so.2`, `libkrb5.so.3`, `libk5crypto.so.3`, `libkrb5support.so.0` | `libgssapi-krb5-2`, `libkrb5-3`, `libk5crypto3`, `libkrb5support0` | `krb5` 1.20.1-6ubuntu2.10 | MIT and other BSD-style notices (a few files BSD-3-Clause OR GPL-2.0-or-later, at the recipient's choice) | The notices |
| `libsasl2.so.2` | `libsasl2-2` | `cyrus-sasl2` 2.1.28+dfsg1-5ubuntu3.1 | BSD-3-Clause-Attribution (Carnegie Mellon University's), RSA Data Security's MD5 notice (`lib/md5.c`) and other BSD-style licences | The notices, with CMU's acknowledgement |
| `libp11-kit.so.0` | `libp11-kit0` | `p11-kit` 0.25.3-4ubuntu2.2 | BSD-3-Clause | The notice |
| `libffi.so.8` | `libffi8` | `libffi` 3.4.6-1build1 | MIT | The notice |
| `libcom_err.so.2` | `libcom-err2` | `e2fsprogs` 1.47.0-2.4~exp1ubuntu4.1 | MIT-style: `lib/et`'s own notices and upstream's `NOTICE` (Debian's copyright file lists it under a catch-all GPL-2 stanza that it calls a simplification) | Nothing for a binary |

What `build-appimage.sh` puts in the image, where `dpkg` is (in the jobs, Ubuntu
24.04), in `usr/share/hmailserver/licenses/`:

- `bundled/<package>/copyright` for each package a library came from: the
  copyright file as Ubuntu ships it (`/usr/share/doc/<package>/copyright`, which
  Ubuntu's container image keeps when it drops the rest of `/usr/share/doc`);
- `bundled/common-licenses/<name>`: every licence text those files name by its
  place in `/usr/share/common-licenses`, one at a time or several in the shell's
  braces (libffi8's `/usr/share/common-licenses/{MPL-1.1,GPL-2,LGPL-2.1}`) - in
  24.04's: Apache-2.0, Artistic, GFDL-1.2 and GFDL-1.3, GPL-1, GPL-2 and GPL-3,
  LGPL-2, LGPL-2.1 and LGPL-3, and MPL-1.1, with `GPL` and `LGPL` as the links
  Ubuntu has - and the GPL-3.0 whenever the LGPL-3.0
  is there, since the LGPL-3.0 is written as permissions added to it. A name the
  machine does not have stops the build;
- `bundled.txt`: each library as `library usr/lib/<soname> <package>:<arch>
  <version> <source> <source version>`, and the release file with their source,
  `sources hmailserver-<version>-appimage-bundled-sources.tar.gz`, with its address
  in the package registry and on the release's page.

`appimage-build.sh` keeps the same list, with the SHA-256 of each package's file,
as `built-against-<arch>/bundled.txt`, for the SBOM and the sources job, and
`archive.txt`, when its apt read the archive, the date of each index it read, and
when its dpkg installed each package a library came from (the time of dpkg's file
list of it): a package the image's base already had is at the version it had when
the base was made, which the archive may have superseded before the job ran.
It then runs `appimage-bundled-sources.sh check`, which fails the image's own job
for a source package whose licence that script does not name: a library the image
starts to carry is looked at before a release carries it.

**Which ask for their source, and what goes out.** By their own terms, seven:
libtasn1 and keyutils (LGPL-2.x); GnuTLS (LGPL-2.1-or-later, with parts under
LGPL-3.0-or-later OR GPL-2.0-or-later, so the LGPL-3.0 as built); and Nettle, GMP,
libidn2 and libunistring (LGPL-3.0, of LGPL-3.0-or-later OR GPL-2.0-or-later). Each is conveyed as a shared
library the server loads through its RUNPATH, the "suitable shared library
mechanism" of the LGPL-2.1's section 6(b) and the LGPL-3.0's 4(d)(1): the source
goes out beside the binary, from the same place (section 4 and 6(d) of the
LGPL-2.1, 6(d) of the GPL-3.0), and a changed copy of a library is used when it
is put at `usr/lib/<soname>` of an extracted image (the file's `README.txt` says
how, and how to make an AppImage of it again). The other seven ask only for their
notices, which the copyright files carry. The release file carries the source of
all fourteen all the same:

- the copyright files of libffi and cyrus-sasl2 give GPL terms to parts of their
  Debian packaging, and e2fsprogs' to every file it does not name, and whether a
  patch reaches a library is a judgement the release should not rest on;
- libpq and libldap are what the server links for its database and directory,
  which some read as part of the AGPL's Corresponding Source when they are
  conveyed with it;
- the cost is size, below.

**How large, and why one file.** 75,320,239 bytes for the fourteen source
packages on 29 September 2026. PostgreSQL's is 33 MB of it (`postgresql-16`'s
upstream archive), e2fsprogs' 9.7 MB and krb5's 8.8 MB; the seven the LGPL asks
for are 17.5 MB. Source packages are the same for every architecture, and both
images take the same versions, so the release carries one file for both: one per
architecture would carry the same bytes twice. A version only one architecture
took is packed beside the other.

**Where the source comes from, and how it is held.** The
`appimage-bundled-sources` job (`build/ci/packages/appimage-bundled-sources.sh
pack`, in the `ubuntu:24.04` image `appimage-x86_64` runs in, by digest) reads both
images' records and, for each source package at each version they name:

- takes it from Ubuntu's archive (`https://archive.ubuntu.com/ubuntu/`), or, for
  a version the archive no longer indexes, from Ubuntu's snapshot service
  (`https://snapshot.ubuntu.com/ubuntu/<time>/`) at a time the image's build
  recorded: first when dpkg installed a package built from that version, which is
  the case of a package the pinned image's base had (on 29 September 2026 nine of
  the fourteen source packages were at the versions of the image's own build, 11
  September, which `apt-get install` does not change), then when the job's apt read
  the archive and its indexes' dates; nothing from anywhere else and no redirect
  followed, and the job keeps GitHub out of itself (`.no-github`). `README.txt`
  names, for each package, the place it was taken from;
- holds every file to one chain: the `InRelease` of `noble`, `noble-updates` and
  `noble-security`, verified with `gpgv` against Ubuntu's archive keys
  (`ubuntu-keyring`'s `/usr/share/keyrings/ubuntu-archive-keyring.gpg` in the
  pinned image); each `Sources` index, held to the SHA-256 and size its
  `InRelease` gives; the `.dsc` and every other file of the source package, held
  to the SHA-256 and size the `Sources` index gives; and every file the `.dsc`
  lists, held again to the `.dsc`, which must name the package and version the
  record does.

With them go `README.txt` (each package, the libraries each image took from it,
its licence and what that asks, each file's SHA-256 and where it came from, how to
build a package again as Ubuntu did, and how to run an AppImage with a changed
library), the images' records as `bundled-<arch>.txt` and `archive-<arch>.txt`,
and `SHA256SUMS`. It is packed reproducibly, as the runtime's sources are, and the
job makes it again from nothing, in another directory under another umask, and
compares the bytes. The release carries it with a line in the sums, a bundle and
a `.sigstore.json` copy, named in the provenance, and without an SBOM, being
source. Like the runtime's sources, it must stay on the release as long as the
AppImages do.

### hMailServer's own licence

hMailServer is under the AGPL-3.0-or-later, whose section 4 asks for a copy of
the licence to go with every copy of the program. Each image carries the
repository's `LICENSE`, the FSF's text, as
`usr/share/hmailserver/licenses/AGPL-3.0.txt`, without the carriage returns
`.gitattributes` checks it out with, which makes it the FSF's file byte for byte
(SHA-256 `0d96a4ff…`); `appimage-test.sh` requires it. Its source is the
release's source tarball, `hmailserver-<version>.tar.gz`.

### The notices of what is compiled into the server

Three third-party components compiled into the server are under licences that
ask for their notice with every copy: QuickJS (MIT); Unicode's data for UTS 46,
which the server's IDNA tables are made from (the Unicode License v3, whose
notice must go with every copy of the data or be in its documentation); and
HTML's list of named character references, which its link reader's table is made
from (the BSD 3-Clause License, whose second condition asks a binary to carry the
notice, the conditions and the disclaimer in its documentation or other materials).
Each image carries the three in `usr/share/hmailserver/licenses/`, beside
`AGPL-3.0.txt`, at the paths the `.deb` and the `.rpm` install them to:

| File | Its source in the tree | Licence |
|---|---|---|
| `QuickJS.txt` | `hmailserver/source/Server/quickjs/LICENSE` | MIT |
| `Unicode-data.txt` | `hmailserver/source/Server/Common/Idna/ucd/unicode.LICENSE` | Unicode-3.0 |
| `HTML-named-character-references.txt` | `hmailserver/source/Server/Common/AntiSpam/whatwg/entities.LICENSE` | BSD-3-Clause |

`build-appimage.sh` copies them, since it runs no install rule, and
`appimage-test.sh` holds each to the tree's file by SHA-256, QuickJS's with the
carriage returns a checkout gives it taken off both sides. The last two came in
6.3.7, with the two tables they are the notices of.

**Run on server2 on 14 October 2026.** The x86_64 image built as its jobs build
it - the runtime in `alpine:3.24`, its libraries' licence files packed by
`appimage-runtime-sources.sh`, the server compiled with GCC 13 in `ubuntu:24.04`
by `appimage-build.sh` - carries the three notices, each the tree's file, and
passed `appimage-test.sh` there and in `fedora:44`. An image packed with the base's
`build-appimage.sh` (`batch48`'s `6e1a336c5`) was refused: "The image has no
usr/share/hmailserver/licenses/Unicode-data.txt." So were copies of the new image
with a line added to each notice in turn, each naming the file that is not the
tree's, and one without `HTML-named-character-references.txt`; the unchanged image,
packed again, passed.

### In the SBOM

Each AppImage's SBOM (`<image>.cdx.json`, `build/asset-sbom.py --kind appimage`)
lists every library in `usr/lib` by its Ubuntu package and version, with the
source package and version it was built from and its copyright file's place in
the image; and every Alpine package whose files the runtime links statically
(`--runtime-linked`: the runtime job's record, which `appimage-build.sh` keeps as
`built-against-<arch>/runtime-linked.txt`), at its version, as
`pkg:apk/alpine/<package>@<version>`, with its aports origin and commit, its
licence as apk gives it, and each file linked.

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
   Decided on 28 September 2026 by the maintainers, under the owner's standing
   instruction to decide such questions and record why, on work item 36: the
   project's own runtime is accepted, with the licence note of item 4. The owner
   may overrule it:
   - **To accept:** there is nothing else to do.
   - **To refuse:** the AppImages cannot be built without GitHub, and the jobs
     should be removed, with the AppImages taken out of what a release carries
     (`build/ci/release-chain.py`'s `gathered_builds`, and the two builds and
     their tests out of `release-packages`' needs). The only other route is a copy
     of `type2-runtime` pulled from GitHub once, by hand, reviewed and hosted in
     the project's package registry. That is a decision about GitHub, not about
     code.
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
   linix rebooted on 28 September 2026 at 18:08 UTC, and `systemd-binfmt`
   registered the handler at that boot: the aarch64 runtime, packing and test
   jobs of 6.3.5's tag pipeline 2892097590 ran under it the next morning, and
   passed.
   Since 6.3.6 `appimage-aarch64-compile` asks first
   (`build/ci/arm64-emulation.sh`) and refuses by name, so that
   `appimage-aarch64` and `appimage-test-aarch64`, which follow it, are not
   started; `appimage-runtime-aarch64`, which follows nothing, still meets the
   error itself.
3. **Let the release collect the AppImages.** Done in batch42 (6.3.6), and run
   in `batch42`'s pipeline 2897966924, whose rehearsal of the release collected
   both images with their SBOMs and both sources tarballs:
   - the `release-packages` job (`.gitlab/ci/release-packages.yml`) needs
     `appimage-x86_64` and `appimage-aarch64` with their artifacts and the two
     test jobs, and `release` needs `release-packages` (a job of its own because
     `release` may need fifty jobs at most, GitLab's `ci_needs_size_limit`);
   - `build/ci/release-chain.py gather` holds each image to
     `appimage/SHA256SUMS-<arch>.txt` and makes its SBOM,
     `hMailServer-<version>-<arch>.AppImage.cdx.json`, from what
     `appimage-build.sh` recorded in `appimage/built-against-<arch>/`: `dpkg.txt`
     and `boost-version.hpp`, what the server was compiled against (the
     container's own for x86_64; `appimage-cross.sh`'s for the cross-compiled
     aarch64 server), `bundled.txt`, each library the image carries in `usr/lib`
     by the package, version and SHA-256 of the file it was copied from and the
     source package and version it was built from, and `runtime-linked.txt`, what
     the runtime links statically (`build/asset-sbom.py --kind appimage`);
   - `collect` takes both into the release, with a line each in the sums; each
     is signed with its bundle and `.sigstore.json` copy and named in the
     provenance, and the feed offers each to an installed AppImage by the name
     its update check asks for;
   - the sentence in `build/ci/release-verify.sh` that no release carries
     AppImages is gone; the notes say instead where the runtime comes from and
     its licences;
   - on a `batch*` branch the jobs now start at once, as on a tag, so that the
     release's rehearsal (`.gitlab/ci/release-rehearsal.yml`) has them; only
     `master` waits for a press.
4. **The licences.** Done in batch42 (6.3.6), and run in `batch42`'s pipelines
   2896082335 and 2897966924: both sources jobs made their tarballs twice to the
   same bytes, and both image tests found every licence file
   ([Licences](#licences)). The runtime links libfuse3 (LGPL-2.1) and lzo
   (GPL-2.0-or-later), which squashfuse is built with on Alpine, and their
   licences ask for the libraries' own source to go with the binary, not only
   the runtime's. The runtime's own source and the script that links it are in
   this repository and the release's source tarball; the source of every library
   linked into it, as Alpine built it, is the release file
   `hmailserver-<version>-appimage-runtime-sources.tar.gz`, which every AppImage
   names in `usr/share/hmailserver/licenses/appimage-runtime.txt`, beside the
   libraries' licence files. The Ubuntu libraries the image carries in `usr/lib`
   go out with their packages' copyright files, the licence texts those name and
   `bundled.txt`, which lists them and names the release file with their source,
   `hmailserver-<version>-appimage-bundled-sources.tar.gz`: each of the fourteen
   source packages at the version the image took, from Ubuntu's archive, which
   the LGPL of seven of them asks for. Each image carries hMailServer's own
   AGPL-3.0 text too. Nothing is left for the owner but to keep both sources
   files on each release for as long as its AppImages are there.

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
  Pinned-Dependencies check and `build/check-release-chain.py` did not follow
  `include:`, so neither saw the images or the jobs in `.gitlab/ci/*.yml`. Every
  image in `packages-appimage.yml` was pinned by digest, but no checker held it
  to that. Both follow it now: `check-release-chain.py` reads the included jobs
  a release depends on, and since 6.3.6 Pinned-Dependencies reads the images and
  installs of every included file and every shell script under `build/`.
