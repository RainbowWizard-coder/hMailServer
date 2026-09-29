# Changelog

This file records what changed in each release of hMailServer that this project has published, newest first,
and what is in the tree after the latest release. It is a summary. The full notes of a release - the reasoning,
the tests and the verification - are on its GitLab release page,
`https://gitlab.com/Progressiverobot/hmailserver/-/releases/<tag>`, which every section below links to.

This project is a maintained fork of hMailServer 5.7 by Martin Knafve. Its own releases begin at 6.0.0; the
`hMailServer-5.7.0-bNNNN` tags in the repository are upstream builds, not releases of this project.

**The latest release is 6.3.5, published on 28 September 2026.** Anything that arrived after it is in the tree
after 6.3.5 and not yet released. It is listed under [Unreleased](#unreleased---in-the-tree-after-635-not-yet-released)
and will carry a version number when a release is cut.

## Where the releases are

The project's home is <https://gitlab.com/Progressiverobot/hmailserver>. Its releases were published on GitHub
until GitHub suspended the account on 18 September 2026, and every `github.com/Progressiverobot` address in older
text now answers 404. Every release up to 6.3.3 has been re-created on GitLab from the GitHub original, under its
original date and with its files byte for byte, but for 6.2.0 and 6.0.0-B4, whose files were not preserved:
<https://gitlab.com/Progressiverobot/hmailserver/-/releases>, with the latest always at
<https://gitlab.com/Progressiverobot/hmailserver/-/releases/permalink/latest>. A tag with no release is at
`https://gitlab.com/Progressiverobot/hmailserver/-/tags/<tag>`.

One release is not a version of the server and has no section here:
[`build-inputs-1`](https://gitlab.com/Progressiverobot/hmailserver/-/tags/build-inputs-1), of 11 September 2026,
holds the three third-party files the Windows installer build downloads instead of keeping them in git - the
7-Zip 19.00 console, the MariaDB Connector/C 3.4.9 client and the SQL Server Compact 4.0 SP1 x64 runtime. Each is
recorded by SHA-256 in `hmailserver/docs/third-party-binaries.json` and fetched by
`build/get-installer-binaries.ps1`. Since 25 September 2026 the 7-Zip console is 26.03, fetched from 7-zip.org itself, and
the release's 7-Zip 19.00 is no longer used.

## How to read a section

- **Dates** are UTC. Where the release's own publication time was not saved, the date is the tag's, and the
  heading says so.
- **Schema** is the database version the release requires: `REQUIRED_DB_VERSION` in
  `hmailserver/source/Server/Common/Application/Constants.h` at the tag. The installer applies the steps with
  DBUpdater on Windows; from 6.3.0, `hmailserver --upgrade-database` does it on Linux. The upgrade is one-way, so
  take a backup of the database and of the data directory before installing.
- **Pre-releases** (the 6.0.0 B builds, 6.2.22-pre1 to pre6 and 6.2.23 Alpha 1 and 2) are listed where they fall
  and marked as such. Three tags have no release and are listed in one line each: 6.0.0-B3, for which no
  release is known, and 6.2.22-pre4 and pre5, whose releases were deleted.

## Issue and pull-request numbers

Every issue, pull-request and discussion number written before 24 September 2026 is GitHub's, from the project's
former home, and this file writes it in words: "GitHub issue 263", "GitHub PR 12", "GitHub discussion 18". GitLab
numbers its own issues from 1 again and links a bare `#` and a number to its own issue of that number, so no
number is written that way here. Upstream hMailServer's numbers are written the same way, as "upstream PR 552" or
"upstream issue N". A person's handle on either forge is written in a code span, so that nobody is notified.

## How this file is kept

By hand. It is not generated: the project does not use GitLab's `Changelog:` commit trailers. The Unreleased
section is updated when a batch lands on `master`, never by the commits of individual roadmap rows, which would
make every fold of parallel work conflict on this file. A released version's section is written once, before
its tag, in the commit the tag points at: from 6.3.5 the tag's pipeline makes the GitLab release's notes from
that section as the tag's own tree has it (`build/ci/release-chain.py`, `release-publish`), and refuses a version
without one, so the section cannot be added afterwards. GitLab CI's `checkers` job runs
`python build/add-license-headers.py --check` on every push; after editing, run it too and read the rendered page
for a stray `#` number or `@` name.

---

## Unreleased - in the tree after 6.3.5, not yet released

### Downloads and the release

- **The AArch64 `.deb` and `.rpm` are in the release again** (not yet run in a pipeline). The releases up to 6.3.3
  carried them; 6.3.4 and 6.3.5 did not, so an arm64 Debian, Ubuntu or RPM server has had no newer file than
  6.3.3. The release now collects `hmailserver_<v>_arm64.deb`, cross-compiled by `deb-ubuntu26.04-arm64` in
  Ubuntu 26.04's own container, and `hmailserver-<v>-1.aarch64.rpm`, `rpm-fedora44-aarch64`'s copy of its Fedora
  44 build - the names an installed arm64 server's update check asks for - each with its own CycloneDX SBOM
  (`<file>.cdx.json`), a line in `hmailserver-linux-<v>-SHA256SUMS.txt` and a Sigstore bundle, checked by
  `release-verify` with the server's own verifier like every other file: (the release's other files follow below)
  `release` waits for each package's build, its SBOM job and its install job, which installs and runs it in an
  AArch64 container of its distribution under qemu-user; no AArch64 machine has run them. A package job's
  container has neither PowerShell nor Python, so every package build now records what it was built against
  (`built-against/`: what dpkg, or the aarch64 sysroot's rpm database, held, and Boost's header), and the new
  `deb-ubuntu26.04-arm64-sbom` and `rpm-fedora44-aarch64-sbom` jobs make the SBOM from that in the CI image
  (`build/ci/packages/package-sbom.sh`; `build/asset-sbom.py make --dpkg-list` or `--rpm-list`). In a cross
  build the C and C++ libraries are named as the cross toolchain's, and every other library as the target
  architecture's own `-dev` package, never the build container's. `collect` holds each AArch64 package to the
  sums its build job wrote, and `release-publish` checks that the feed gives Debian and RPM servers on AArch64
  their file and bundle, as it does for x86-64. The x86-64 files, their SBOMs and `linux-build`'s two lines of
  the sums are unchanged; `linux-repro` still compares the x86-64 packages alone, and the AArch64 ones are
  built once and not compared (ReproducibleBuilds.md). `build/check-release-chain.py` now holds `release` to
  those six package jobs, none of which may be allowed to fail or wait for a press in a tag's pipeline, and
  `collect` to where they leave the files; the update feed's `/download/hmailserver_arm64.deb` and
  `/download/hmailserver.aarch64.rpm` follow the first stable release that carries them, and its
  `--serve-check` fails a stable release that carries any server's file without its bundle. The generic arm64
  `.deb` goes to the apt suite `ubuntu26.04`, as the amd64 one does; the generic `.rpm`s still go to no dnf
  repository, and the pages job no longer calls the aarch64 one built on Ubuntu (ContinuousIntegration.md,
  "What a release carries"; PackagesDeb.md; PackagesRpm.md).

- **The container image is pushed from the release's own pipeline, by hand, and started before it is left there**
  (not yet run in a pipeline). The image jobs are in every protected `v*` tag's pipeline. `container-image-deb`,
  started by hand after `release-publish` once the project variable `HM_PUBLISH_IMAGE` is 1, takes the `.deb` and its
  SBOM from the published release and verifies both with cosign as the tag's pipeline. The variable is read as the
  jobs run, not by a rule, so a release tagged before the move into the group `hmailserver` has its image pushed
  from its own pipeline afterwards. The image is pushed only at `registry.gitlab.com/hmailserver/hmailserver`,
  which the Compose file and the chart now name (`linux-image.sh` refuses the old path), then signed, scanned and
  started beside PostgreSQL by `container-image-start`, which checks SMTP and the REST API. The image sets
  `HM_INSTALL_KIND=container` and carries OCI and Artifact Hub labels
  ([ContainerImage.md](hmailserver/docs/ContainerImage.md)).

- **The Helm chart as a file of each release, served from `https://updates.progressiverobot.com/helm/`** (not yet
  run in a pipeline). The release carries it, signed, with its `.sigstore.json` copy. `helm-chart-package` packages `hmailserver-chart-<version>.tgz` at the release's version,
  naming the release's image, and `build/ci/helm-repo.py` writes the repository's index and Artifact Hub's metadata
  from the update feed in `package-repos`.

- **`hmctl` as a PyPI package, `hmailserver-cli`, and the PowerShell module with a Gallery manifest** (neither
  published yet). `build/ci/cli-packages.py` builds `hmailserver-cli`'s sdist and wheel from
  `hmailserver/source/Tools/Cli/python/` with the same `hmctl` file the `.deb` and the `.rpm` install, copied into it
  byte for byte, at `Version.h`'s version, with no dependency and no code that runs when it is installed; it runs
  on Python 3.8 and later. `HmailServer.psd1`, the module's new manifest, lists its 95 functions for Windows
  PowerShell 5.1 and PowerShell 7 at the server's version, which the release's version stamp now includes. The
  manual `master` jobs `pypi-publish` (a PyPI trusted publisher, with PyPI attestations) and `psgallery-publish`
  (`PSGALLERY_API_KEY`) say "not configured" until each account is set up. They publish only a release the update
  feed lists, rebuilt from its owner-signed tag, and never a version twice: `pypi-publish` finishes an upload that
  stopped halfway and fails when a PyPI publisher that once worked stops matching, and `psgallery-publish` refuses
  a module with no Authenticode signature, which the release does not sign yet
  ([CommandLinePackages.md](hmailserver/docs/CommandLinePackages.md)).

- **A source tarball with nothing non-free in it, which the Linux build and suite run from** (not yet run in a
  pipeline; the release carries it from 6.3.6, below). `build/ci/source-tarball.sh` writes `hmailserver-<version>.tar.gz`
  from a commit with `git archive`: the commit's tree less every path of `build/source-exclusions.txt` - SQL Server
  Compact 3.5's headers and both copies of its licence, which are not free, and what only the Windows build and its
  installer use (the rest of `libraries/` with the Fat Cow icons, the COM API, the Windows service host and
  helpers, the vendored zlib, the Control Panel and the database tools, the VBScript add-ons, the .NET Framework
  developer tools and the installer's sources, keeping the Control Deck, `tlds.txt` and `dh2048.pem`, which the
  packages install). It is reproducible - every entry carries the commit's time, uid and gid 0 and a 0644 or 0755
  mode, line endings are `.gitattributes`' alone (no system attributes file is read, and a clone whose
  `.git/info/attributes` sets any is refused), gzip writes no name or time - and git 2.39.5, 2.47.3, 2.50.1 and
  2.53.0 with gzip 1.10 to 1.14 made the same bytes. QuickJS stays and is declared with the other third-party
  notices of the code the Linux build compiles; `docs/SourceTarball.md` also lists the third-party test data kept
  (Mail::DKIM's corpus, and captured messages with no licence stated, for the Debian package to decide on).
  `build/check-source-tarball.py` holds the list to the tree in the `checkers` job (nothing the CMake build,
  `linux-build.sh`, `linux-suite.sh`, the other Linux jobs' scripts - packages, fuzzing, cross, sanitizer,
  coverage, repro - the suite's project and its directory, what the build and the suite read at run time, or a
  POSIX `#include` reads may be left out, and every file of the Windows-only trees must be) and a tarball to the
  list, its licences and its format; `--files-excluded` prints Debian's `Files-Excluded` field from the same list.
  The `source-tarball` job makes it twice and compares, and `linux-build` and `linux-suite` take it with
  `HM_SOURCE_TARBALL`; `build/native-dependencies.ps1 -LinuxOnly` gives the packages' SBOMs their components
  without the Windows build's inputs. See [SourceTarball.md](hmailserver/docs/SourceTarball.md).

- **The release carries every file the pipeline makes for it** (not yet run in a pipeline). From 6.3.6 a release is
  58 files and 149 links: the installer; the generic `.deb` and `.rpm` for x86-64 and AArch64; each Debian 12 and 13,
  Ubuntu 24.04, Fedora 44 and 43, EL 10 and 9, openSUSE Leap 16.0 and Tumbleweed release's own package for both
  architectures (`hmailserver_<v>-1~<release>_<arch>.deb`, `hmailserver-<v>-1.<release>.<arch>.rpm`); both AppImages,
  with the project's own runtime (work item 36), the source of the libraries linked into it and the source of the
  libraries they carry in `usr/lib` (below); the source tarball; the Helm chart; the sums of every Linux file, the
  two AppImage sources files and the chart; an SBOM for each of the 25 compiled files and the tarball, and the tree's
  two; a Sigstore bundle beside each, a byte-identical `<file>.sigstore.json` of each primary file's bundle, and
  `hmailserver-<v>.intoto.sigstore.json`, SLSA v1 build provenance naming every file by its SHA-256, signed as the
  pipeline with `cosign attest-blob` and checked with `cosign verify-blob-attestation` (SECURITY.md, "Build
  provenance"). The new `release-packages` job gathers the per-distribution packages, the AppImages, the two AppImage
  sources files and the tarball, holds each to its build's sums and makes the SBOM of each but the sources files
  (`build/asset-sbom.py` gains the kinds `appimage`, which lists what the image bundles and what its runtime links,
  and `source`), so that `release` stays under GitLab's fifty needs. Before anything is signed, and again before
  anything is uploaded, the release counts what GitLab and Pages take: every link's path (the `~` of a `.deb`'s
  version, which GitLab refuses there, is written `-`), `_redirects` under 1,000 rules and 64 KiB with the apt and
  dnf rules, and the feed under the 4 MiB a server reads (with the repositories as `package-repos`' `--keep` keeps
  them). The apt and dnf suites will serve the new packages once the repository key exists; openSUSE's go through
  OBS. A protected `batch*` branch rehearses the chain (`.gitlab/ci/release-rehearsal.yml`): collected with
  `windows-packages-x64`'s installer, signed as the branch, which no server trusts, and checked with the server's own
  verifier, which must refuse every bundle at its identity; nothing is published (ContinuousIntegration.md, "What a
  release carries", "The release rehearsal").

- **The source of the AppImages' runtime's libraries goes out with them** (not yet run in a pipeline). Each
  AppImage's runtime is linked statically on Alpine, and two of the libraries it takes in, libfuse3 (LGPL-2.1) and
  lzo (GPL-2.0-or-later), ask for their own source to go with the binary, not only the runtime's; with lzo's GPL, so
  does every other library linked in but the compiler's own. From 6.3.6 the release carries
  `hmailserver-<v>-appimage-runtime-sources.tar.gz`: for libfuse3, lzo, squashfuse, zstd, lz4, xz, zlib and musl, at
  the version and aports commit each runtime job recorded from the linker's map and apk's database, Alpine's APKBUILD
  and patches from gitlab.alpinelinux.org, fetched by the commit's id, and the upstream archive from Alpine's
  distfiles mirror - never the GitHub releases several APKBUILDs name - each source held to the SHA-512 its APKBUILD
  gives, with a README.txt saying where each file came from, what each licence asks and how to link the runtime again
  with a changed library. The new `appimage-runtime-sources` job packs it reproducibly, twice, and compares; the
  release gathers it with a line in the sums, a bundle and a `.sigstore.json` copy, and no SBOM, being source. Each
  image's `usr/share/hmailserver/licenses/appimage-runtime.txt` lists what its runtime links and names the file, and
  the image carries each library's licence files in `usr/share/hmailserver/licenses/appimage-runtime/`, with the
  GPL-3.0's text, under which the runtime takes lzo (runtime.c being AGPL-3.0-or-later). GCC's start-up files and
  libgcc stay out, under the GCC Runtime Library Exception
  ([PackagesAppImage.md](hmailserver/docs/PackagesAppImage.md), "Licences"; work item 36).

- **The AppImages carry the licences of the libraries in their `usr/lib`, and the release their source** (not yet run
  in a pipeline). Each AppImage carries in `usr/lib` the shared libraries the server needs that are not left to the
  host - libpq, libldap and what they need, nineteen files from fourteen Ubuntu 24.04 source packages - and seven of
  them are under the LGPL (GnuTLS, Nettle, GMP, libidn2, libunistring, libtasn1 and keyutils), which asks for the
  library's source to be offered with the binary. From 6.3.6 each image carries, in
  `usr/share/hmailserver/licenses/`, the copyright file of every package a library came from, every licence text
  those name (a list in braces included), `bundled.txt`, which lists each library with its package, version and
  source package and names the release file with their source, and hMailServer's own AGPL-3.0 text (`AGPL-3.0.txt`).
  The release carries `hmailserver-<v>-appimage-bundled-sources.tar.gz`: all fourteen source packages (`.dsc`,
  upstream archives, Ubuntu's changes) at the versions the images took, from Ubuntu's archive or, for a version the
  archive has superseded since - as a package the pinned build image already had can be, which the build does not
  upgrade - from its snapshot service at the time the image's dpkg installed it, each file held to the `Sources`
  index of an `InRelease` verified against Ubuntu's archive keys and again to its `.dsc`, with a README.txt saying
  what each licence asks, where each package was taken from and how to run an AppImage with a changed library; about
  75 MB, one file for both architectures. The new `appimage-bundled-sources` job packs it reproducibly, twice, and
  compares; the release gathers it like the runtime's sources, with no SBOM. Each AppImage's SBOM now lists every
  Alpine package its runtime links statically, with its aports commit and licence, and each bundled library's source
  package; a library an image starts to carry fails the image's own job until its licence is named in
  `build/ci/packages/appimage-bundled-sources.sh` ([PackagesAppImage.md](hmailserver/docs/PackagesAppImage.md),
  "Licences"; work item 36).

- **The release channels, each published by hand from `master`, dormant until its account exists** (none live yet).
  `.gitlab/ci/channels.yml` adds `registry-mirror` (the image by digest, with its cosign signature and SBOM
  attestation, to Docker Hub, Quay.io and optionally Amazon ECR Public, with `regctl`), `aur-publish` (the AUR's
  `hmailserver`), `ppa-upload` (the Debian source package to `ppa:hmailserver/hmailserver` for Ubuntu 24.04 and
  26.04), `copr-build` (Fedora Copr through its webhook; `.copr/Makefile` makes the source RPM in Copr's builder),
  `obs-publish` (the openSUSE Build Service, from `platform/packaging/obs/_service`), `chocolatey-release` with
  `chocolatey-publish` (packed on bench150), `snap-upload` (edge and candidate) and `channels-verify`, the dry run.
  Each publishes the release `HM_CHANNEL_VERSION` names or the update feed's latest (Copr and OBS the latest alone),
  verifies every release file it takes against its Sigstore bundle as the tag's pipeline - the project's id, read
  from the certificate's extensions, required at the group's path - before it logs in or sends anything, and never
  moves a channel back to an older release. The snap, not a release file, is held to the verified tarball or the tag;
  OBS fetches the tarball itself once the job has checked the permanent link serves the verified bytes. cosign and
  regctl come out of their images pinned by digest; nothing from GitHub, and no credential on a command line
  ([Channels.md](hmailserver/docs/Channels.md)).

- **The Arch package builds from the release's source tarball**, not a git tag, as a distribution's build
  (`-DHM_LIVE_UPDATE=OFF`, naming `makepkg -si` from the AUR as its update) and without LTO (Arch's default full-LTO
  link of the server passed 6 GB and was killed); `aur-publish` writes the verified tarball's SHA-256 in.

- **The Chocolatey package's id comes from `make-package-manifests.ps1 -ChocolateyId`**: `hmailserver`, or
  `hmailserver-progressiverobot` if upstream's maintainers do not add the project. Its addresses are the group's.

- **The release checks read the signing certificate's fields from its extensions, never by searching its bytes, and
  only from the certificate cosign verified.** release-verify.sh and container-image-deb.sh took the project id
  (Fulcio's 1.3.6.1.4.1.57264.1.15) from wherever its bytes first appeared, and a certificate's public key is bytes
  its holder chooses; registry-mirror read the id from whichever signature certificate held the identity's bytes, and
  check-provenance matched the identity the same way. One reader, build/ci/sigstore-certificate.py, now walks the
  certificate's DER to its extensions, with a shell twin for cosign's image held to it case by case. The shell reader
  reads a v0.3 bundle only - cosign verifies just the first certificate of a v0.1 or v0.2 chain, and accepts field
  names spelled in ways a count of `rawBytes` misses - container-image-deb.sh refuses a cosign without
  `--new-bundle-format`, and registry-mirror refuses a signature manifest Go's JSON reader would read otherwise (a
  key in another case or with a long s, a key twice, an index at the `.sig` tag); the Python reader refuses a
  duplicated key. build/check-release-chain.py holds every script to the readers and refuses the Sigstore OID written
  as bytes, decimal numbers or base64 anywhere else. The container image's mirror has cosign verify its signature and
  SBOM attestation from one copy of their manifests, each fetched by its digest, reads the project's id from those
  same manifests - the attestation's certificate now held to it as the signature's is - and puts exactly those
  manifests at each registry, replacing whatever signature was there; the release's SLSA provenance is held to the
  project's id too, as its files are; and neither certificate reader relies any longer on cosign refusing a malformed
  bundle to know which certificate it read. The server's own verifier reads the extension through OpenSSL and was
  never affected.
- **An Atom feed of releases** at https://updates.progressiverobot.com/releases.atom: every release, newest first,
  the first paragraph of its notes as the summary, ids that do not change when the project moves into the group;
  build/check-atom-feed.py holds it to RFC 4287 and to releases.json in the pages job
  ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md), "The Atom feed"). The files a server reads are unchanged, byte
  for byte, and a release whose name XML cannot carry is entered under its tag rather than stopping the feed.
- **Releases pause after 6.3.6; security fixes do not.** The 6.3 line receives security fixes until 30 September
  2027, each in a release of its own ([SECURITY.md](SECURITY.md), "While releases pause"; [RELEASE.md](RELEASE.md),
  "A security release during the pause").

### Administration

- **Text and values in the Control Panel can be selected and copied (work item 48).** Every page and dialogue: drag
  over text, double-click a word, Ctrl+C, or right-click for Copy / Select all; with nothing selected, Copy takes the
  whole text, and a selection dragged out into another program carries plain text only. Text inside buttons, lists,
  tabs, menus, grids and editors is left to its control, and keyboard navigation and screen-reader output are
  unchanged. Every plain list - domains, accounts, aliases and distribution lists, public folders, incoming relays,
  an account's IMAP folders, folder permissions, group members, recipients, a route's addresses and the Active
  Directory picker - copies its selected rows with Ctrl+C or its right-click Copy, one row per line in the list's
  order, and offers Select all where several rows can be chosen; the Ctrl+K search's results copy from their
  right-click Copy (its search box keeps the keyboard); data grids copy their rows as before. Live logs selects
  several lines (click, Shift+click, Ctrl+A) and copies them exactly, in the log's order; it follows new lines only
  while the end is in view and no longer splits a line the server is still writing into two rows. On REST API keys, a
  key's label, id, state and scope can be copied. Every Copy button now writes the clipboard the same way: a program
  that reads the clipboard the moment it changes (Windows' clipboard history, a remote-desktop client) no longer
  makes a copy fail after a second's freeze (a chart's Copy then showed no confirmation, and the new API key's and
  app password's Copy an error), and text copied just before the Control Panel is closed still pastes after it has
  gone.

### Operations

- **Why the container image waits, said truly.** `.gitlab/ci/container-image.yml`,
  [ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md) ("What the owner sets on the registry", 3),
  Roadmap2's release-chain row, the packaging README and a comment of `build/check-release-chain.py` said, and
  6.3.4's notes below say, that the image waits because GitLab.com cannot transfer a project whose container
  registry holds an image. GitLab's documentation does not say that: on GitLab.com a project whose registry is
  enabled is transferred only within its top-level group, one with more than 1,000 container repositories not at
  all, and a project must hold no image only on GitLab Self-Managed. The image waits because the maintainers decided
  on 27 September 2026 to move the project into the group `hmailserver` before the first image is pushed, so that
  its address, `registry.gitlab.com/hmailserver/hmailserver`, is the lasting one - a container repository's path is
  always its project's - and 6.3.4's `SigstoreVerifier` already trusts the group's path with the project's id,
  86730559. The texts that said the image appears at `registry.gitlab.com/progressiverobot/hmailserver` - the
  comments of the Compose file, the chart's values and the Dockerfile, the packaging README, EvidencePack.md,
  ContinuousIntegration.md's `cosign verify` example and SECURITY.md's `cosign verify-attestation` example, and
  `renovate.json`, which also said the registry was not switched on - now name the group's address; the Compose file
  and the chart still pull the present path, and change with the move. Nothing the pipeline does changes.

- **The documents say what the pipelines have run.** `batch41`'s pipelines of 27 September 2026 ran the package jobs
  for the first time, and [PackagesDeb.md](hmailserver/docs/PackagesDeb.md),
  [PackagesRpm.md](hmailserver/docs/PackagesRpm.md), [PackagesAppImage.md](hmailserver/docs/PackagesAppImage.md) and
  [PackagesWindows.md](hmailserver/docs/PackagesWindows.md) now say which, job by job, where they said no job had
  run: every `.deb` build job passed in pipelines 2886955494 and 2887115157, and every `.rpm` build job in
  2887115157; no install job has started in any pipeline, no AppImage job has been started, and
  `windows-packages-x64` built the unsigned x64 installer in 2887115157.
  [ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md) no longer says no GitLab job has run, says
  the GitHub workflows stayed unchanged from `faca023d4` until `6593259e0` removed them, and gives the registry's
  cleanup policy as the API does: it kept two of the five patterns the document listed, and the maintainers added the
  other three through the API on 28 September 2026, before the policy is ever switched on; it and README.md say that the fuzz schedule and Renovate's schedule and token
  exist since 27 September. README.md no longer says the project has no ARM machine to build the AArch64 packages on
  (the pipeline cross-compiles them), `build/ci/release-chain.py` no longer says the pipeline does not build them,
  and Roadmap2's Windows on ARM row names the CI job it said there was not.
  [PackageManagers.md](hmailserver/docs/PackageManagers.md) no longer says the GitHub workflow built the manifests
  for the releases up to 6.3.3 - it arrived after the `v6.3.3` tag, and no release ran it - nor that the AppImage is
  attached to each release, nor that the Chocolatey identifier `hmailserver` is free: it holds upstream's
  hMailServer 5.6.8.2574, unlisted, and publishing under it needs the project to become one of its maintainers; its
  table no longer says the workflow ran on every release, and its walk for the first Chocolatey push says the push
  waits for that. The documents that sent a reader to `6399e08a5` for the removed GitHub workflows now give
  `faca023d4`, the same commit as `master`'s history holds it: `6399e08a5` is its copy from before its branch was
  rebased, which `master`'s history does not hold.

- **`HM_DB_WAIT_SECONDS`** (0 to 999999, default 0): the image's entrypoint waits for the `[Database]` server to
  accept a TCP connection before opening it, and does not start (exit 69) when it has not. The entrypoint reads the
  ini's `[Database]` section as the server does (spaces trimmed, names in any case), and `HM_INSTALL_KIND`, which
  the image sets for the update check, is not a reason for it to open the database.

- **A build without the live update, and a live update that knows a snap and a container** (not yet run in a
  pipeline). CMake's new `-DHM_LIVE_UPDATE=OFF` builds the server with no apply step and installs neither
  `hmailserver-update` nor its two units; an AppImage made from an OFF tree carries no `hmailserver-update`
  either. Its update check still names a newer release and the distribution's own command
  (`-DHM_UPDATE_COMMAND="apt upgrade hmailserver"`). A server in a snap (`SNAP` naming the directory it runs from)
  or in the container image (`HM_INSTALL_KIND=container`) names `snap refresh hmailserver` or the image to pull,
  and never downloads or installs a release file. `GET /api/v1/update` reports `installKind`, `liveUpdate`,
  `updateCommand` and `updateAdvice`, and the Control Deck shows them. What the live update did in images before
  6.3.6 is written down in [LiveUpdateKinds.md](hmailserver/docs/LiveUpdateKinds.md): it installed nothing,
  downloaded two packages and reported HM6890. Tested by `build/test-install-kind.sh` (CI job
  `linux-live-update`).

- **A new Windows installation can be set up against SQL Server, PostgreSQL or MySQL with no window** (not yet
  run against a server). Both installers take the database on the command line - `/dbtype=`, `/dbhost=`,
  `/dbport=`, `/dbname=`, `/dbuser=`, `/dbpasswordfile=`, `/createdatabase=`, `/dbservice=` - or in the
  container's own `HM_DB_*` variables, the password only in `HM_DB_PASSWORD` or a file, never on a command line.
  `DBSetup.exe /unattended` then creates the database, or uses one that exists and upgrades it with DBUpdater if
  it is older, and ends only at the schema the build requires, its lines in the setup log. Settings it cannot use
  are refused before anything changes, among them a password file that is not UTF-8 and the characters the server
  cannot yet pass to its database (a single quote or a backslash for PostgreSQL; for SQL Server a semicolon, or a
  quote as a value's first character). This is how a silent installation of the ARM64 build, which has no
  built-in database, finishes; its installer now refuses a silent new installation given no database before
  anything is installed, rather than open DBSetup's wizard where nobody can answer it. On x64 it is an option
  beside the built-in database, which stays the silent default unless `HM_DB_TYPE` is set in the machine's or the
  user's environment, which makes a new installation an external one; the setup log says whether the type came
  from `/dbtype` or the environment. The x64 installer still does not run on ARM64 under emulation
  (`x64compatible`), by the maintainers' decision of 28 September 2026, which WindowsOnArm.md records with its
  reasons. The installer smoke test checks the path with `-DatabaseSettings` (`installer-smoke-run.sh
  --database`).

- **Scripting off runs no script engine.** With scripting off the server no longer compiles the event script and runs
  it once per handler at every start, reinitialise and reload; switching scripting on - by COM, REST or an
  environment - loads it, compiled and searched, before the switch takes effect, and reports then a script that does
  not compile, whatever runs beside the switch - a reload, a reinitialise, a switch-off, another switch-on: a reload
  that read the switch as off is made again once it is on, and of two loads that overlap, the script of the one
  started last stays in force, unless it does not compile, which is reported. A rule's Run function action against a
  script loaded while scripting was off, which never compiled, is refused (HM5018) instead of being handed to the
  engine. An empty script runs no engine.

- **Crash records that place the fault.** The crash oracle's record adds, after the fields it always had, what an
  access violation read, wrote or executed and where, the faulting address's image base and offset, and up to twelve
  frames of the faulting thread; the REST API writes to the application log which method and path it answered 500
  after an exception.

- **Fixed: settings read while they were being reloaded.** hMailServer.ini's database and directory values, the
  administrator's second factor, and each value of the settings store and its map are read and written under locks,
  each value whole; a reinitialise or a COM Settings.Directories call could hand another thread an empty or freed
  value. A reinitialise now builds the settings store, the SMTP, POP3, IMAP and cache configurations, the server
  messages, blocked attachments, SSL certificates, DNS blacklists and SURBL servers, the local addresses and the
  top-level domains whole before another thread can see them, and hands each over under a lock: a COM call made
  during one - the Control Panel's or a script's - could copy a pointer while it was being replaced, or find the
  settings not yet read and take every one as missing (0 or empty, and HM5015 in the ERROR log). The logger's level,
  device and format are read and written whole, and `[Database] UpgradeOnStart` reads as off until the file is first
  read. Several values read one after another are still not read as one set, and a setting changed during a
  reinitialise, after the new settings were read, is stored but not in force until the next reinitialise or start.
- **Active-active nodes: the design, not built** ([ActiveActive.md](hmailserver/docs/ActiveActive.md)). Until it is,
  one service per database.

### Build and tests

- **[Roadmap3.md](Roadmap3.md): the plan for everything the project had on GitHub.** A new file at the
  repository root maps, row by row, what it takes to bring back on GitLab what a user could download, verify and
  install before 18 September 2026 - the AArch64 `.deb` and `.rpm`, the AppImages, the container image, the
  Sigstore bundles and the build provenance - and to add what the owner asked for beside them: the ARM64 Windows
  installer, the apt and dnf repositories, winget and Chocolatey, and every other channel people install mail
  servers from. It opens with a counter line like Roadmap2.md's, and `build/check-roadmap2.py` now holds both
  files: it fails when either counter disagrees with its rows or either file is missing, checks one file alone
  when given its name, and has a `--self-test`, run by the `checkers` job, whose controls include a wrong
  Roadmap3.md counter beside a right Roadmap2.md one, which the check as it was let through. README.md,
  Roadmap.md, CONTRIBUTING.md, SUPPORT.md, the documentation index and the Feature and merge request templates
  link it. The `.deb` and `.rpm` help pages, which said no Roadmap2 row carried choosing a package by
  distribution, now name the Roadmap3.md row that does.

- **A clock flake in the two-factor tests, not a server fault.** The Windows gate of pipeline 2887524317 failed
  `TwoFactorAuthentication.ACodeComputedElsewhereIsAccepted` with "The previous step must be accepted": the test
  began at 23:37:29.5 UTC, half a second before a thirty-second step ended, computed the previous step's code in
  step s and had it judged in step s+1, two steps behind the server and outside its window of one step either side.
  `OneTimeCodes.WaitForRoomInTheStep` now returns at once when ten seconds or more of the step remain and otherwise
  sleeps into the next step, and the two tests that judge codes against the server's step
  (`ACodeComputedElsewhereIsAccepted` and `ACodeIsTakenOnceOverCom`) call it after enrolling. The suite's other
  codes come from `OneTimeCodes.Fresh`, which never hands out a step behind the clock, or are refused whatever the
  step (a wrong code, one five steps out, a wrong password or a spent budget), so they needed nothing. The new
  fixture `ServerFreeTests.OneTimeCodesStep` replays the failed run against a clock of its own, without a server,
  in both suites: without the wait the previous step's code is two steps behind (its negative control), with it
  the code is judged in its own step.

- **The ARM64 toolchain check could not find the ARM64 C++ runtime on any machine.**
  `build/ci/packages/check-arm64-toolchain.ps1` looked for it with `Get-ChildItem -Path '...\VC\Redist\MSVC\*\arm64'
  -Filter 'Microsoft.VC*.CRT'`, which with a wildcard in the path returns the `arm64` folders themselves and filters
  their names. It reported the runtime missing on the laptop, whose Build Tools 18.9 has it, and on bench150 in
  pipeline 2887115157, where `windows-packages-arm64` stopped on that report with the ARM64 tools and ATL for ARM64
  present, as 6.3.5's notes below say. The check now asks for `...\arm64\Microsoft.VC*.CRT` as one path. It also no
  longer tells the owner to add components the instance already has: when every component is installed and a file is
  not there, it prints the Installer's `repair` of the instance, or, for the runtime, the `modify --add` of the
  component that the instance's own catalog says installs `Microsoft.VisualCpp.CRT.Redist.ARM64` when the instance
  lacks it. `check-arm64-toolchain.ps1 -SelfTest` holds both to instance trees and catalogs made to order, with the
  old path and the old command as its negative controls; on the laptop the check now exits 0.

- **The command-line packages are checked wherever they change.** `build/check-cli-packages.py`, in the
  `checkers` job, fails when the manifest's version or function list drifts from `Version.h` or the module, or the
  Python package gains a dependency, a version of its own, an install hook, a second copy of `hmctl` or a
  standard-library module newer than Python 3.8, or `hmctl` runs code when it is imported; its `--self-test` breaks
  each rule in a copy. The new `cli-packages` and `cli-powershell` jobs (`.gitlab/ci/cli-packages.yml`; merge
  requests and branches, never a tag) build the sdist and the wheel twice and compare them, run `twine check
  --strict`, install the wheel alone, and run `Test-ModuleManifest` and PSScriptAnalyzer - from the PowerShell
  Gallery, held to its published SHA-512 - under pwsh. PSScriptAnalyzer's 25 findings in `HmailServer.psm1` are
  fixed or justified on the one function, with no command, parameter or confirmation prompt changed;
  `Export-HmConfiguration -Verbose` now says why the ini section is empty when the server does not answer for it.

- **The Linux build includes the distribution's `zlib.h`.** Six core sources included the zlib header vendored
  for `hMailServer.exe` on every platform, so the Linux server was compiled against zlib 1.3.2's header and
  linked against the distribution's library (1.3.1 on Ubuntu 26.04). They include `<zlib.h>` under
  `HM_PLATFORM_POSIX` now; the Windows build compiles the same text as before.

- **An ARM64 publish of the .NET tools no longer takes the x64 COM wrapper from an earlier x64 publish.** MSBuild
  searched each tool's own `publish` folder before the reference's hint path, so `build-tools.ps1 -Platform ARM64`
  after an x64 publish compiled all five tools against the x64 wrapper (warning CS8012) and shipped it, in the
  add-ons' `Addons` folders where nothing replaced it; `check-pe-machine.ps1` found five. The publish folders are
  kept out of the projects' items (`Tools\Directory.Build.props`). CI builds in a clean checkout and never met it.

- **`native-dependencies.ps1 -LibrariesRoot`** holds each linked library's version in an installer's component
  list to the tree the build used, which records its own (OpenSSL's `VERSION.dat`, Boost's `version.hpp`,
  PostgreSQL's `meson.build`); the ARM64 list was checked that way against the ARM64 libraries of 26 September.
  The lists themselves are unchanged.

- **A merge request answers for what it brings.** On GitLab's shared runners, with no secret:
  `build/check-binary-manifest.py`, run by `checkers` in every pipeline, fails on a committed binary that
  `hmailserver/docs/third-party-binaries.json` does not list at its SHA-256 (what `verify-binary-provenance.yml` did
  until 18 September); `semgrep-sast-gate` holds a merge request on a Critical or High `semgrep-sast` finding in a
  file it changes that `.gitlab/security-exceptions.yml`, as the merge request's base has it, does not accept
  (`build/ci/security-gate.py --merge-request`, the release's own rule; OpenSSF Baseline OSPS-VM-06.02); and
  `mr-dependency-check` holds one that adds a dependency version with a Critical or High advisory in OSV's own
  export of NuGet's or PyPI's advisories (`build/ci/mr-dependency-check.py`), what GitHub's dependency review did
  (`.gitlab/ci/mr-checks.yml`; ContinuousIntegration.md, "Merge request checks").

- **The server is built and tested with the newest GCC as well as clang.** `linux-build-gcc` (`.gitlab/ci/gcc.yml`;
  linix; `master` and `batch*`, never a tag) builds the server and its tester programs with the CI image's own GCC,
  Ubuntu 26.04's GCC 15, and runs them and the OpenPGP round trip; `build/ci/linux-build.sh` takes
  `HM_COMPILER=gcc`, and a GCC build makes no packages. The Debian and Ubuntu package jobs already built with GCC
  12, 13 and 14; none built natively with GCC 15. The CI image already carried it through `build-essential`, and is
  unchanged.

- **OpenSSF Scorecard, self-run.** `scorecard` (`.gitlab/ci/scorecard.yml`, the schedule described `scorecard`) runs
  Scorecard 5.5.0 taken from cache.nixos.org by its Nix store path, with every path of its closure pinned by NAR
  hash, not from GitHub, and `build/ci/scorecard-pages.py` publishes its result and a badge at
  https://updates.progressiverobot.com/scorecard/; a result it cannot fetch or read never stops the site's
  deployment. Until the owner's `GITLAB_AUTH_TOKEN` exists it says "not configured" and passes.

- **SonarQube Cloud's scope, and GitHub kept out of a job.** `.sonarcloud.properties` scopes SonarQube Cloud's
  automatic analysis to the project's own code and tests. `.gitlab/ci/no-github.yml`'s `.no-github` puts GitHub's
  six hosts at 0.0.0.0 and `::` inside a job, has the hosts file read first where an image has no
  `/etc/nsswitch.conf`, and refuses a job it cannot protect. ContinuousIntegration.md says what GitLab's free tier
  and Ultimate give and why the Code Quality widget is not used.

- **The channels are checked where they change.** `channels-check` runs every channel script's self-test and proves
  the verification with the pinned cosign itself against `build/ci/channels/fake-sigstore.py`, a Sigstore of the
  jobs' own (a Fulcio-shaped authority with an embedded SCT, a Rekor-shaped log, a trusted root).
  `build/check-channels.py`, in `checkers`, holds the jobs to protected master, by hand, allowed to fail, no ID
  tokens and `.no-github` first, and the PKGBUILD, the AUR's pinned host keys, OBS's `_service` and the Copr Makefile
  to shape.

- **The Windows gate is not failed by an earlier run's crash record, and keeps what places a fault.**
  build/ci/windows-gate.ps1 moves aside and prints the crash-oracle.log the binary before it left, and on a failure
  saves the service's module list with every recorded address placed in it (server-modules.txt) and the Application
  log's crash events (crash-events.txt). On 28 September a gate failed its first test on the record a killed earlier
  gate had left, with every other test passed.

- **Boost for the package builds comes from the job's cache, archives.boost.io or SourceForge.**
  build/ci/packages/fetch-boost.sh makes six attempts at each source and holds every copy to the pinned SHA-256; the
  EL, openSUSE and Debian 12 build jobs cache the tarball under its SHA-256. On 28 September three package jobs of
  6.3.5's pipeline failed because some of archives.boost.io's addresses did not answer the runner.

- **The CI's Python is 3.14, and Renovate no longer proposes a locked requirements file it cannot lock.** Every job
  image pinned by digest is at Renovate's digest of 28 September 2026, and every Python job runs in
  python:3.14-slim-trixie; the locked requirement files are remade for 3.14 with the same versions and hashes.
  Renovate's Python merge request of that day had moved twelve versions inside build/ci/requirements.txt without
  resolving or hashing them - click kept its old hashes, and Semgrep 1.178.0 accepts none of the new versions - so
  the semgrep job, and with it the release's security gate, would have failed. Its pip manager now reads the .in
  files only, and its merge request says how to remake the locked files. Microsoft.NET.Test.Sdk is 18.10.1.

- **The semgrep job counts a file Semgrep could not parse, or parsed leaving more than ten of its lines unread, as a
  hole in the scan**, as it did a rule that timed out: Semgrep reports both only as a warning, and the job had taken
  `build/ci/semgrep.sh` (not parsed at all), `container-image-sign.sh` (97 of 138 lines unread, every command that
  signs and attests the image) and `scorecard.sh` (117 of 243) as scanned; the three are now written so Semgrep's
  bash parser reads every line.

- **The regression tests that read a mailbox by the order its messages arrived in no longer depend on which delivery
  thread wins.** Each message is in the mailbox before the next is sent (FullTextSearch, which failed 6.3.5's gate
  once with "1 3" for "1 2"; RestApiRecoverDeleted, Discovery, ObjectStorage, and RestApiSelfService's subscriptions
  and keywords tests), and the quota warning, which is queued before the message that crosses the threshold is
  stored, is found by being the one message not sent rather than as the last one. About 290 other tests that deliver
  several messages were read and left as they are: they count, compare sets, find by content, or already wait.

---

## [6.3.5](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.5) - 2026-09-28

Everything that landed on `master` after the `v6.3.4` tag, from 27 September 2026 ([compare `v6.3.4` with
`v6.3.5`](https://gitlab.com/Progressiverobot/hmailserver/-/compare/v6.3.4...v6.3.5)). The schema moves from **6104
to 6105 in one step**, which adds a column to the quarantine's table, `hm_quarantine.quarantinehelo`. What changes
on upgrade without a switch comes first. The operator documents named below are in `hmailserver/docs`, which has
five more of them than it had at 6.3.4.


This is the first release built to be made by the project's GitLab CI from its tag ([RELEASE.md](RELEASE.md), *The
release on GitLab* and *On GitLab, from 6.3.5*). The tag's pipeline builds the Windows installer on the project's
Windows runner after the whole Windows suite has passed there; once the owner has started the `release` job, it
Authenticode-signs the installer through Azure Artifact Signing with an RFC 3161 timestamp, signs every file of the
release with Sigstore as that pipeline -
`https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/v6.3.5`, issuer `https://gitlab.com` -
into a `<file>.cosign.bundle` beside it, and checks every bundle with the server's own `--verify-update` before
anything is published. That identity is the one a 6.3.4 server trusts (`SigstoreVerifier.cpp`, unchanged since
6.3.4, and the verifier that check runs), so the live update of a 6.3.4 server is built to install this release, on
Windows and on Linux. A server from 6.2.28 to 6.3.3 trusts only the GitHub workflow that signed the releases up to
6.3.3, and as it stands its live update refuses this one: install 6.3.4 or this release by hand, and keep
`UpdateAutoDownload` off until then, or on Windows give it the GitLab signer as
[UpdateFeed.md](hmailserver/docs/UpdateFeed.md) describes. What the release carries, and what the new package jobs
leave in CI only, is under Operations.

### What changes on upgrade without a switch

1. **The Control Panel's sidebar is in the order a new administrator needs.** Welcome and Dashboard come first,
   then a new group, **Set up this server**: the **Setup checklist**, which was *External setup* under Monitoring &
   troubleshooting, then Setting profiles and DNS records. Two groups are renamed - *TLS & certificates* is
   **Certificates & encryption** and *Access & abuse protection* is **Sign-in & access** - and Connections &
   protocols gains a **Webmail** page (Administration, below). The palette (Ctrl+K) still finds the checklist as
   External setup. A page's `nav-<key>` UI Automation id is unchanged; the `navgroup-` and `navrail-` ids of the
   new and the renamed groups follow their new headings, so a UI test that finds a group by its old id must be
   changed.
2. **The quarantine shows the HELO name, and groups.** The Control Panel's Quarantine page and the Control Deck's
   Quarantine view gain a HELO column and group the held messages by the sender's domain unless told otherwise.
   Messages held before the upgrade have no HELO name recorded and are grouped with those that have no HELO
   domain. The Deck now asks before a delete with a Yes or No of its own whose default is No, as the Control Panel
   already did; it used the browser's `confirm()`, whose default is OK. It offers whitelist and blacklist only to a
   credential holding `manage_settings`, and a held message's headers and preview only to one holding
   `view_mailbox`, as the server requires (below).
3. **The webmail's keyboard.** The message list is one Tab stop, where each row put its box, star and marker in
   the Tab order, and the list's keys reach what a row holds. Enter on a button in the mail view presses the
   button; it used to open the message under the list's cursor. A letter typed into the formatted signature is
   text, where `c` opened a new message. Escape closes every modal dialogue as its Cancel does and gives the focus
   back to where it was, the page under a dialogue cannot be reached until it closes, and menus close on Escape.
   The event and task editors close when the page changes. The compose window's *Plain* is **Remove formatting**,
   its tooltip's words. On a phone a row's importance marker is shown and not pressed; the message's More menu
   marks it.
4. **An invitation sent the way Outlook sends one is shown as an invitation.** `GET /api/v1/me/messages/{id}`
   answers `"invitation": true` for a `text/calendar` part whose `Content-Type` carries parameters and which has no
   `.ics` name, where it answered `false`, and the webmail shows the invitation's card with Accept, Maybe and
   Decline (the fix is under IMAP, POP3 and the webmail).
5. **What a script sees.** `GET /api/v1/quarantine` gives each message three more fields - `helo`,
   `sender_domain` and `helo_domain` - and four routes are new. Over COM, `Quarantine` gains `Bulk` (dispid 9)
   and `QuarantinedMessage` gains `HeloName`, `SenderDomain`, `HeloDomain`, `ReadHeaders` and `ReadPreview`
   (dispids 11 to 15), added to the existing interfaces; no existing member or dispid changes. A program built
   against 6.3.5's type library that calls one of them needs a 6.3.5 server.

### Anti-spam, anti-virus and mail flow rules

- **Reviewing the quarantine** (schema 6105; [GitLab issue #1](https://gitlab.com/Progressiverobot/hmailserver/-/issues/1)):
  - **The HELO name** the client gave is stored with each held message. A message held during the SMTP
    conversation records the session's own; one held later, at delivery - by a rule, a block list, an account's
    own threshold, the sandbox or impersonation protection - takes it from the `Received` header this server wrote
    when it accepted the message, and only when that header names this server, so a `Received` line the sender
    wrote is never read. Empty for a client that gave none, for a message that did not arrive over SMTP and for
    every message held before the upgrade.
  - **Grouping**: by the sender's domain, by the HELO name's domain or not at all, each group folding, the group
    with the newest message first, and every column sorting the messages inside the groups. The domain is the
    organization's - the registrable domain by the Public Suffix List - which the server works out, so that every
    page groups a message the same way.
  - **Many at once**: a box on every message, every group and the header, and four actions on a message, a group
    or the checked messages - release; delete; whitelist, then release; blacklist, then delete. Whitelist and
    blacklist write the lists that already exist and no third one: the sender's address on the server's allow
    and block list (the default), or the organization's domain as `.example.com` on the domain white and black
    list, matched on the envelope sender, or on the HELO name when the page is grouped by HELO domain. Several
    messages from one sender or domain make one entry, and an entry already there is changed rather than
    doubled. The dialogue first shows what the server would write (a dry run); a domain that is itself a public
    suffix (`co.uk`, a HELO of `localhost`) is written only after a box is ticked, and without it the server
    refuses with 409 and changes nothing. The entries are written before any message is acted on, so a message
    whose entry could not be written - the null sender, no HELO recorded, a full list - is left in the
    quarantine with the reason.
  - **The headers and a plain-text preview** of a held message, read without releasing it: the headers up to
    65,536 characters, and the preview up to 16,384, the text part or the HTML part with its markup, style sheets
    and scripts taken out. Nothing is rendered as HTML.
  - **The Control Deck** is level with the Control Panel: the same groups, columns, boxes, icons and dialogues, one
    request for the checked messages, a delete dialogue of its own with No as the default, and Remove expired.
    Against a server from before this release, the Control Panel releases and deletes one message at a time, and
    its whitelist and blacklist say the server needs upgrading.
  - **REST**: `POST /api/v1/quarantine/bulk` (1 to 1,000 ids and one action, with `dry_run` and
    `confirm_public_suffix`), `POST /api/v1/quarantine/expire`, and `GET /api/v1/quarantine/{id}/headers` and
    `/preview`. The list, the headers and the preview need `view_quarantine`, and the headers and the preview
    `view_mailbox` beside it, since they are the mail itself; `bulk` and `expire` need `manage_quarantine`, and a
    `bulk` that allows or blocks needs `manage_settings` beside it, since it changes who is let in for everyone.
    Every one of them is refused to a credential confined to named domains, as the quarantine's other routes are.
    **COM**: `Quarantine.Bulk` takes and answers the same JSON, and each `QuarantinedMessage` has `HeloName`,
    `SenderDomain`, `HeloDomain`, `ReadHeaders()` and `ReadPreview()`; `Bulk`, `ReadHeaders()` and
    `ReadPreview()` are the server administrator's, as the rest of the quarantine is over COM
    ([SenderListsAndDigests.md](hmailserver/docs/SenderListsAndDigests.md#reviewing-the-quarantine)).
- **A HELO that is an address has no domain.** A client that says `HELO 192.0.2.7`, bracketed or not, and a sender
  `someone@192.0.2.7`, have no organization's domain: `helo_domain` and `sender_domain` over REST, and `HeloDomain`
  and `SenderDomain` over COM, are empty, the message is grouped with those that have none, and whitelisting or
  blacklisting it by domain leaves it where it is and says why: "The HELO name is an address, which the domain list
  does not name." for the HELO, and "The sender's address has no domain to list." for the sender. Handed to the
  Public Suffix List, whose implicit rule takes the last label for a top-level domain, `192.0.2.7` would have been
  the domain `2.7`, and blacklisting it would have refused at `MAIL FROM` every client whose HELO is an address
  ending in `.2.7`.
- Fixed: a rule with a **Readable text** or **Sensitive content found** condition, which 6.3.4 added, read a
  message's HTML part in time that could rise with the square of its size. The part's entities were unwrapped by
  a replacement that moved the rest of the text once per occurrence, so 400,000 `&amp;` - two million characters -
  held the delivery for about a minute, and on a server with such a rule anybody could send one. Each entity is
  now unwrapped in one pass into a new string, with the same result (`ContentPolicy::StripHtml`). The quarantine's
  preview ends in the same function, and takes the style sheets, scripts and head out of an HTML part in one pass
  too, so a held message's preview costs time in proportion to its size.

### IMAP, POP3 and the webmail

- **The webmail, audited on a phone and a desktop, light and dark, by keyboard and through the accessibility
  tree**, with a seeded mailbox, and what it found fixed. On a phone: the search box has room to type and its
  options are a sheet the width of the screen; the tour skips a step whose target a phone does not draw and keeps
  its card on the screen; help covers the page instead of squeezing it; the settings tabs are one line that
  scrolls; and everything a finger presses is at least 44 pixels each way. By keyboard: the changes above, a focus
  ring on about twenty kinds of control that had lost it, and the earlier messages of a conversation and the
  Storage page's rows opened by keyboard as well as by mouse. For a screen reader: an unread row says Unread, a
  narrow list's tabs are named with their unread count and are a tab list, the row under the list's cursor is
  said, and the hidden page headings are translated. Contrast is raised where it fell short: the initials on the
  avatar colours, white on the dark theme's pale accent and danger colours (dark text now), the placeholders, and
  the light theme's warning colour (5.5:1, from 4.3:1); and the page declares its colour scheme, so that the
  browser draws its own parts of a control for the dark theme. Also: a long attachment name is cut with an
  ellipsis, and the week and agenda views name their month.
- **The webmail heard through a screen reader.** Every view was walked with NVDA 2026.2 in Chromium, in English
  and German, and what it said fixed. The message list is one control that reads each message as one line, its
  state first ("unread, starred, Grace Hopper, Re: The migration plan, Sep 26, has attachments, 3 of 51"), where
  Tab used to read all fifty rows and each row's controls; Home, End and the list's own keys work in it, Enter or
  `o` opens a message at its subject and Escape comes back to the same row, and Shift+F10 opens a message's menu
  from the keyboard. The star says whether it is pressed. What happens without the focus moving - a toast, once,
  not its countdown; a search's count; an attachment added; a new calendar month; a send that failed after the
  undo delay, which now also reopens the message and says why - is said in one place, and `z` presses a toast's
  Undo. Address suggestions, the command palette and mentions say the suggestion Enter will take; folders say
  their unread count in words and a subfolder its parent; the current folder, page, tab and view are said;
  dialogs and help open at their headings; the logo, the avatars' initials and a duplicate heading are hidden
  from a screen reader. 82 texts that were English on a translated page are in all 23 languages, and the server
  has a browser check the webmail's language file on each use instead of keeping it for a day, so new texts are
  not left in English after an upgrade. A dialog's name is said twice as the focus enters it, and a colour
  picker's name is not said; both are NVDA's reading of what Chromium exposes. Roadmap2 row 43 is done.
- Fixed: an invitation whose calendar part carries parameters in its `Content-Type` and no `.ics` name - the shape
  Outlook sends - was never marked as an invitation, so the webmail showed no card for it. The part's media type is
  now read without its parameters, as the server's iMIP code already read it. 6.3.4 had it too.

### Administration

- **Every database setting that should have an editor has one in the Control Panel.** 146 of the 314 settings the
  server keeps in its database had no editor in the Control Panel, and 137 of them needed one: the webmail's
  sign-in (passkeys, OpenID Connect, SAML, account recovery), its sessions and limits, OpenPGP, Web Push, the
  alerts, the SNMP agent, the audit trail, outbound webhooks, the spam traps, the complaint feedback loops, the
  reputation watch, country blocking, compromised-account detection, the object store, the managed lists and the
  migrations. They are now on the pages an administrator looks for them on - new tabs on Delivery of e-mail,
  Anti-spam, Auto-ban, Logging, Performance and Advanced - and a new **Webmail** page, under Connections &
  protocols, holds the webmail's own: Sign-in, Browser sessions, What people may do, Encryption, Notifications and
  Import. The 34 a COM property carries are edited over COM. The other 103, which the server reaches over its REST
  API alone, are read and saved over that API, which is off as shipped (`RestApiPort` 0): with it on, the Control
  Panel signs in to it once with the credentials it signed in with, asks for the administrator's one-time code once
  when there is a second factor, and sends only the keys that changed; with it off or out of reach, each such tab
  says so, and why. The domain dialog gains Country blocking, OpenPGP and Webhooks tabs, the account dialog an
  Identity tab (a person, a room or a piece of equipment, and the external ID SCIM matches on), an external account
  the options for a mailbox several people share, and a managed list's member how they receive the list's mail.
  `build/check-db-settings-coverage.py` now fails any database setting with no editor and no recorded reason for
  having none; the other nine have such a reason.
- **The Control Panel, audited page by page** in both themes at two window sizes, with each colour measured. The
  server status at the top right is a filled box with no outline - Connected is green on a soft green fill,
  6.0:1 in the light theme and 5.9:1 in the dark - and every status and notice is at 5.5:1 or better; High
  Contrast keeps a one-pixel outline. The light theme's field hints and captions are at 5.3:1, where they were
  3.3:1. Toasts are on the theme's neutral surface, with their level in the title and a glyph, and a failure - a
  page that could not load, a reinitialise that failed, a page partly saved - has a style of its own and stays up
  for eight seconds. A header or a cell too narrow for its text ends in an ellipsis and shows the whole of it as a
  tooltip. A list's Delete is one style, red words while it can be pressed and grey while it cannot; Incoming
  relays' and IP ranges' Properties and Delete need a selection, as every other list's do. The Domains page's
  alias and list boxes have captions. The **Setup checklist** numbers its items in the order a new installation
  meets them and marks the first one that needs action as the next step, and each item's button names the page it
  opens; the dashboard's attention card numbers and marks its items the same way
  ([SetupChecklist.md](hmailserver/docs/SetupChecklist.md),
  [ControlPanelDesign.md](hmailserver/docs/ControlPanelDesign.md)).
- **The Control Deck's census** ([DeckParity.md](hmailserver/docs/DeckParity.md)) counts 498 of the 508
  properties the Control Panel writes as writable over REST and reachable from a Deck view; the ten it lists as
  missing are an account's directory fields and its external ID, and the server's country policy. The census had
  missed 16 COM members whose help string holds a `]`.

### Operations

- **Packages for Debian, Ubuntu, Fedora, RHEL and openSUSE, the AppImages and both Windows installers, built on
  GitLab CI** on the project's own runners, never for a merge request, and with no tool from GitHub:
  - **Debian and Ubuntu**: Debian 12 and 13 and Ubuntu 24.04 for amd64 and arm64, and Ubuntu 26.04 for arm64, as
    `hmailserver_<v>-1~<distro>_<arch>.deb` - Ubuntu 26.04's as `hmailserver_<v>_arm64.deb`; the amd64 package for
    Ubuntu 26.04 is the release's own, below ([PackagesDeb.md](hmailserver/docs/PackagesDeb.md)).
  - **RPM**: Fedora 44 and 43, RHEL 10 and 9, and openSUSE Leap 16.0 and Tumbleweed, for x86_64 and aarch64, as
    `hmailserver-<v>-1.<disttag>.<arch>.rpm`, with Boost linked in and every other `Requires` resolved from the
    release's own repositories; the executables are PIE with full RELRO, the stack protector, stack-clash
    protection and `_FORTIFY_SOURCE=2` ([PackagesRpm.md](hmailserver/docs/PackagesRpm.md)).
  - Each `.deb` and `.rpm` is built in its distribution release's own container with that release's compiler and
    libraries (the arm64 and aarch64 ones cross-compiled), then installed in a clean container of the same release
    and architecture, run and removed. They are built for every `v*` tag and `batch*` branch, and on `master` when
    started by hand.
  - **AppImages** for x86_64 and aarch64, built on Ubuntu 24.04, so for a host with glibc 2.39 or later and
    OpenSSL 3, with a runtime of the project's own (`platform/packaging/appimage/runtime.c`) in place of the
    AppImage project's, which is published only on GitHub, and without linuxdeploy; each is run again on
    Fedora 44. They are built for a `v*` tag, and elsewhere when started by hand
    ([PackagesAppImage.md](hmailserver/docs/PackagesAppImage.md)).
  - **Windows**: the x64 and the ARM64 installer, unsigned, in every pipeline of `master`, a `batch*` branch or a
    `v*` tag, after the Windows suite. The ARM64 job may fail without failing the pipeline, and in this release it
    does: its toolchain check looks for Visual Studio's ARM64 C++ runtime by a path that finds it in no
    installation, so it stops on the Windows runner, which has the ARM64 tools, ATL and SDK libraries, and prints a
    command that changes nothing. The check is corrected for the next release
    ([PackagesWindows.md](hmailserver/docs/PackagesWindows.md)).


- **What the release carries.** The release collects the x64 installer built by its own `windows-installer` job,
  the amd64 `.deb` and the x86-64 `.rpm` that `linux-build` makes (clang, in the CI's Ubuntu 26.04 image), the
  `hmailserver-linux-<v>-SHA256SUMS.txt` of those two, the server's SPDX and CycloneDX SBOMs and each compiled
  file's own `<file>.cdx.json`, every file with its Sigstore bundle (`build/ci/release-chain.py`). The packages
  above - every other `.deb` and `.rpm`, the AArch64 ones, the AppImages and the ARM64 installer - are CI job
  artifacts only, kept for 7 to 30 days: they are not signed, not in the release and not offered by the update
  feed. A server installed from one of the other x86-64 `.deb` or `.rpm` packages is offered the release's by its
  live update, whose dependencies its distribution may not meet; apt or dnf then refuses it, and the server stays
  as it was. One installed from an AArch64 package, an AppImage or the ARM64 installer is told that the release
  carries nothing for it to download.

### Build and tests

- **Reproducible builds on both platforms** ([ReproducibleBuilds.md](hmailserver/docs/ReproducibleBuilds.md)).
  `hMailServer.exe` builds to the same bytes from a checkout at any path: the Release configuration now trims the
  checkout's root and the libraries' root from every path the compiler writes (`/d1trimfile`), where it trimmed
  only the project's own directory, so that sixteen of the server's source files and 31 Boost headers left their
  absolute paths in the executable. 6.3.4 and the releases before it reproduce, if at all, only from a checkout and
  libraries at the paths they were built at. On Linux a new `linux-repro` job builds the server, the `.deb` and the `.rpm` a second time - from a copy of
  the tree at another path, without `.git` and with other file modes, in another build directory, under another
  umask and time zone - and fails unless all three are byte for byte `linux-build`'s; the release waits for it.
  For that, the packages now have the same modes whoever makes them - their directories 0755, the `.deb`'s
  maintainer scripts 0755 and its `conffiles` 0644 - and the `.rpm` records the commit's time as its build time
  and `reproducible` as its build host. Not claimed: the installer, the Updater and Minidump helpers, the .NET
  programs and the signatures. The release's runbook builds the executable once more from a second checkout
  (RELEASE.md, step 8).
- **Coverage on Linux, with branches.** `build/ci/linux-coverage.sh` builds the server and its tester programs with
  clang's source-based coverage (`-DHM_COVERAGE=ON`, a new CMake option), runs the testers and the whole Linux
  regression suite against that server, and writes llvm-cov's line, region and branch figures; a run of the suite
  that did not reach its end is void. On 26 September 2026 on linix they were 49.01%, 47.23% and 38.87% of the
  Linux build (43.35%, 41.38% and 33.82% before the fixtures below were linked, which were measured in a run of
  their own and the counters merged) - a floor, since the Linux suite runs what its REST shims can drive and
  llvm-cov counts every line of a function. The `linux-coverage` job runs it weekly once the owner creates the
  schedule described `coverage` ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), *The
  coverage schedule*); it has not run on GitLab yet. The Windows measurement of 15 September is in the tree as
  `build/coverage-windows.ps1`, not yet run from it; it stores the suite's resolver over COM before the console
  server starts, as the suite asks since the settings moved into the database. Seventeen class constants the
  server passes to `Format` by reference are defined now (`static constexpr`): six of them kept the instrumented
  build from linking, and the other eleven had the same defect and linked only because the optimiser folded them.
- **Twenty-nine more fixtures run on Linux**, among them DKIM signing, ARC sealing, the OpenPGP keys, message
  recall, shared address books, the IMAP mirror, journaling, per-domain relay and the webmail's footers: each
  already compiled against the REST shims and ran clean on linix, and none was linked. `API/RestApiRouteSweep` is
  new, in both suites: every administrative operation under `/api/v1/` that the server's OpenAPI document
  describes is refused, with 403, to an account without a role, and every read is answered to an auditor without
  a server error. The Linux shims give `Domains.ItemByName[...]` and `DistributionLists.ItemByAddress[...]` as
  indexers, and `Paths.Combine` reads a relative path written the Windows way as one on Linux. The `linux-suite`
  job, which the fixtures lengthen by about 20 minutes, has the runners' four hours now rather than three, and its
  test filter is a variable of its own, `HM_LINUX_SUITE_FILTER`, which the job refuses, so a variable meant for
  another job cannot make the gate run part of the suite.
- **The maintainers' tools**:
  - `build/scan-secrets.py`, which the git hooks and the landing script run: the directory its scanner's container
    writes to is inside a private (0700) directory, locally and over ssh. 6.3.4 made the whole directory 0777, so
    that on a POSIX machine another account could, while a scan ran, read the lines being committed, plant a link
    the writes followed, or put an empty report in place of the scanner's (a Semgrep finding accepted for 6.3.4 as
    a real defect of low severity). And a scan that did not run to its end no longer passes: gitleaks 8.30.1
    writes an empty report and exits 0 when the `git log` it reads fails, so a `--range` naming a commit the
    repository does not have scanned nothing and said nothing was found. Such a range is now refused before any
    scanner is asked, and an empty report is clean only when gitleaks also exited 0 and logged no error.
  - `build/ci/repo-hygiene.py`'s Token-Permissions reads CI rules as GitLab does - every alternative of a
    condition, a rules list at any indentation, tags and `extends` in every form, templates and includes, and a
    job's rules from the last of its `extends` parents, which GitLab merges last over first - and refuses what it
    cannot read; its `--self-test` holds it to 33 cases. Its note on each PostgreSQL password written in
    `.gitlab-ci.yml` names the job whose service container it is.
  - New checkers: `build/check-idl-case.py` fails a COM name that `hMailServer.idl` spells two ways - a type
    library keeps one spelling of a name whatever its case, so a new member spelled like an existing one would
    rename it for every early-bound client - with the seven that shipped that way listed and why;
    `build/check-db-settings-coverage.py` (under Administration). `build/check-deck-parity.py` reads a member
    whose help string holds a `]`, and `build/ci/security-findings.py --reasons` writes the reason each finding
    is known rather than fixed into the baseline.
  - The installer smoke test takes an upgrade that writes `[Security] AdministratorPassword` again as a PBKDF2
    hash, which is the documented upgrade of an older hash, and still requires every other `[Security]` key as it
    was. And it now makes the checks [SecureDefaults.md](hmailserver/docs/SecureDefaults.md) lists for an
    installation on a clean machine: after a new installation, the certificate made at installation (its record,
    its two files, a key only SYSTEM, the administrators and the service can read, RSA 2048 for 825 days naming
    the host), STARTTLS on ports 25, 587, 110 and 143, authentication refused without TLS from the *Internet*
    range and not from *My computer*, the audit rows and the setup checklist; after an upgrade, the ports and
    ranges exactly as they were. Against the signed 6.3.4 installer on a clean Windows Server 2025: a new
    installation passed 59 checks and an upgrade from 6.3.3 73, none failed. The run script follows the VM to the
    address its DHCP lease moved to after a snapshot revert.
- **The CI**: `secret_detection` failed `master`'s first pipeline after 6.3.4 on the 24 vetted test keys and
  fixtures, because a pipeline started by a push scans the checkout as files and names a finding by its path, not
  its commit; `.gitleaksignore` now names each of them that way too. The `static-analysis` and `semgrep`
  baselines are written again from runs over the release's tree in the jobs' own images - 1,165 and 122 known
  findings, 264 and 37 of them with the reason each is known rather than fixed, and every finding no scan reports
  any more removed - and a clang-tidy that did not finish on a file (an abort, a kill) is run again alone and
  fails the job if it still does not finish, where before the file's findings read as fixed and the job passed. The package builds no longer read `$HM_PIPELINE`, which
  in a pipeline where the variable was defined would have refused every build and left GitLab making no pipeline
  for `master`, a batch or a tag; `build/check-release-chain.py` now reads the four package files and fails such a
  rule.
- **The documents, corrected**: 6.3.4's own notes - it was published on 27 September, not 26; the security
  exceptions file accepted 455 findings at the release, not 169; and `ScriptAllowedObjects` first exists in
  6.2.25, so before it an event script could create any COM object. `ServiceAccountName` and
  `ServiceAccountPassword` are no longer used, though still loaded
  ([ServiceAccount.md](hmailserver/docs/ServiceAccount.md)); the update feed's
  site has served over HTTPS since 26 September 2026 ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)); an
  inbound webhook's URL writes an international domain name in its `xn--` form
  ([InboundWebhooks.md](hmailserver/docs/InboundWebhooks.md)), and before 6.3.4's fix the check let a URL outside
  printable ASCII through on Linux and Windows alike, where 6.3.4's notes said Linux refused it; and
  CONTRIBUTING.md's schema-upgrade gate and ContinuousIntegration.md's list of what is still done by hand say what
  the jobs do. Two of these, ServiceAccount.md's and UpdateFeed.md's, were found by the check of the wiki's copies
  ([GitLab issue #25](https://gitlab.com/Progressiverobot/hmailserver/-/issues/25)).

Full notes: [the 6.3.5 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.5).

---

## [6.3.4](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.4) - 2026-09-27

Everything that landed on `master` after the `v6.3.3` tag, between 15 and 26 September 2026 ([compare `v6.3.3` with
`v6.3.4`](https://gitlab.com/Progressiverobot/hmailserver/-/compare/v6.3.3...v6.3.4)). The schema moves from **6040
to 6104 in 64 steps**. Most of what is new is off until an administrator turns it on; what changes on upgrade
without a switch comes first. The operator documents named below are in `hmailserver/docs`, which has 90 more of
them than it had at 6.3.3. This release carries an Authenticode signature and no Sigstore bundle, so every server
installs it by hand: a live update of any version that is offered it says it cannot be verified and downloads
nothing ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)). Its `hmailserver-6.3.4-SHA256SUMS.txt` names every
file of the release but itself and its signature, and is signed with the release tag's SSH key, `hmailserver-6.3.4-SHA256SUMS.txt.sig`;
SECURITY.md, "Verifying a release", has the two commands that check a download against it.

### What changes on upgrade without a switch

1. **The database is the settings store, and the `[Settings]` section of `hMailServer.ini` is its copy** (schema
   6042). At the first start the section's keys are taken into the database, with nothing to run by hand; a line
   edited after the old server last started, including one edited to prepare the upgrade, is taken for an edit and
   put back, so make such a change after upgrading. From then on a line edited in the file is ignored in favour of
   the stored value, named in the error log as `HM5804`, and put back; deleting a line no longer returns a setting
   to its default. Under a service account that cannot write `hMailServer.ini` - `NT SERVICE\hMailServer`, a new
   installation's - the copy is not refreshed and the database's values are used, said once a start in the
   application log. A setting is changed in the Control Panel, over COM (`Settings.SetIniSetting`), over REST (`PUT
   /api/v1/settings/ini/{name}`), or on Linux with `hmailserver --set-setting NAME=VALUE`. A `[SettingsOverride]`
   section in the file is the deliberate way round a database that cannot be read, and is named as `HM5807` at
   every start. No new key is added to `[Settings]` from now on: a new setting is a row in `hm_settings`.
2. **Most saved settings take effect at once.** Every `[Settings]` key now says when a save applies it - 211
   at once, 28 at a reinitialise, 2 at a restart - and the Control Panel, the Control Deck, COM and REST list
   the ones still waiting ([SettingsApply.md](hmailserver/docs/SettingsApply.md)).
3. **SPF is RFC 7208**, ported from upstream PR 645, in place of the RFC 4408 library the server had carried.
   127.0.0.0/8 and `::1` no longer pass unconditionally before any policy is read, so where the SPF test or
   DMARC runs for a loopback client, mail from a domain whose policy does not name the address is judged like
   any other; a content filter that re-injects mail should authenticate or connect from an IP range with
   anti-spam off. SPF lookups follow the server's own resolver settings on Windows, and work on Linux, where
   every check had been TempError. Every result is written under its own keyword; only a fail scores, as
   before.
4. **The audit trail is on** (schema 6043): an administrative change over COM or REST writes a row, kept
   until a retention is set, with secrets recorded as `(changed)`. **Alerts are on**, with seven of their
   eleven conditions enabled, `backup.failed` and `certificate.expiring` among them, but nothing is mailed
   until `AlertRecipient` and `AlertSenderAddress` are set
   ([AlertsAndAuditTrail.md](hmailserver/docs/AlertsAndAuditTrail.md)).
5. **Compromised-account detection starts in watch mode** (schema 6071): it builds each account's baseline
   from authenticated submissions and raises `account.compromised` when a submission has the shape of a
   takeover, and holds nothing until `compromise_detection_mode` is set to `enforce`
   ([CompromisedAccounts.md](hmailserver/docs/CompromisedAccounts.md)).
6. **A destination that asks this server to slow down is given a pause.** A 421, or a 4xx with one of the
   providers' rate-limit codes or phrases, holds deliveries to that destination alone for
   `OutboundBackoffMinutes` (2 by default, doubling to `OutboundBackoffMaxMinutes`, 60); 0 turns the back-off
   off. A wait this server chose no longer spends a message's retries until the message has been queued
   for longer than its retry schedule spans, so a destination that never stops asking still ends in a bounce
   ([OutboundShaping.md](hmailserver/docs/OutboundShaping.md)).
7. **A backup carries what an account keeps beyond its mail** - the address book, the webmail's preferences,
   scheduled sends, file links, S/MIME keys, calendars, password history, the first-contact memory and every
   message's keywords - and a restore puts them back, with each folder's access grants. Passkeys, calendar and
   address-book shares, deleted mail waiting to be recovered, messages that expire, IMAP annotations and the
   out-of-office memory are not carried, each for the reason `AccountStores.cpp` gives. An archive written by
   6.3.3 or earlier restores exactly as it did, without what it never held.
8. **The out-of-office memory survives a restart** (schema 6060): who has been answered is kept in a table,
   so a restart no longer sends every correspondent the reply again.
9. **Addresses collected from what a person sends are suggestions, not contacts.** They appear under Other
   contacts in the webmail, and CardDAV serves only the contacts a person kept.
10. **In the container image, the environment wins.** `HM_SETTING_<key>` and `HM_PROPERTY_<name>` (with `_FILE`
    variants) are stored at every start, and a setting the environment manages is refused to the other
    interfaces until a start without the variable
    ([EnvironmentConfiguration.md](hmailserver/docs/EnvironmentConfiguration.md)).
11. **`GET /api/v1/me/search` goes on from where it stopped.** A search with more hits than `limit` answered
    `complete: true, more: true` and could not reach the rest; it now answers `complete: false, more: true`
    with `next_before_id`, which `before_id` continues from, and `complete` is false with a
    `next_before_id` whenever an answer is not the end. Hits are newest first across every folder by message
    id, where they were folder by folder ([Performance.md](hmailserver/docs/Performance.md)).
12. **IMAP answers as the RFCs say, where Dovecot's imaptest found it did not.** A client that relied on the old
    answers sees these: a mailbox that does not exist, or a right the account lacks, is refused `NO` rather than
    `BAD`, and a failed SELECT or EXAMINE leaves no mailbox selected; an unknown LIST selection or return option is
    `BAD`; every `BODY[<section>]` without `.PEEK` sets `\Seen`, `BODY[HEADER]` and `BODY[1.MIME]` included; SORT
    uses every criterion, REVERSE reverses only its own, ties go to sequence order and an ASCII string folds ASCII
    letters only (a string beyond ASCII is compared as before), and THREAD ORDEREDSUBJECT and REFERENCES build the
    RFC's trees, so both can order messages differently; a session that is the first to be told of new messages
    counts them as `\Recent` (RFC 3501 2.3.2), where it said `* 0 RECENT`; and an APPEND stores the literal byte
    for byte, where it used to add a CRLF, so RFC822.SIZE of a newly appended message is two octets smaller than
    the same message appended before.
13. **MySQL and MariaDB tables become utf8mb4** (schema 6102). Every table is converted to utf8mb4 under
    `utf8mb4_general_ci`, InnoDB and `ROW_FORMAT=DYNAMIC`, and so is the database's default. Before it, 106 of the
    123 tables were utf8, three bytes a character, so an emoji or a CJK Extension B character in a contact, a
    subject in the message index, a signature or a log line was refused with error 1366 or made a question mark.
    The step rebuilds every table, which takes time on a large database; it needs MySQL 5.7.9 or MariaDB 10.2.2 or
    later and the ALTER privilege on the database, and a step stopped part of the way is finished by running it
    again. So do steps 6041 and 6101 before it, and each now opens with a probe table that needs the same key size,
    so on an older server the upgrade stops at 6041 with error 1071 having changed nothing. Fixed: those two steps
    made their first tables before the index that failed, and MySQL commits DDL at once, so they stopped half done
    and every later run failed on a table already there; a database an earlier build left that way, at 6040 (or,
    after a change of database server between the two steps, at 6100), is put back by hand with the statements
    Upgrading.md gives. Two rows the old collation told apart and the new one does not - on MariaDB 11.8, `straße` and `strase`
    in one address book; on MySQL 8.4, two names that differ only in an emoji - stop it with error 1062 until one
    of them is removed. SQL Server, SQL Server Compact and PostgreSQL only move the version
    ([Upgrading.md](hmailserver/docs/Upgrading.md),
    [MigratingDatabaseBackend.md](hmailserver/docs/MigratingDatabaseBackend.md)).
14. **The upgrade alters the message table.** Schema 6101 adds two columns to `hm_messages`, the table that has a
    row for every stored message: the account an authenticated session submitted a message as, and the domains that
    passed DMARC and SPF when it arrived, recorded as properties of the message and never as headers. On a large
    store the step is not instant on every backend.
15. **The database connection is encrypted when the database server offers TLS.** MySQL and MariaDB through MariaDB
    Connector/C - the client bundled on Windows and the one Ubuntu packages - had never used TLS, whatever the
    documents said; with nothing configured they now use it whenever the server offers it, without checking the
    certificate, and connect without it when it offers none. SQL Server through MSOLEDBSQL 18 and SQL Native Client
    now encrypts the whole session, and tries once more without encryption when the failure reads as TLS; through
    SQLOLEDB, which the server picks when MSOLEDBSQL 18 or later is not installed and no failover partner is set,
    only the login is encrypted unless `TlsMode` is `require` or `verify-full`. PostgreSQL is unchanged. Two
    `[Database]` keys choose otherwise: `TlsMode` (`prefer`, the default, `require`, `verify-full` or `disable`)
    and `TlsCaFile`. The application log says at every start what the connection got, and `--check-database`
    makes one connection and says the same
    ([DatabaseTls.md](hmailserver/docs/DatabaseTls.md)).
16. **The administrator password is hashed again at sign-in.** The Windows installer wrote it as an unsalted MD5
    digest that nothing ever replaced. The first successful administrator sign-in - the Control Panel, COM, REST,
    `hmctl` or the Control Deck - writes an MD5 or salted SHA-256 value, or a PBKDF2 hash with fewer iterations
    than `PasswordHashIterations`, again as PBKDF2; a browser session made under the old hash ends once. A file the
    service cannot write keeps the old value and says so once (`HM7494`). An upgrade seals nothing else: see
    Encryption ([SecretsAtRest.md](hmailserver/docs/SecretsAtRest.md)).
17. **A COM change to a shared configuration object takes effect at `Save()`.** For routes and their addresses,
    incoming relays, DNS black lists, SURBL servers, remote-domain policies, server messages, blocked attachments,
    SSL certificates and groups, a `put_*` now writes a copy held by the COM object, not the item every session is
    reading, so it is not in effect until `Save()`, and one never saved changes nothing; an object read from the
    collection before the save keeps what it read. A script that set one of these and relied on it without saving
    must now save. The old way let a session read a string while it was being freed, the use-after-free under the
    heap-corruption abort below; a save whose re-read fails is reported as error 7390
    ([Upgrading.md](hmailserver/docs/Upgrading.md)).
18. **Mail labelled ISO-8859-1 is decoded as windows-1252**, as the WHATWG Encoding standard and every mail client
    read it, and ISO-8859-2 to -9, -13 and -15 by their own code pages. There was no ISO-8859 name in the server's
    table at all: a Western Windows server decoded Latin-1 right by accident and decodes it as before, one with
    another ANSI code page now decodes it right too, and on Linux Latin-1 had become replacement characters.
19. **A restore lists the archive before it extracts anything, and refuses an unsafe one whole**: an entry with an
    absolute path, a `..` segment or a link with somewhere to point is named in the backup log and as `HM7540`, and
    nothing is extracted. No backup this server writes has such an entry. The Windows installer's 7-Zip is 26.03,
    fetched from 7-zip.org, in place of 19.00, which predates the fixes for CVE-2023-31102 and CVE-2025-55188
    ([Backup.md](hmailserver/docs/Backup.md)).
20. **Event scripts: a rule's Run function action names a plain function.** A name that is not ASCII letters,
    digits, `_` and `$`, not starting with a digit, is refused as `HM5018` each time the rule fires, in either
    Windows language, VBScript or JScript, where it used to be written into the script's call as it was. On Linux,
    whose JavaScript engine looks a handler up by name instead of building a call, such a name is never evaluated
    and finds no function: nothing is run, and the ERROR log has a Script Error line, not `HM5018`
    ([EventScripts.md](hmailserver/docs/EventScripts.md)). On a JScript server every value a
    handler is given - a password, a remote UID, an error text - now arrives exactly as it was, backslashes
    included (see the fix under Security and identity).
21. **A domain can no longer be named as a directory the server keeps its own files in.** A domain's mail is kept
    in the directory named after it under the data directory, beside the server's stores, so a single-label name
    such as `protected`, `confidential`, `recoverable`, `discovery`, `quarantine`, `files`, `lists` or `blobs` (in
    any case), or `temp`, `events` and `database`, which the Linux packages put there, is refused for a new domain
    and for a rename over COM and REST. A domain that already has one is still saved, and is named as `HM7597` at
    every start and as `domain_name_reserved` on the first-hour checklist: rename it
    ([SetupChecklist.md](hmailserver/docs/SetupChecklist.md)).
22. **A one-time code is taken once.** Over COM and over REST, a TOTP code the server has accepted is refused if
    it comes again, for the administrator and for an account alike (see the fix under Security and identity). A
    script that sent the administrator's password and a code with every REST request signs in once instead -
    `POST /api/v1/session`, and the `hmailsession` cookie from there - or uses an API key; otherwise only its first
    request in each half minute is accepted. The others are refused with 401 and are not counted as wrong codes, so
    such a script does not lock the administrator out. Somebody asked for a code twice within half a minute - to sign in to
    the Control Deck and then to register a passkey, say - waits for the authenticator's next one
    ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
23. **The outbound relay sends its OAuth2 token over TLS only.** A route or the SMTP relay whose host is on
    `OutboundOAuth2Hosts` and whose connection security is none - or STARTTLS optional against a server that does
    not offer it - sent `AUTH XOAUTH2` with the server's own token in the clear. Such a delivery now sends
    nothing in its place, fails with a temporary `450 4.7.0`, and the error log says to set the security to TLS
    or STARTTLS required (`HM5912`); the message waits for the retries the route allows. Exchange Online offers
    STARTTLS on 587, so a route set up as Microsoft documents it is not affected.
24. **A password hash stored before 6.3.4 of a password with a character above U+00FF is refused**, to its own
    password too, and the application log names the account: that hash was of NUL bytes or question marks, and
    any other password of that form opened it (see the fix under Security and identity). An administrator sets
    such a password again, and soon: until then SCRAM-SHA-256, whose client derives its key itself, still opens
    such a PBKDF2 hash to the NUL bytes (Windows) or question marks (Linux) it was made over, which the server
    cannot tell from the password. Every ASCII and Latin-1 password signs in as before.

### Delivery and SMTP

- **Remote-domain policies** (schema 6045): for one remote domain or a pattern, the outbound TLS it demands
  (encrypted, verified or DANE - a delivery that cannot meet it is deferred, never sent in the clear), TLS
  required of its senders, size, connection and rate limits, whether out-of-office replies, forwards and Sieve
  redirects may go to it, and a backup-MX recipient callout
  ([RemoteDomains.md](hmailserver/docs/RemoteDomains.md)).
- **Throttling per destination and per tenant**: besides the back-off above, `MaxOutboundConnectionsPerDestination`
  and `MaxOutboundConnectionsPerTenant` (both off) so that one busy destination or one hosted domain cannot
  hold every delivery thread; the state is at `GET /api/v1/queue/shaping`.
- **IP warm-up** (schema 6093): a schedule for a new outbound address holds it to a daily ceiling of recipients
  at each large provider, rising along a standard, gentle or fast plan; nothing is held until a schedule exists
  ([IpWarmup.md](hmailserver/docs/IpWarmup.md)).
- **A send API for applications** (schema 6089): an API key becomes a sender, with an address of its own and
  ceilings a minute and a day; `POST /api/v1/send` sends and `GET /api/v1/send/{queue_id}` says what became of
  each recipient ([SendApi.md](hmailserver/docs/SendApi.md)).
- **DKIM keys that rotate** (schema 6048): a policy per domain publishes a second selector, switches once the
  world has seen it and retires the old key, with an alert when a rotation stalls; off on every domain
  ([DkimKeyRotation.md](hmailserver/docs/DkimKeyRotation.md)).
- **What a domain does to a message** (schema 6044): an external-sender tag on the subject or as a header, a
  first-contact note, and a disclaimer on mail leaving the organisation, all off; the domain's signature and
  disclaimer now also go on mail sent from the webmail and its send-later
  ([DomainTransforms.md](hmailserver/docs/DomainTransforms.md), [Signatures.md](hmailserver/docs/Signatures.md)).
- **Restricted delivery** (schema 6084): accounts, or a whole domain, that may exchange mail only with named
  domains ([RestrictedDelivery.md](hmailserver/docs/RestrictedDelivery.md)).
- **Message recall within the server** (schema 6085): the sender withdraws an unread message delivered to
  accounts on this server ([MessageRecall.md](hmailserver/docs/MessageRecall.md)).
- **Journaling and dual delivery** (schema 6075): a copy of a domain's or one account's mail sent to an
  address, as a journal report or as the message itself ([Journaling.md](hmailserver/docs/Journaling.md)).
- **Quota policy** (schema 6061): a domain's own warning threshold and text, a grace past the limit for
  incoming mail, and a share of the limit at which the account can no longer send
  ([QuotaPolicy.md](hmailserver/docs/QuotaPolicy.md)).
- **Out-of-office policy** (schema 6060): a window of days before the same sender is answered again, a text
  for colleagues and replies to colleagues only ([OutOfOffice.md](hmailserver/docs/OutOfOffice.md)).
- **Mail in, to a webhook** (schema 6100): an address, or a whole domain this server hosts, is routed to a URL, and
  the mail that arrives for it is posted there parsed - as `multipart/form-data` in SendGrid's inbound parse field
  names or as one JSON document - signed with the outbound webhooks' HMAC-SHA256 recipe and retried on their
  schedule; when the attempts run out a route keeps the message to be downloaded or retried, or returns it to its
  sender. A route may deliver to the mailbox as well. Link-local and cloud metadata addresses are refused to
  everybody, an API key may name only a public address, and plain `http` is taken only to this machine. A URL is
  printable ASCII, and anything outside it is refused on both platforms - fixed before release, when Linux and
  Windows alike put such a URL into the request line as it was. Routes are
  made over REST (`/api/v1/inbound-webhooks`) and on the Control Deck, under a new permission,
  `manage_inbound_webhooks`, that only `serveradmin` holds; there is no COM member and no Control Panel page.
  Nothing is on until a route is made ([InboundWebhooks.md](hmailserver/docs/InboundWebhooks.md)).
- Fixed: an outbound bind address of one address family no longer stops delivery to the other (GitHub issue
  276); `SMTPDeliveryBindToIP` may name one address of each family, separated by a comma.

### Anti-spam, anti-virus and mail flow rules

- **Greylisting hardened** (schema 6072; GitHub discussions 281, 282 and 284): the triplet key no longer
  truncates long addresses; a sliding wait on an early retry and an abuse lock, each off by default; and a
  bypass list whose entries may require SPF, DKIM or DMARC to pass
  ([Greylisting.md](hmailserver/docs/Greylisting.md)).
- **SMTP policy a score cannot express** (schema 6073; GitHub discussions 283, 285 and 286): scores of their own
  for SPF softfail, permerror and temperror, a temperror deferral, a ban or refusal for a null sender with many
  recipients, and a domain white and black list by envelope domain or HELO; all off
  ([SmtpPolicy.md](hmailserver/docs/SmtpPolicy.md)).
- **Allow and block lists and quarantine digests** (schema 6046): senders or domains allowed or blocked for the
  server, a domain or one reader, covering the From header's address as well as the envelope sender, and URL
  entries for the links in a message; held mail summarised daily to the person it was
  held for, once `QuarantineDigestBaseUrl` is set and a domain or an account turns the digest on (off by default)
  ([SenderListsAndDigests.md](hmailserver/docs/SenderListsAndDigests.md)).
- **Mail flow rules** (schema 6063): a domain's own rules, a condition on an attachment's name, and actions to
  add a disclaimer, reject with a text, require TLS, quarantine or warn the sender; and an **outbound content
  policy** - card numbers, IBANs, account numbers and attachment types, to external recipients - as conditions
  of the same rules, with no second engine ([MailFlowRules.md](hmailserver/docs/MailFlowRules.md),
  [OutboundContentPolicy.md](hmailserver/docs/OutboundContentPolicy.md)).
- **Impersonation protection** (schema 6087): protected persons and partner domains per domain, judged on mail
  from outside; off on every domain ([ImpersonationProtection.md](hmailserver/docs/ImpersonationProtection.md)).
- **Attachments opened somewhere safe** (schema 6091): an outside message whose attachment is an executable, a
  script, a shortcut, an archive, a disk image, an office document or a PDF is held until a sandbox service the
  administrator names answers;
  off ([Sandbox.md](hmailserver/docs/Sandbox.md)).
- **Spam traps** (schema 6058): an address that was never real refuses like an unknown user, bans the connecting
  address and, when its domain's SPF passes for that address, puts the envelope sender on the block list
  ([SpamTraps.md](hmailserver/docs/SpamTraps.md)).
- **Country blocking** (schema 6052): connections and sign-ins judged by country from a MaxMind database the
  administrator supplies, with loopback and private ranges exempt and a missing database meaning allow
  ([CountryBlocking.md](hmailserver/docs/CountryBlocking.md)).
- **Sealed mail** handled honestly: a message the server cannot read is no longer gutted by the
  blocked-attachment list or given a clean verdict by a scanner that saw only ciphertext
  ([SealedMail.md](hmailserver/docs/SealedMail.md)). "Cannot read" is judged from the body, never from the
  `Content-Type` label: only a message that really is S/MIME enveloped or signed-data, or PGP/MIME of exactly a
  version part and armoured ciphertext under a session key, named as clients name them or not named at all, is
  spared the blocked-attachment list, the anti-virus strip action, eDiscovery's body search and the rules' body, `content`
  and `sensitive_content` conditions. Fixed before release: as first written, a `multipart/encrypted` header any
  sender could add switched all of those off for the ordinary parts under it, `invoice.exe` included. The
  detached signature of a signed message is likewise left alone only when it is one - the second of the two
  parts of a `multipart/signed`, called and made as a signature is; as first written, any part anywhere that
  typed itself `application/pkcs7-signature` or `application/pgp-signature` was spared the list, whatever it
  was called.
- Fixed: a message held by this server's own outbound shaping - a destination's back-off, a rate or connection
  ceiling, the tenant share, the IP warm-up - no longer sends a server or domain rule's forward, copy or reply,
  or calls its script function, once per held pass. Each pass re-ran the rules, and each queued the new message
  again, so a message held every few minutes for as long as its retry schedule spans sent one copy per pass;
  those actions now run once per counted attempt. An account's own rules, which run once for their recipient,
  are not held back by this, and an account rule's warn_sender now goes when its recipient is reached on a later
  attempt too - it was skipped, as the server's once-per-message notice is
  ([MailFlowRules.md](hmailserver/docs/MailFlowRules.md), [OutboundShaping.md](hmailserver/docs/OutboundShaping.md),
  [OutboundContentPolicy.md](hmailserver/docs/OutboundContentPolicy.md)).
- Fixed: a mail flow rule's forward, copy, reply or notice to the sender, and an account's own forwarding, no
  longer wait up to a minute in the queue. Each is queued from inside the delivery of the message that caused
  it, after the delivery manager last read the queue, and the manager was not told of it, so it sat there until
  something else woke the manager, a minute later at the latest.
- Fixed: one domain's mail flow rules no longer act on another domain's recipients. A domain's rules ran on the
  whole of every message with a recipient in the domain, and on every message whose envelope sender claimed the
  domain, authenticated or not, so one tenant's delete or reject removed a message for another tenant's
  recipients, and its header, route or bound address applied to their copies. The sending domain's rules - the
  submitting account's domain, and the envelope sender's where its account submitted the message, DMARC passed
  for it, or the message reaches no other domain's mailboxes - still act on the whole message; a recipient
  domain's act on the whole message only when every recipient is its own, and otherwise take its own recipients
  off (a quarantine keeps a copy for them, and a reject mails a hosted sender a notice naming them), copy to them
  alone and leave the message's headers, body and routing alone
  ([MailFlowRules.md](hmailserver/docs/MailFlowRules.md)).
- Fixed: a custom virus scanner whose command line holds a bare `%FILE%` was handed the message file's path
  cut at its first space - and the default data directory, under `C:\Program Files`, has one - so it scanned no
  file, answered with an exit code other than the virus one, and the message was delivered as clean. A `%FILE%`
  outside double quotes is now replaced by the path in quotes; one already inside them, `"%FILE%"` as the
  documented examples write it, is replaced as before. ClamWin's database and temporary folders lose a trailing
  backslash, which escaped the closing quote of `--database="..."` and took the file to scan with it.

### IMAP, POP3 and the webmail

- **Twelve webmail features**: installs as an app with the unread count on its icon, shares from other apps and
  `mailto:` links; contacts imported and exported as vCard or CSV; saved searches as folders; the reader's own
  time zone; Undo on three more actions; the tracker notice counts what it blocked; swipe actions and snooze
  presets; three stars (`$Star2`, `$Star3` on top of the flag); rows coloured by a rule; `@`-mentions completed
  from the address book; a formatted signature with an image; and Report phishing beside Junk.
- **Arabic, Hebrew and Persian**, with the layout mirrored: 23 catalogues besides English.
- **Recover deleted items** (schema 6068): a deleted message is kept aside for `recover_deleted_days` (off by
  default) and put back by its owner ([RecoverDeleted.md](hmailserver/docs/RecoverDeleted.md)).
- **A message that expires** (schema 6069): the recipient gets a link, not the message, with an optional
  passcode ([ConfidentialMessages.md](hmailserver/docs/ConfidentialMessages.md)).
- **Offline mail** for the last N days, kept in the browser under a ceiling the administrator sets
  (`ReaderOfflineDaysMax`, 30) ([OfflineMail.md](hmailserver/docs/OfflineMail.md)); **Web Push** for new mail
  with the page closed (schema 6054, off).
- **A second account in the same page** ([SecondAccount.md](hmailserver/docs/SecondAccount.md)); the reply card
  above the conversation as a choice (GitHub discussion 266); auto-delete of Trash and Junk and auto-archive of
  the inbox as the reader's own choices.
- **Important, and sections of the inbox**, and emoji, pictures and tables in compose
  ([ImportantMail.md](hmailserver/docs/ImportantMail.md), [FormattedMessages.md](hmailserver/docs/FormattedMessages.md)).
- **Masked addresses** (schema 6067), **sending as another verified address** (schema 6066), managing
  subscriptions, and a sender's BIMI logo as its avatar for mail that passed DMARC
  ([MaskedAddresses.md](hmailserver/docs/MaskedAddresses.md), [SendAs.md](hmailserver/docs/SendAs.md),
  [BrandLogos.md](hmailserver/docs/BrandLogos.md)).
- **Calendars**: CalDAV (RFC 4791) beside CardDAV, on an iCalendar (RFC 5545) reader and writer of this
  project's own with bounded recurrence expansion (schema 6041); the webmail's calendar and tasks pages, several
  calendars with colours and shares (schema 6064), invitations by mail (iMIP), and free-busy, group and
  delegate shares (schema 6086) ([Calendar.md](hmailserver/docs/Calendar.md)).
- **Contacts**: contact groups, the organisation's directory of a domain's people and lists (schema 6078), and
  shared address books handed to a colleague or a whole domain (schema 6079)
  ([Contacts.md](hmailserver/docs/Contacts.md), [Directory.md](hmailserver/docs/Directory.md),
  [SharedAddressBooks.md](hmailserver/docs/SharedAddressBooks.md)).
- **Rooms and equipment** (schema 6101): an account can be a room or a piece of equipment (`kind` over REST, the
  Deck's account editor, `Account.Kind` over COM), which answers the invitations it is sent by itself, at delivery,
  by its booking policy - who may book, a booking window (180 days as shipped), a longest booking, working hours,
  conflicts and recurring meetings - and leaves what it does not decide to its delegates. It believes an organiser
  only from what the server recorded when the message arrived - the account that submitted it, or a DMARC or SPF
  pass for an outside domain the policy names - never from a header. A resource cannot be signed into. The
  directory lists rooms with their capacity, and CalDAV's room finder searches them. The policy is at
  `/api/v1/accounts/{address}/booking-policy`, with no COM or Control Panel page; nothing changes until an account
  is given a kind ([RoomsAndEquipment.md](hmailserver/docs/RoomsAndEquipment.md)).
- Fixed: on SQL Server and SQL Server Compact a room's record of whom it accepted a booking from was keyed on the
  UID under a collation that ignores case and width, so a booking whose UID differed from another's only that way
  replaced the other's record, and a delegate's later decline of the first booking told its organiser nothing. The
  UID is now compared exactly on every database, so the first booking keeps its record; the second, which the
  unique index there takes for the same UID, goes unrecorded, and the application log says that a delegate's later
  answer about it will tell nobody.
- **Delegated mailboxes named in the Autodiscover answer**, in Exchange's `AlternativeMailbox` element and only
  to a request signed in as the delegate. Outlook reads that element for an Exchange account and not for an IMAP
  one, so for IMAP clients the mechanism that works is still the `#Users` namespace, which Thunderbird and
  Outlook's IMAP code show by themselves.
- Fixed: a heap-corruption abort under concurrent IMAP `LIST` and folder changes on the same account; every
  loop over a shared collection now iterates a locked copy.
- Fixed: an IMAP command pipelined behind an `APPEND` with a non-synchronising literal was never answered, which
  stopped `mbsync` after one message.
- Fixed: a heap-corruption abort when two IMAP sessions on one mailbox met on the same message's keywords - a
  `STORE` of `$Forwarded`, `Junk` or a label beside a `FETCH`, a `SELECT` or another `STORE`, which is what two
  of Thunderbird's connections, or a phone and a desktop, do. Every session shares the one message object, and its
  keywords were rewritten with no lock; they are now read and changed under a lock of their own, so the abort
  cannot happen. 6.3.3 had it too.
- Fixed: two changes to one message's flags at the same moment - two IMAP `STORE`s from two sessions, a `STORE`
  beside a `FETCH` that sets `\Seen`, or beside the webmail's flag and label routes - could each write back the
  flags and keywords it had read before the other's change, so one change was lost, in memory and in the
  database. The read, the change and the write are now one step under a lock of the message's own, and each flag
  is set and cleared on its own bit. `STORE ... (UNCHANGEDSINCE n)` is tested in the same step; a `STORE` refused
  for too many keywords leaves the message as it was rather than half changed in memory and never written; and a
  flag change no longer writes back a `\Recent` another session has just claimed. As old as `STORE`.
- Fixed: an IMAP `APPEND` whose client went away after sending a message's octets and before the command's
  final line break left that message's file in the data directory for good, with no row, counted by no quota,
  and - with `MessageStoreCompression` on - holding its references to shared parts. An authenticated user could
  fill the volume by repeating it until the server refused mail for everyone. The file is now removed with the
  connection, and one left by a server that stopped in the middle of such an `APPEND` - a crash, a kill, a power
  cut - is removed when the server next starts: every such file has a marker in the temp folder's `IMAPAppend`
  directory until its row is saved, and the file of a marker no row answers goes
  ([MessageStore.md](hmailserver/docs/MessageStore.md)). 6.3.3 had it too.
- Fixed: an IMAP `APPEND` into a folder shared from another mailbox (`#Users`) was measured against the quota
  of the user appending, not of the mailbox that stores it, so a delegate with the insert right could push the
  owner past the limit, and a delegate over their own limit was refused an append to a mailbox with room. It is
  now charged to the owner, as `COPY` and `MOVE` already were. 6.3.3 had it too.
- Fixed: an IMAP `FETCH` of a message the server could not read - one in a mailbox encrypted at rest while its
  owner's key is closed, as a delegate meets it, or a damaged one (`HM7350`) - was answered `OK` with an
  `ENVELOPE` of `NIL`s, an empty structure and empty sections, which clients keep by UID for good, and a
  non-`PEEK` section set `\Seen`. It is now answered `NO [UNAVAILABLE]` for a closed key, with nothing of the
  message sent, and said in the debug log only, not the error log: a closed key is not damage. A damaged one is
  left out and the rest of the range fetched, and the command ends `NO`, as for a message lost from an object
  store, so one damaged message does not stop a whole folder's sync at itself. Every part is read before any is
  sent, so a failure can no longer leave an untagged `FETCH` unfinished
  ([EncryptionAtRest.md](hmailserver/docs/EncryptionAtRest.md)).
- Fixed: nothing bounded an IMAP `LIST` or `LSUB` pattern but the 11 MB one command may hold - a quoted pattern
  goes on over the lines a literal joins - and matching it against each folder cost at least its length on the
  threads every protocol shares: 6.3.3 copied the whole pattern twice for each, and this release's matcher, which
  finds a wildcard anywhere in a pattern, cost its length times the folder name's. So one authenticated client's
  pattern of `*` megabytes long could hold those threads. A run of wildcards is now matched as one and a pattern
  that cannot match stops at once, and a pattern longer than 8,192 characters with its reference - longer than
  any name a folder can have - or more than 32 patterns in one `LIST` is answered `BAD`. `LIST` and `LSUB` also
  read a reference or pattern sent as a literal (`{n}`), as RFC 3501 allows, and the same bound holds for it: they
  matched the literal's length as the pattern, so such a command did not list what it asked for. 6.3.3 had that
  too.
- Fixed: a message whose `Date` header held a backslash - one at its end, say - broke the IMAP `ENVELOPE` it was
  sent in: the backslash escaped the date's closing quote, so a client read the subject and the addresses the
  sender wrote as part of the date and then as the response's own structure. The date's backslashes are left out,
  as its quotes already were. 6.3.3 had it too.
- Fixed: the same for IMAP `BODYSTRUCTURE`: a part's type, charset, file name, boundary, `Content-ID` and
  disposition were put between quotes as the sender wrote them, so a file name ending in a backslash or a
  `Content-ID` holding a quote broke the structure every client parsed, and a folded `Content-ID` ended the
  response line. Their quotes and backslashes are now escaped and a control character is sent as a space; a value
  without any of these is sent as before. 6.3.3 had it too.
- Fixed: on an IMAP connection compressed with `COMPRESS DEFLATE`, what one read inflated past the connection's
  250,000-octet receive buffer was dropped without a word. A few hundred compressed octets of a repeated byte
  inflate to more than that, so such an `APPEND` stored its message cut short, or never finished, and the session
  lost step with the client. The rest now waits and is taken before the connection reads again. 6.3.3 had it too.
- Fixed: a later message of an IMAP `MULTIAPPEND` refused under a `{n+}` literal, or any literal after the command
  had failed, was read and thrown away whatever size it declared, past the 2 GB ceiling too, so a client could
  keep a connection reading for as long as it liked. Declared larger than the maximum message size (and 1 MB),
  it now closes the connection with `BAD [TOOBIG]`, as the first literal already did. 6.3.3 had it too.
- IMAP `COMPRESS DEFLATE` is taken, and `COMPRESS=DEFLATE` advertised, only once the client has signed in; before
  that it is answered `NO`. It was taken in any state, which put the inflater within reach of anyone who could
  open a connection, for no client that compresses before it authenticates.
- Fixed: IMAP `STARTTLS` was taken, and offered, after `LOGIN` and inside a TLS session it had already started,
  where it began a second handshake on the running one. It is a command of the not authenticated state only
  (RFC 9051); it is now answered `BAD` and left out of `CAPABILITY` once TLS is active or the client has signed
  in, as SMTP's `STARTTLS` and POP3's `STLS` already were. 6.3.3 had it too.
- **A folder of a hundred thousand in the webmail**: a page copies one page of the folder, never all of it;
  Oldest reaches the end in one request; a search is bounded by time as well as by files read, spared by
  the full-text index, and goes on from where it stopped across every folder; select-all hands the
  selection to the server (`POST /api/v1/me/folders/{id}/bulk`), which works through it in bounded steps.
  The seeding and timing scripts are in `build/perf/`; the numbers are to be measured on the bench
  ([Performance.md](hmailserver/docs/Performance.md)).

### Security and identity

- **Administrator roles** (schema 6047): `serveradmin`, `domainadmin`, `helpdesk` and `auditor` over 27
  permissions, custom roles, assigned per account and per domain ([Roles.md](hmailserver/docs/Roles.md)).
- **Passkeys and single sign-on** (schema 6077): WebAuthn, and OpenID Connect from the organisation's issuer,
  into the webmail and the Control Deck; **SAML 2.0** beside it (no schema step), with an XML canonicaliser and
  signature verifier of the project's own ([PasskeysAndSingleSignOn.md](hmailserver/docs/PasskeysAndSingleSignOn.md)).
- **SCIM 2.0 provisioning** (schema 6096): the identity provider makes, renames, suspends and removes accounts,
  aliases and lists at `/scim/v2` ([Scim.md](hmailserver/docs/Scim.md)).
- **A recovery address and one-time recovery codes**, and self-service password reset (schema 6076)
  ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Sessions and devices** (schema 6053): every client that signs in is a row its owner and the administrator
  can see and revoke; the session policies start unset, after an upgrade as in a new database, so the
  ceilings the server always had (thirty minutes idle, twelve hours in all) apply until they are set
  ([DevicesAndSessions.md](hmailserver/docs/DevicesAndSessions.md)).
- **A new password checked against published breaches** by k-anonymity, wherever a password is chosen; off by
  default ([PasswordBreachCheck.md](hmailserver/docs/PasswordBreachCheck.md)).
- **Compromised-account detection** (schema 6071), in watch mode as above, with a review queue.
- **Fixed: a password could run code inside the server.** On Windows, with event scripting on, the script language
  JScript and an `OnClientValidatePassword` handler in `EventHandlers.js`, any client that could reach a sign-in -
  SMTP AUTH, POP3, IMAP or anything else that checks an account's password - and name an existing, active account
  could have JScript of its own run inside `hMailServer.exe`, with the rights of the service's account, before it
  had authenticated. That script could create any COM object, `WScript.Shell` among them - before 6.2.25 always,
  from 6.2.25 with `ScriptAllowedObjects` at its shipped `*` - and so run any command the service's account may.
  The server built each call as source text and escaped the apostrophe but not the backslash before it. The same shape was open to a remote POP3
  server through the UID it answers (`OnExternalAccountDownload`), and to a remote mail server through the error
  text it refuses a delivery with and the failed recipient's address (`OnDeliveryFailed`), each on a JScript server
  with that handler; and a rule's Run function action wrote its function name into the call unescaped in either
  language, so whoever may edit a rule, a domain's own administrator included, could do the same. VBScript servers
  were not open through a password, a UID or an error text, and the Linux builds had no script engine. The code was
  there in every release from 6.0.0 to 6.3.3. Every JScript value is now escaped in one helper, and a rule's
  function name must be a plain identifier ([EventScripts.md](hmailserver/docs/EventScripts.md)).
- **Fixed: an app password was given the account's private keys, sealed under the account password.**
  `GET /api/v1/me/smime` answered a request made with an app password every own S/MIME key as the webmail
  wrapped it, under a PBKDF2 key from the account password, as 6.3.3 already did; and, new in this release,
  `GET /api/v1/me/zero-access` the zero-access mailbox key's two sealed copies with their Argon2id salt and
  parameters, and `GET /api/v1/me/openpgp/keys` every own OpenPGP secret key. Whoever held a leaked app password -
  a credential for a mail program, which cannot change any of these - could take the copies away and guess the
  account password offline, and a right guess opened the mailbox key and the account. The three reads now leave
  the keys out, with `keys_withheld`, for an app password and for an administrator's support session, so a webmail
  session started with either cannot open them and says so. A zero-access row's Argon2id parameters
  are held to at least two passes over 19 MiB, where 8 MiB and one pass were taken
  ([ZeroAccess.md](hmailserver/docs/ZeroAccess.md)).
- **Fixed: an app password could replace or remove the account's own S/MIME certificate and private key**
  (`PUT /api/v1/me/smime/own`, `DELETE /api/v1/me/smime/own/{fingerprint}`), whose wrapped key the store holds the
  only server copy of, as 6.3.3 already allowed; both are now refused to it with 403, as the same writes to an
  OpenPGP key of the account's own always were.
- **Fixed: an administrator's support session could change the account's keys.** A support session acts as the
  person in the webmail so an administrator can help them, but it could store or remove the account's own S/MIME
  certificate and key and a correspondent's certificate, as 6.3.3 already allowed, and - new in this release -
  store or remove an OpenPGP key of the account's own or a correspondent's, mark a correspondent's key verified or
  accept its change, upload, remove or publish the key the Web Key Directory serves for the address, and switch
  zero access on or off or re-wrap its key: planting what the person's mail is then sealed to, or storing the next
  delivery to a zero-access mailbox in the clear for the administrator it exists to keep out. Every one of those
  writes is now refused to a support session with 403 and `code` `support_session`, as the S/MIME authority's
  enrolment already was; the session still reads them, without the wrapped keys. A correspondent's S/MIME
  certificate, and the published OpenPGP key and its switch (`PUT`/`DELETE /api/v1/me/openpgp`,
  `PUT /api/v1/me/openpgp/publish`), are now refused to an app password too, as a correspondent's OpenPGP key
  always was - a leaked app password could otherwise have published its own key for the address. A support
  session can no longer remove or rewrite the webmail's note that an administrator opened the mailbox
  (`support_last`) or the consent (`support_allowed`) in `PUT /api/v1/me/preferences`, and an app password can
  no longer give that consent ([Roles.md](hmailserver/docs/Roles.md), [OpenPGP.md](hmailserver/docs/OpenPGP.md),
  [ZeroAccess.md](hmailserver/docs/ZeroAccess.md)).
- **Fixed: a person could collect any Microsoft 365 mailbox with the server's own token.** On a server set up to
  collect Microsoft 365 mail with its own OAuth2 app token (`OutboundOAuth2*` or `OutboundOAuth2FixedToken`), an
  external account for a host on `FetchOAuth2Hosts` logs in with that token as whatever user name it gives. The new
  Other accounts page (`/api/v1/me/fetch-accounts`) let any person add one for `outlook.office365.com` naming
  another person's address, or rename the user on one an administrator had set up, and so copy - and with days to
  keep 0, delete - any mailbox the app registration reaches; a key restricted to named domains could do the same,
  and so could a domain administrator over COM, which had been open since 6.2.23 Alpha 1. Only a server
  administrator now adds or changes such an account. The fetchers also sent a bearer token without TLS when the account's security was none: they now
  send one over TLS only ([Migration.md](hmailserver/docs/Migration.md)).
- **Fixed: a relay that refused the OAuth2 login put the server's token in the bounce.** When the relay answered
  `AUTH XOAUTH2` with a refusal - Exchange Online's `535 5.7.3` for a mailbox with SMTP AUTH off, say - the bounce
  to the message's sender quoted the line this server had sent, and that line is the token in base64: the
  server's own, which sends as every mailbox the app registration was granted and, through `FetchOAuth2Hosts`,
  collects them. The SMTP log kept the line too, and the POP3 collector's log its own. The bounce now reads
  `hMailServer sent: AUTH XOAUTH2 <Token removed>` and the logs `AUTH XOAUTH2 ***`, as they have always left
  out a password; open since 6.2.22-pre1.
- **Fixed: an external account's user name or password could add commands to the collection.** Both are sent in
  command lines - POP3 `USER` and `PASS`, IMAP `LOGIN` - and a CR or LF in either ended the command and sent what
  followed as another, to the host and port the account names; the new Other accounts page let any person set
  them. A user name or password with a CR, LF or NUL is now refused by the REST routes and by `FetchAccount.Save`
  over COM, and the fetchers do not send one saved before.
- **Fixed: a person's other account could reach inside the network after it was saved.** Its host was judged
  public once, when it was saved, by the system's resolver; the server later resolved it again with its own DNS
  and connected to whatever came back, on any port from 1 to 65535. A name that answered a public address at
  the save and 127.0.0.1 or 10.x afterwards (DNS rebinding), or one the server's DNS answered differently (split
  horizon), made the server connect inside its own network every fifteen minutes and send a user name and
  password there. Schema 6104 marks an account a person makes on the Other accounts page, or moves to another
  server or port there, as `reader_created`; the fetcher connects such an account only to the public addresses
  the name resolves to at that moment (`HM5909` when there are none), and a person names port 110, 995, 143 or
  993 - both unless `reader_fetch_accounts_private_addresses` is on. Accounts from before 6104, and an
  administrator's, are not marked and behave as they did ([Migration.md](hmailserver/docs/Migration.md)).
- **Fixed: a host added to `FetchOAuth2Hosts` later gave the server's token to accounts others had named.**
  The save refuses anybody without the server's authority a fetch account for a host on the list, but the
  fetchers judged only the host at collection time, so an account a person, a key restricted to named domains
  or a domain administrator had set up for a host the administrator put on the list afterwards logged in with
  the server's own OAuth2 token as whatever user name it gave. Schema 6104 records whose word the server and
  the user name are (`hm_fetchaccounts.faorigin`): an account such a caller makes, or whose server, port or
  user name it changes, is `server_token_withheld`, and the fetchers log it in with its own user name and
  password instead. Accounts from before 6104 are not marked; a domain administrator's saved before this
  release cannot be told apart from a server administrator's
  ([Migration.md](hmailserver/docs/Migration.md)).
- **Fixed: a Web Push endpoint could point the server inside its own network.** With Web Push on (off by
  default), a person could subscribe an https endpoint at any address - a host on the private network, a cloud's
  metadata address - or plain http to any port of the server's own loopback, and the test push answered why the
  connection failed word for word, which mapped the network behind the server one request at a time. A push now
  goes to a public address only; plain http to the loopback is taken only while `WebPushLoopbackHttpForTests`
  (`loopback_http_for_tests` under `/api/v1/settings/push`), a switch for the regression suite that is off by
  default, is on, and then only from a request made on the machine itself that no proxy forwarded - a reverse
  proxy on the same machine that adds no forwarding header makes every person a caller from the machine; a
  failure is shown to the person as "could not be reached" and its reason written to the application log; and a
  push waits ten seconds at most, a test push five.
- **Fixed: one person could stall the webmail and the Deck for everybody.** The REST listener has four workers,
  and a request that waits on another server - a brand logo not yet known, a one-click unsubscribe, an OpenPGP key
  lookup, a test push - held one for up to its deadline, so one account keeping four of them in flight against a
  host that never answers, a made-up subdomain for each logo, left nothing to answer anybody else. An account now
  has one such request in flight at a time and the server two in all; the rest are answered `429` at once. A
  logo lookup or a key directory that took three seconds or more, in the DNS or at the host and whatever it came
  to, is held against its whole organizational domain by the Public Suffix List for an hour, so the next made-up
  subdomain is answered at once without asking it ([BrandLogos.md](hmailserver/docs/BrandLogos.md)).
- **A new installation is secure before anyone configures it.** A database this installation created - never an
  upgraded or attached one - is given a self-signed certificate for the machine (RSA 2048, 825 days), STARTTLS
  offered on 25, 587, 110 and 143, and TLS required for authentication on the Internet IP range, so that a password
  from another machine no longer crosses the network in the clear; My computer is left as it was. The Windows
  installer does it after the database step; on Linux `--create-database` marks the database and the unit's first
  start within a day does it, and the AppImage and the container run `--apply-new-install-defaults` by hand. The
  setup checklist's new Passwords only over TLS item judges every server, new or upgraded
  ([SecureDefaults.md](hmailserver/docs/SecureDefaults.md)).
- **A dedicated low-privilege service account on Windows.** A new installation runs the service as the virtual
  account `NT SERVICE\hMailServer` instead of LocalSystem; an upgrade keeps the account it has, and the installer's
  page, or `/serviceaccount=virtual|localsystem|keep`, offers the other. `hMailServer.exe /GrantServiceAccess`
  grants the account the program folder's Data, Logs, Temp and Database folders and folders an administrator
  approved, and nothing the configuration merely names. `hMailServer.INI` is read-only to the service, which works
  on with the database's values. What only LocalSystem could do is refused with a sentence: the live update's apply
  (an administrator runs the verified installer), the auto-ban's firewall rules and `Utilities.MakeDependent`.
  `ServiceAccountName` and `ServiceAccountPassword` are retired: `/Register` takes the account from
  `--service-account` alone ([ServiceAccount.md](hmailserver/docs/ServiceAccount.md)).
- **What the first security scan found, fixed**: the 7-Zip and the restore above; the in-process testers' 379 bare
  `throw;` statements, which ended the service when a check failed, now fail the test suite; MIME boundaries drawn
  from a per-thread generator seeded from the operating system instead of `rand()` reseeded from the clock; the
  vendored zlib 1.3.2; and VEX statements for the advisories that do not reach the server.
- **An evidence pack for a buyer's security questionnaire**: where mail and personal data are kept, what leaves the
  server, ISO/IEC 27001:2022 Annex A and SOC 2 mapped to what the product provides, and the supply chain, each fact
  with the file it can be checked in and each gap named ([EvidencePack.md](hmailserver/docs/EvidencePack.md)).
- **Fixed: SCRAM-SHA-256 signed in with a password every other path refused.** SMTP `AUTH`, IMAP `AUTHENTICATE`
  and POP3 `AUTH` with `SCRAM-SHA-256` (or `-PLUS`) accepted the account's own password for an account with a
  second factor enrolled - whose own password is otherwise refused in every mail client, app passwords being
  the only credential - and a password past `PasswordPolicyMaximumAgeDays`, and never asked the domain's or the
  account's country policy: SCRAM proves the password without reaching `PasswordValidator` or
  `AccountLogon::Logon`, where those rules are. So a phished password of an account with TOTP could submit mail
  as it and read its mailbox. The second-factor and expiry gaps were in 6.3.3 as well. Each is now a forced
  failure of the exchange, answered as a wrong password is; the country refusal is charged to nothing, as it is
  on the other paths. The `OnClientValidatePassword` script hook is still not consulted for SCRAM, which has no
  password to give it ([CountryBlocking.md](hmailserver/docs/CountryBlocking.md),
  [DevicesAndSessions.md](hmailserver/docs/DevicesAndSessions.md)).
- **Fixed: a sign-in under a domain alias escaped the domain's country policy.** The policy was looked up under
  the domain the client typed, and an alias domain has no policy of its own, so `bob@alias.example` was allowed
  from a refused country and then signed in as `bob@example.com`. The name is now resolved as the sign-in resolves
  it - domain aliases, then the default domain - before the policy is read
  ([CountryBlocking.md](hmailserver/docs/CountryBlocking.md)).
- **Fixed: a token, a certificate or a Kerberos ticket signed in past the country policy.** `XOAUTH2`,
  `OAUTHBEARER`, `EXTERNAL` and `GSSAPI` on SMTP, IMAP and POP3 never asked the domain's or the account's
  country policy, which the password paths and the REST API ask, so a stolen bearer token signed in from a
  refused country. They now ask it where they ask the compromise hold (`AccountLogon::RefusedByHold`), and the
  refusal is charged to nothing, as a password's is ([CountryBlocking.md](hmailserver/docs/CountryBlocking.md)).
- **Fixed: an SMTP session signed in without a password sent only one message.** At each `MAIL FROM` after the
  first the session's credentials are checked again, and the check validated the password the session kept -
  which only `PLAIN` and `LOGIN` keep. After `SCRAM-SHA-256`, `XOAUTH2`, `OAUTHBEARER`, `EXTERNAL` or `GSSAPI` the
  second message was refused with 550 "Login credentials no longer valid" and the session signed out. Such a
  session now has its account checked again instead: still active in an active domain, not a room, not held for
  review, not refused by its country policy, and for SCRAM not since given a second factor or an expired password.
- **Fixed: the IMAP master user opened a mailbox past the mailbox's country policy.** `AUTHENTICATE PLAIN` with an
  authorisation identity asked the country policy, and the compromise hold, of the master user only, so a master
  user in a domain with no policy signed in as a mailbox from a country the mailbox's domain refuses. The account
  signed into is now asked too, after the password, and the refusal is charged to nothing
  ([CountryBlocking.md](hmailserver/docs/CountryBlocking.md)).
- **Fixed: a sign-in under a domain alias was not the account to SMTP's own controls.** After signing in as
  `bob@alias.example`, the send-stop of the quota policy, the sending quota (counted under a second name), restricted
  delivery, the takeover check, message recall, the address book and the message-size limit looked the account up
  by the name as typed, with the default domain alone, and found nothing - so none of them held. They now use the
  account the sign-in resolved to.
- **Fixed: a domain's maximum message size was lifted by `MAIL FROM:<>`.** A signed-in account was held to its own
  size limit whatever `MAIL FROM` named, but not to its domain's, which was read from the envelope sender's domain;
  naming no sender or another domain's address took the server's instead. The signed-in account's whole answer -
  its own value, else its domain's - now holds as well ([SettingLevels.md](hmailserver/docs/SettingLevels.md)).
- **Fixed: the backup-MX recipient callout's ceiling was per recipient domain.** A policy for a pattern such as
  `*.partner.example` gave every subdomain a sender named ten verifications a minute of its own, all to one
  primary, each holding a thread of the SMTP service for up to the timeout, and every address asked about stayed
  in memory. The ceiling is now the policy's and the primary's, at most 8 verifications run at once (2 to one
  server), and the remembered answers are swept and capped at 10,000
  ([RemoteDomains.md](hmailserver/docs/RemoteDomains.md)).
- **Fixed: one connection could put any correspondent on the server's block list.** A message to a spam trap
  blocked its envelope sender server-wide for `SpamTrapBlockDays` (30), whatever that sender was, and the
  envelope is whatever the client types: anybody who knew a trap address - published to be found - could have a
  bank's or a supplier's real mail held or marked for every mailbox. The sender is now blocked only when its
  domain's SPF passes for the connecting address; the address is banned and the hit recorded as before
  ([SpamTraps.md](hmailserver/docs/SpamTraps.md)).
- **Fixed: the send API was not journalled.** A message an application sent through `POST /api/v1/send` reached
  neither of the two points where journal rules are evaluated, so a domain or account under a compliance rule
  had its application mail, and a journalled recipient's copy of it, missing from the archive. It is evaluated
  now, the key's sender counting as the sender when it is an account here. Journaling.md said mail that arrives
  was covered whichever way it arrived; it now says what is not covered in either direction: everything the
  server queues itself - the webmail, a send-later draft, an event script, a rule's forward, copy or reply, an
  automatic reply, a Sieve redirect, an account's forwarding, a delivery report
  ([Journaling.md](hmailserver/docs/Journaling.md)).
- **Fixed: a single sign-on ticket could be redeemed in another person's browser.** Found in the audit before
  this release, in the new OpenID Connect and SAML sign-in. The ticket a finished sign-in carried to the page in
  `/portal#sso=<ticket>` was redeemed by value alone, so anybody who could complete a sign-in of their own could
  send that link to someone else, and the server's own page would sign that person's browser into the sender's
  mailbox - login forgery, past both the `X-Requested-With` check and SAML's `hmsaml` cookie. The ticket is now
  bound to the browser that finished the sign-in by an `hmsso` cookie (HttpOnly, `SameSite=Strict`,
  `Path=/api/v1/session`) set beside it, and `POST /api/v1/session` refuses it without that cookie
  ([PasskeysAndSingleSignOn.md](hmailserver/docs/PasskeysAndSingleSignOn.md)).
- **Fixed: a one-time code asked for on top of a password could be guessed without limit.** Found in the audit
  before this release. Changing the password, making an app password or recovery codes, setting a recovery
  address and registering a passkey ask for the account's password and then its one-time code, and a wrong code
  there was neither counted nor logged, so somebody holding the password and a session (or, for the password
  change, an app password) could try codes at the request rate until one matched and leave with a credential
  that needs no second factor. The administrator's passkey registration had the same gap. A wrong code is now
  logged and charged like a wrong password, and after five in fifteen minutes every code for that account, or
  for the administrator, is refused with 429 for fifteen minutes
  ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Fixed: with a parent-domain relying party, a page on any sibling host could sign somebody in with their
  passkey.** Found in the audit before this release. A passkey's origin was compared with the request's own
  `Host` header, which the client writes, so with `passkey_relying_party` set to `example.com` as the document
  advised, a script on any host under `example.com` could collect a person's assertion and post it here naming
  that host, and be signed in as them with no password and no code. A new setting, `passkey_allowed_origins`,
  names the origins this server's pages are served at; once a relying party is set it is required, and the
  origin of every registration and sign-in must be one it names. A credential made in a frame of another site's
  page is refused too. With neither setting, the default, the check is unchanged
  ([PasskeysAndSingleSignOn.md](hmailserver/docs/PasskeysAndSingleSignOn.md)).
- **Fixed: a switched-off domain still let its people in with a passkey or single sign-on.** Found in the audit
  before this release. A password is refused for an account whose domain is not active, but a passkey, an OpenID
  Connect or SAML sign-in to an existing mailbox, and every browser session already open went on working. All
  three now check the domain, and a session ends at its next request once its domain is switched off
  ([PasskeysAndSingleSignOn.md](hmailserver/docs/PasskeysAndSingleSignOn.md)).
- **Fixed: the forgotten-password route's timing said which mailboxes can be reset.** Found in the audit before
  this release. `POST /api/v1/password-reset` gave every address the same answer, but gave it only after making
  the link and queueing the message when the mailbox had a confirmed recovery address, so the answer came
  measurably later for exactly those mailboxes; and a reverse proxy on the same machine had no per-client limit
  at all. The answer now comes before the address is looked up, the link is made on the async work queue, and
  requests from loopback share a ceiling of thirty in any minute
  ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Fixed: a one-time code could be used again for as long as it was in its window.** Found in the audit before
  this release. No place that checks a TOTP code - the COM sign-in (`AuthenticateWithCode`) for an account and for
  the administrator, the administrator's Basic sign-in on the REST API, and the REST routes that ask for a code on
  top of a password - remembered the codes it had accepted, so a code somebody read over a shoulder, out of a
  proxy's log or off a phishing page, with the password beside it, was a whole credential for up to a minute and a
  half after its owner had used it. RFC 6238 says a verifier must not take a code twice. Each owner's last step
  taken is now remembered, in one record every one of those places shares: the same code, or one from an earlier
  step, is refused wherever it comes next. It is not counted as a wrong code - it is no guess, and the one who
  sends it again is most likely its owner - and the wrong codes that are counted are judged and counted under one
  lock, so that requests racing each other cannot all be judged before the fifth is counted
  ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Fixed: a wrong one-time code at the COM sign-in was not counted.** Found in the audit before this release.
  `AuthenticateWithCode` checked an account's code before its password and counted neither, and the
  administrator's wrong code was logged and nothing more, so somebody with the password could guess codes over
  COM at the speed of the calls. The password is now checked first, and a wrong code after a right password
  counts against the same budget the REST API keeps for that owner - five in fifteen minutes, then no code taken
  for fifteen, over COM and REST alike. COM stays outside the per-name lockout and the auto-ban, as its passwords
  are ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Fixed: behind a reverse proxy on the same machine, the administrator's one-time code could be guessed at the
  request rate.** Found in the audit before this release; present since the administrator's second factor came
  to the REST API. A wrong code in `X-hMailServer-OTP` on the administrator's Basic sign-in was charged only to
  the auto-ban, which leaves 127.0.0.1 and `::1` alone - and behind a proxy on this machine every request comes
  from there. It now counts against the administrator's budget, and once that is spent the answer is 429 with
  `code` `second_factor_locked` and `Retry-After`; the auto-ban is still charged once for each wrong code
  ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Fixed: behind a reverse proxy that terminates HTTPS, the session cookie was not marked `Secure`.** Found in
  the audit before this release. `hmailsession`, and the sign-in cookies `hmoidc`, `hmsso` (OpenID Connect) and
  `hmpasskey`, were marked `Secure` only when the REST listener itself spoke TLS, so behind the usual proxy in
  front of the plain loopback listener the session cookie of an HTTPS site was one a browser would also send over
  plain HTTP. They now follow the rule SAML's cookies already did: `Secure` over TLS, or when the proxy on this
  machine says `X-Forwarded-Proto: https` ([DevicesAndSessions.md](hmailserver/docs/DevicesAndSessions.md)).
- **Fixed: registering an administrator passkey skipped the one-time code when the administrator's second factor
  could not be opened.** Found in the audit before this release. With `AdministratorTotpSecret` enrolled but its
  secret unopenable on this machine - the key it was protected with lost - every administrator sign-in refuses,
  but `POST /api/v1/passkeys` for the administrator took the password alone from a session already open. It now
  refuses with 409, as the sign-ins do ([PasskeysAndSingleSignOn.md](hmailserver/docs/PasskeysAndSingleSignOn.md)).
- **Fixed: a domain's spam traps were not confined to the domain over REST.** A domain administrator, a role or an
  API key confined to one domain could declare a spam trap in any other domain on the server with `POST
  /api/v1/domains/{domain}/spam-traps` - and a hit on it bans the connecting address and blocks the envelope sender
  for every domain - and read another domain's traps and the senders and addresses that hit them with `GET` on the
  same path and on `.../spam-traps/hits`. The three routes are now scoped to the domain they name, as a trap's
  removal already was, and answer `403` for any other.
- **Fixed: a domain administrator could become the server administrator through an app password.** Where the server
  administrator's own mailbox sits in a domain somebody else administers, that domain's administrator could issue
  an app password on it (`POST /api/v1/accounts/{address}/app-passwords`, or COM's `AppPasswords.Add`), was handed
  the secret, and could then log on to COM with it - `Application.Authenticate` accepted an account's app password
  and gave the logon the account's whole role, past its second factor. COM no longer accepts an app password as a
  logon at all (it still opens the mailbox over IMAP, POP3 and SMTP), and a credential may no longer issue or take
  back an app password, share a folder, or write, flag, copy or delete mail, or revoke a device, of an account
  whose authority it does not take in, over REST or COM ([Roles.md](hmailserver/docs/Roles.md)).
- **Fixed: a domain administrator could delete or rename the server administrator's mailbox in its domain.** `DELETE
  /api/v1/accounts/{address}` and `POST /api/v1/accounts/{address}/move` asked only whether the account's domain
  was the credential's, so the server administrator's own mailbox could be deleted with its mail, or renamed so
  that its logon name no longer existed. Both now refuse, with `403`, an account whose authority the credential
  does not take in, as COM's `Account.MoveTo` always did; COM's `Account.Delete`, `Accounts.Delete` and
  `Accounts.DeleteByDBID` refuse it too ([Roles.md](hmailserver/docs/Roles.md),
  [MailboxMove.md](hmailserver/docs/MailboxMove.md)).
- **Fixed: the REST listener read request headers out of the body.** A header the request's head did not carry was
  looked for in the body as well, so a cross-site form posting `text/plain` to `/api/v1/session` could carry an
  `Authorization` line of an account the attacker held and sign the victim's browser in to it (login CSRF), and an
  `X-Requested-With` line in a body satisfied the guards that ask for that header. Every header is now read from
  the head alone.
- **Fixed: a credential confined to one domain could set the webmail's branding for every domain.** `PUT
  /api/v1/portal/branding` and `PUT /api/v1/portal/files` name their domain in the body, where the authorisation
  did not look, so an API key or a role holding `manage_settings` and confined to one domain could rewrite the
  server's sign-in announcement and logo, or another domain's, and the file-link policy of either. A confined
  credential now sets only its own domains' and is refused the server's and any other's with `403`, and a pass
  over the scheduled sends (`POST /api/v1/scheduled/run`) is refused to it as server-wide
  ([Roles.md](hmailserver/docs/Roles.md)).
- **Fixed: a domain administrator could repoint the server administrator's fetch account.** The routes under
  `/api/v1/accounts/{address}/` for an account's rules, fetch accounts, OpenPGP keys, directory entry, booking
  policy, address-book shares, push devices, recovery address and codes and review state asked only whether the
  account's domain was the credential's. Where the server administrator's own mailbox sits in a domain somebody
  else administers, that domain's administrator could point its fetch account at a host of its own - the server
  then signed in there with the kept password - set rules on the mailbox, publish a key under its address, clear
  its recovery or exempt it from the compromised-account detection; a support session into the mailbox and a
  migration job copying mail into it asked the same and no more. Every route that changes an account named in its
  path now asks one rule, in one place, and refuses with `403` an account whose authority the credential does not
  take in; `build/check-role-choke-point.py` fails a new such route that is not in it. A migration, or a job of
  it, is started, paused, resumed, retried, cancelled or deleted only by a credential that takes in every account
  its jobs copy into ([Roles.md](hmailserver/docs/Roles.md), [Migration.md](hmailserver/docs/Migration.md)).
- **Fixed: over COM, a domain administrator could change what is in the server administrator's mailbox in its
  domain, and point a rule or a fetch account at another domain's account.** COM's `FetchAccount`, `Rule` (with its
  actions and criteria), `IMAPFolder`, `IMAPFolderPermission` and `Message` writes, and `Account.DeleteMessages`,
  `RevokeDevice`, `ClearRecoveryAddress`, `ClearRecoveryCodes` and `RemovePushSubscriptions`, asked nothing of the
  account they were in - only how the object had been reached - so a domain administrator could add a fetch account
  or a rule to the server administrator's mailbox, share or delete its folders and mail, or clear its recovery; and
  a rule, a rule's action or a fetch account reached in its own domain could be given another account's id, in any
  domain, and saved there. Each now asks where it writes whether the credential reaches the account (the server
  administrator, an administrator of its domain, or the account itself) and takes in its authority, and refuses
  otherwise; an event script's objects are left as they were ([Roles.md](hmailserver/docs/Roles.md)).
- **Fixed: a key or a role confined to a domain could route the server administrator's mail in that domain to a URL
  of its own.** An inbound webhook route was judged by its domain alone, and a route takes an address's mail instead
  of its mailbox, so a credential holding `manage_inbound_webhooks` and confined to a domain could route the server
  administrator's address there - or the whole domain - and read its mail, password resets included. A route is now
  made, changed or removed only by a credential that takes in the account at its address (or the one its plus tag
  reaches), and a domain's route only by one that takes in every account of the domain with an admin level or a
  role; the refusal is `403` ([InboundWebhooks.md](hmailserver/docs/InboundWebhooks.md)).
- **Fixed: a domain administrator could not clear its own users' recovery address, recovery codes or push devices
  over REST.** `DELETE /api/v1/accounts/{address}/recovery`, `.../recovery/codes` and `.../push` (and `GET .../push`)
  read the whole address as a domain name, which no credential confined to a domain lists, so they answered `403`
  to every domain administrator and every key issued for named domains. They are now scoped to the address's
  domain, as the other account routes are.
- **Fixed: a large download over the REST API or the web services was cut off, and held whole in memory while it
  lasted.** The deadline that covers reading a request covered writing its response too, so a file link, an export
  or a message source the reader could not take in within the request deadline (30 seconds on the REST listener,
  15 on the web services) ended part way, every time - a 100 MB file link below about 27 Mbit/s - and the
  connection's five-minute limit cut off whatever outlived it. Meanwhile each download held the whole body three
  or four times over, so 64 slow readers of a 500 MB file link, which needs no sign-in, could hold some 32 GB of the
  mail server's memory. A response is now written in pieces and the deadline is progress: a reader that stops
  reading is closed after the request deadline. A file link, the export archive (`/export.zip`) and an eDiscovery
  export are sent from their file on disk 64 KB at a time, never read into memory whole. A large download - a
  file, or a body of 4 MB or more - may outlive the connection's limit for as long as its size takes at 128 KB a
  second, and at most sixteen are written at once; one more is answered `503` with `Retry-After: 30`
  ([PersonalData.md](hmailserver/docs/PersonalData.md), [Discovery.md](hmailserver/docs/Discovery.md)).
- **Fixed: a request head that never ended could hold 16 MB per connection.** The listener read a head into the
  buffer sized for the largest body a route takes and applied the 64 KB cap on a head only once its blank line
  arrived, so each connection could be made to hold 16 MB until the request deadline closed it. A head is now
  refused with `413` as soon as it passes the cap.
- Fixed: BATV tags, SRS addresses, the fabricated SCRAM salt of an unknown account and the Web Push endpoint's
  hash were made over a conversion that, on Windows, gave zero bytes for everything after the first character
  above U+00FF and, on Linux, `?` for each such character - the conversion the audit trail's chain was fixed of
  (under Administration). So two internationalised mailboxes of one length shared a BATV tag and an SRS hash, and
  a bounce to one was taken, or reversed, as a bounce to the other; a database password of Cyrillic letters added
  only its length to the SCRAM key; and two push endpoints of one length shared the hash the unique index is on.
  Each is now made over UTF-8. A String that holds UTF-8 widened one byte to a character - what the SMTP and IMAP
  command lines give, and so what BATV and SRS are handed - is hashed as those bytes, which the old conversion
  hashed too; so for such a mailbox, as for ASCII, a tag or an SRS address handed out before the upgrade still
  verifies, and the salt an unknown account is given does not change. `ClassTester` holds each to the old
  conversion over ASCII and over widened UTF-8, and to a difference over Cyrillic, and `Batv.cs` accepts a tag
  computed the old way. A push endpoint outside printable ASCII - which no browser gives - is refused.
- Fixed: a password of Cyrillic, Greek or CJK letters could be opened by any other password of its length. Every
  password hash (MD5, salted SHA-256, PBKDF2, Argon2id, scrypt, and the pepper) was made over the same
  conversion - NUL bytes on Windows from the first character above U+00FF on, `?` for each on Linux - so SASL
  PLAIN, whose fields are UTF-8, took `ключик-вход` for `пароль-тест`, and on Windows a REST API Basic credential
  of NUL bytes took either; the administrator password was hashed the same way. Hashes are now made over UTF-8,
  which leaves every ASCII password's hash as it was and lets such a password serve SCRAM-SHA-256. A password that
  arrives as its UTF-8 widened one byte to a character (IMAP `LOGIN`, REST Basic) is hashed as those bytes, as it
  was, so it and the same password decoded are one; a sign-in also tries a Latin-1 password's characters as bytes
  (for a client that sends UTF-8, those of the text it decodes to), which opens a hash made of them before, and
  that sign-in makes the hash again over UTF-8 (as it does one made under a pepper outside ASCII, under
  the pepper's UTF-8), after which it serves SCRAM-SHA-256 too. A password holding U+0000 is refused. COM's
  `Utilities.MD5` and `Utilities.SHA256` are the same code, so for text outside ASCII they now give the digest of
  its UTF-8. A hash the old conversion made of a password above U+00FF is refused and logged (upgrade item 24);
  on Linux such a hash still opens to the password with `?` for those characters until it is set again, and on
  both platforms SCRAM-SHA-256 still opens a PBKDF2 one to the bytes it was made over
  ([SecretsAtRest.md](hmailserver/docs/SecretsAtRest.md), *A password outside Latin-1*).
- Fixed: the crash-dump helper, `hMailServer.Minidump.exe`, loaded `dbghelp.dll` by name, so a copy put in `Bin`
  would have run under the service's account at the next crash. It loads System32's, and the service starts
  the helper by its full path.
- Fixed: the Control Panel restarted the service after a settings change by starting `cmd.exe` elevated by name,
  and Windows looks for a bare name in the working directory first - wherever a file dialog last left it - so
  a `cmd.exe` put there would have run behind the elevation prompt. It runs System32's `cmd.exe` and
  `net.exe` by their full paths, and `DBUpdater.exe`, run as an administrator, opens an upgrade script in
  System32's Notepad by its full path, where it named `notepad.exe` alone.
- Fixed: the MySQL client's own DLLs were looked for in the current directory and along `PATH` as well; they
  are looked for beside `libmysql.dll` and in System32 only. The MariaDB client the x64 installer ships needs
  nothing else; a client copied into `Bin` by hand needs its DLLs beside it.

### Encryption

- **The server publishes its users' OpenPGP keys** (schema 6050) through Web Key Directory; publishing is off
  by default, and a secret key is refused at every entry point.
- **OpenPGP in the webmail** (schema 6080): end-to-end encryption in the browser, with the keys under a
  passphrase this server never sees; a correspondent's key remembered at first sight and a changed key shown
  as a warning (schema 6081); and a password-protected message for a correspondent with no key (schema 6082)
  ([OpenPGP.md](hmailserver/docs/OpenPGP.md)).
- **Zero-access mailboxes** (schema 6083): opt-in per account, delivered mail is encrypted to the account's own
  key once the filters have run, and the administrator cannot read, search or recover it
  ([ZeroAccess.md](hmailserver/docs/ZeroAccess.md)). A message that arrives already encrypted is stored as
  it arrived; whether it is, is read from its body and not its `Content-Type` - fixed before release, when a
  `multipart/encrypted` label over plain text, or an S/MIME v2 opaque-signed message with no `smime-type`,
  was stored in the clear; and the whole message has to be under the key, not merely hold some ciphertext -
  fixed before release, when real PGP/MIME ciphertext with a readable part beside it was stored as it arrived,
  that part in the clear. Gateway encryption to partner domains makes the same decision the same way, and
  encrypts such a message rather than sending the readable part as it was.
- **Gateway encryption to partner domains** (schema 6094): a remote-domain policy encrypts mail to a partner's
  OpenPGP key or S/MIME certificate at this server, and says whether to refuse or send unencrypted when it
  cannot.
- **An organisation CA for S/MIME** (schema 6095): a domain's own authority certifies keys its people generate
  in the browser. It never makes a person's key, and the private key stays wrapped where this server cannot
  open it, so nothing here can recover mail encrypted to a lost key
  ([SmimeAuthority.md](hmailserver/docs/SmimeAuthority.md)).
- **The webmail's S/MIME made honest**: signing out takes the private keys with it, an administrator's password
  change re-wraps them, and a message that was to be sealed is refused as a draft rather than stored in the
  clear.
- **Mailbox encryption at rest** (schema 6099): switched on per account, after the domain's recovery key has been
  made, every message stored in the account's folders from then on is encrypted to an X25519 key of the account's
  own, with AES-256-GCM, as the message store's last stage. Mail that arrives while nobody is signed in is
  encrypted all the same; reading it needs the private half, which the account's own password opens at sign-in and
  which closes `MailboxKeyIdleMinutes` (15) after the last use. App passwords are refused for such a mailbox, an
  administrator's password reset is refused while the key is closed and goes through a recovery with the domain's
  passphrase, and it cannot be combined with zero access or a TOTP second factor. It is on REST and the Control
  Deck, with no webmail card and no Control Panel page, and `--encrypt-store` and `--decrypt-store` rewrite what
  was stored before. Off on every account ([EncryptionAtRest.md](hmailserver/docs/EncryptionAtRest.md)).
- **Secrets at rest** (schema 6103): an account's TOTP second factor and the `[Settings]` secrets (`SRSSecret`,
  `BATVSecret`, `OAuth2HmacSecret` and the rest) are sealed with AES-256-GCM under a stored-secrets key the
  installation's servers share, kept in each server's `hMailServer.ini` under DPAPI or the Linux key file, never in
  the database or a backup; the database records which key it is. The Windows installer makes the key on a new
  installation, and the container's `--create-database` on Linux; after an upgrade nothing is sealed until an
  administrator runs `--create-stored-secrets-key`, which the setup checklist offers. `--export-stored-secrets-key`
  and `--import-stored-secrets-key` move it to another server, the checklist stays open until it has been exported,
  and `--forget-sealed-secrets` with a lost key's id clears only what that key sealed.
  `ProtectStoredSecretsWithDPAPI=0` keeps everything as written. The step widens `accounttotpsecret` to 1024
  characters. The installer now writes the administrator password as PBKDF2 itself
  ([SecretsAtRest.md](hmailserver/docs/SecretsAtRest.md)).
- **A certificate for an IP address is accepted on Linux**: an HTTPS endpoint given by its address - an object
  store, a webhook, an identity provider, the update feed - was refused when its certificate named the address as
  an iPAddress entry, which is where RFC 5280 puts it; and no server name is sent for an address.

### Administration

- **One value, three levels** (schema 6088): 27 settings may be set for the server, a domain or an account,
  with every interface showing what is in effect and where it came from
  ([SettingLevels.md](hmailserver/docs/SettingLevels.md)).
- **Profiles**: office, gateway, relay and sender, each a documented set of settings applied with a diff first
  and a way back ([Profiles.md](hmailserver/docs/Profiles.md)); **what this server has changed from stock**
  ([ChangedFromStock.md](hmailserver/docs/ChangedFromStock.md)).
- **Two command-line clients over REST**, `hmctl` (installed by the Linux packages) and the `HmailServer`
  PowerShell module, with the whole configuration as one document to export, diff and apply, and accounts
  imported from and exported to a spreadsheet ([CommandLine.md](hmailserver/docs/CommandLine.md)).
- **The audit trail** (schema 6043) as a hash chain that can be verified, a setting's change put back from its
  row, and a Control Panel page; its first gaps - misspelt table names, the settings store, the access lists -
  closed. **Alerts** now cover eleven conditions, by mail or signed webhook.
- Fixed: the audit trail's chain did not cover a character above U+00FF. The fields were hashed through a
  conversion that, on Windows, hashed everything after the first such character as zero bytes and, on Linux,
  each such character as `?`, so a row naming a Cyrillic account or an internationalised address could be
  edited after that character (`deleted` to `created`, say) and `verify` still answered intact. The fields
  are now hashed as UTF-8 and the hash written in upper case; rows written before, in lower case, are checked
  the old way ([AlertsAndAuditTrail.md](hmailserver/docs/AlertsAndAuditTrail.md), *Verifying the chain*).
  `GET /api/v1/audit` listed such a row through the same conversion, so its actor, object and detail came back
  as zero bytes or question marks, and its `actor`, `object_type` and `action` filters matched nothing outside
  ASCII; the listing is now UTF-8 and the filters are read as UTF-8.
- Fixed: the alerts carried text outside ASCII as something else. The REST routes widened the JSON body's UTF-8
  one byte at a time, so a rule's condition, address, webhook and secret, and a test alert's condition and
  summary, were stored as other text than was sent, and a condition was counted in bytes against its limit of 64
  characters; and
  `GET /api/v1/alerts/rules` and `/events`, the webhook's body and the mail wrote text through the conversion
  above, so an alert the server raised about a name outside Latin-1 arrived with zero bytes or question marks.
  Every field is now read and written as UTF-8; the mail's Subject is RFC 2047 encoded words when it is not
  ASCII; the condition in `X-hMailServer-Event` is percent-encoded outside printable ASCII, so no byte of it can
  end the header; and the webhook is signed with the secret's UTF-8, which for an ASCII secret and body is the
  signature it always was (`ClassTester` holds it to a value computed apart); a secret stored before this fix,
  as the bytes it was sent as, signs with those bytes, so no receiver's check changes. Text that arrived that
  way - an address from the SMTP command line, a rule written before - is written as the UTF-8 it stands for.
- **A setup checklist the server judges**, a sample domain that can be thrown away, **help beside every page**
  of the Control Panel, the Deck and the webmail from the documents built into the binary, and first-run tours
  ([SetupChecklist.md](hmailserver/docs/SetupChecklist.md), [SampleDomain.md](hmailserver/docs/SampleDomain.md),
  [Help.md](hmailserver/docs/Help.md)).
- **The Control Panel on one design system**: a command bar, a sidebar that collapses to a rail and dialogs
  sized to their content ([ControlPanelDesign.md](hmailserver/docs/ControlPanelDesign.md)); 65 pages, ten more
  than at 6.3.3.
- **The Control Deck** in twenty languages besides English, redrawn on the webmail's design and usable without a pointer, with
  one connection's log followed live ([FollowConnection.md](hmailserver/docs/FollowConnection.md)). The census in
  [DeckParity.md](hmailserver/docs/DeckParity.md) counts 458 of the 466 properties the desktop Control Panel
  writes as writable over REST and reachable from a Deck view; the eight it lists as missing are an account's
  directory fields.
- **A person's data as one archive**, and an erasure that reaches the rest of it
  ([PersonalData.md](hmailserver/docs/PersonalData.md)); **mailbox rename and move** as one operation (schema 6056)
  ([MailboxMove.md](hmailserver/docs/MailboxMove.md)).
- Fixed, in the person's export new in this release: an app password could download the whole account at
  `GET /api/v1/me/export.zip`, and the archive's `account.xml` carried the password's hash, the authenticator's
  seed in the clear, the app passwords' hashes and the fetch accounts' passwords and OAuth tokens, sealed only
  with a key built into every copy of the server. The download is now refused (403) to an app password and to a
  webmail session started with one, and `account.xml` - in every form of the export - leaves out the password,
  the seed, the hashes of the app passwords, recovery codes, password history and file passwords, the fetch
  accounts' secrets, a browser's push secret and the wrapped mailbox key of an account encrypted at rest
  ([PersonalData.md](hmailserver/docs/PersonalData.md)).
- Fixed, in the same export: its `logs.txt` took every log line in which the address occurred inside another,
  so that `ann@example.com`'s own download carried the sessions, peers and recipients of `joann@example.com` and
  `ann@example.com.au`. The address is now matched whole.
- Fixed, in the same export: each folder was built as one mbox in memory, deflated beside a copy of itself, and
  the 500 MB download ceiling was checked only once the whole archive was written, with nothing limiting how
  many were built at once - so any account, four requests at a time, could hold every HTTP worker (the webmail's
  and the whole API's) for minutes and push the service toward running out of memory. A folder past 256 MB is
  now written in parts, a mailbox whose mail alone is past 2 GB is refused (413) before anything is built, and a
  download is refused while an archive of the same account is being built (409) or two are on the server (429).
- Fixed, in the erasure of an address from the logs new in this release: every occurrence of the address inside
  another was rewritten too, so that erasing `ann@example.com` turned `joann@example.com` into `jo[erased]` and
  counted it as removed, and each log was truncated and written in place, so that a crash or a full disk part way
  lost the rest of it. The address is matched whole - still where a line quotes it (`'ann@example.com'`) or
  writes it after `=` or `/`, though not as the last part of an SRS address, which is another person's - and each
  log is written beside itself and renamed over it.
- Fixed: on PostgreSQL an erasure compared the message trace, the greylisting triplets, list memberships and
  aliases with the address exactly as given, and PostgreSQL compares text by its bytes, so a row written in another
  case (`Ann@Example.com`) survived an erasure of `ann@example.com` that reported success. Every store the
  erasure deletes rows from now compares both sides lower-cased.
- **eDiscovery** (schema 6097): a compliance officer opens a case, searches every mailbox, the deleted items and
  the archive by person, date, words and attachment, puts the mailboxes, domains or archived copies it is about on
  a legal hold, and exports what was found as EML and mbox with a manifest whose SHA-256s prove what was handed
  over. A hold stops the account's and its domain's deletion, a mailbox move to another domain, a purge of its
  deleted items, the retention sweep and a full restore, and keeps what its owner deletes among the deleted items
  until it is released. Every search, hold, export and download is on the audit trail. Two permissions,
  `view_discovery` (which the auditor role holds) and `manage_discovery`; the routes are `/api/v1/discovery/cases`,
  with a view on the Control Deck and a read-only page in the Control Panel
  ([Discovery.md](hmailserver/docs/Discovery.md)).
- Fixed, in the eDiscovery export new in this release: the .zip counted its entries in the 16 bits of the
  classic end record alone, so an export of more than 65,535 entries - a search may keep 100,000 hits, each
  written as an .eml - ended with a count modulo 65,536, and a reader that trusts it, .NET's `ZipArchive` among
  them, refused the evidence package or listed part of it. From 65,535 entries on the archive ends with the ZIP64
  end record and its locator, which count them in 64 bits; the person's export is written the same way
  ([Discovery.md](hmailserver/docs/Discovery.md)).
- **An erasure reaches the stores beside the mailbox**: messages held for review, messages that expire,
  password-protected messages, the recipients an account remembers writing to, the out-of-office memory and the
  correspondents' S/MIME certificates and OpenPGP keys. Every comma-joined list is matched whole, so that erasing
  `ann@` never touches `joann@`, and the archive index's own match is fixed the same way. A legal hold outranks the
  erasure, and `POST /api/v1/addresses/erase` answers `held` beside `removed`
  ([PersonalData.md](hmailserver/docs/PersonalData.md)).
- Fixed: a domain could be given the single-label name of a directory the server keeps its own files in under
  the data directory - `protected`, `confidential`, `recoverable`, `discovery` and the rest - over COM and REST
  (the Control Panel asked for a dot), and then shared that directory with the store. A backup left such a
  domain's mail out and a restore did not put it back; on Windows the recoverable, confidential and protected
  stores' clean-up could remove a mailbox directory named by a number; and renaming the domain carried the whole
  directory, the store's files with it, into the new name and deleted it where the store looks. Such a name is
  now refused for a new domain and for a rename; an existing one is reported at every start (`HM7597`) and on the
  checklist, and renaming it moves only its accounts' mailboxes
  ([SetupChecklist.md](hmailserver/docs/SetupChecklist.md)). The operator's `GET /api/v1/accounts/{address}/export.zip`
  in the OpenAPI document now says, as the download does, that the credentials are left out of `account.xml`, and
  lists its 403, 409 and 429 answers.
- Fixed: renaming a domain failed half-way when any of its accounts had a Sieve script, a vacation reply's memory
  or a duplicate check on disk. The domain's Sieve tree was moved first, and each account's rename then refused
  to move its own Sieve directory onto the one that move had just made - after the mailboxes had moved and before
  the row took the new name, so the domain's mail was where its name did not look, and a second rename was refused
  because the new directory existed. An account's rename in the cascade now moves nothing that is not left under
  the old name.
- Fixed: 72 COM classes answered a refusal as a bare HRESULT; each now says why. The message trace's search
  answered nothing on SQL Server Compact (one parameter written twice and bound once).

### Migration and import

- **A migration that survives being interrupted** (schema 6090): mailboxes copied from another IMAP server,
  Google Workspace or Microsoft 365 as jobs, one per mailbox, throttled, resumed without copying anything twice,
  with contacts and calendars beside the mail ([Migration.md](hmailserver/docs/Migration.md)).
- **Other accounts in the webmail** (schema 6065): POP3 and IMAP accounts fetched from the page, and an import
  from Gmail or Outlook.com with a sign-in (OAuth with PKCE).
- **Fixed: one person could switch every import sign-in off.** The table of sign-ins waiting to finish refused new
  ones once it held a thousand, and each lived ten minutes, so one account starting an import a thousand times in
  under a minute kept every other person's import and every migration job's Google or Microsoft sign-in refused
  for as long as it went on. An account or a job now keeps three at most, a fourth dropping its own oldest, and a
  full table drops its oldest instead of refusing ([Migration.md](hmailserver/docs/Migration.md)).
- **PST import**: the server reads Outlook data files itself, with a reader of this project's own written from
  Microsoft's published specifications, as a migration of kind `pst` - one job per file into one account, with the
  migration row's throttle, resume marks, report and Deck view. Mail, folders and their flags and categories come
  in, and contacts, calendar items and tasks go into the account's address books and calendars as vCard and
  iCalendar, notes and journal entries as messages. ANSI files are read, and an OST where it is a PST in all but
  its name; the 4K-page layout, Windows Information Protection and an OST caching an Exchange mailbox are refused
  with a sentence saying what to do. The reader runs in a process of its own, killed on a pause, a cancel or a
  stop, so a file that crashes it costs one message, not the service. An administrator copies the file into
  `<PstImportDirectory>/<domain>/` - empty as shipped, so no PST job runs until an administrator names a
  directory - and the job is started over REST (`POST /api/v1/migrations`), `hmctl` or the Deck
  ([Migration.md](hmailserver/docs/Migration.md)).

### Lists and bounces

- **Bounces read and a suppression list** (schema 6049): delivery reports classified and counted per address,
  hard-bounced addresses suppressed; off by default ([BounceSuppression.md](hmailserver/docs/BounceSuppression.md)).
- **Complaints read** (schema 6055): the providers' feedback-loop reports recorded and fed to the suppression
  list; off by default ([Complaints.md](hmailserver/docs/Complaints.md)).
- **A distribution list that defines itself** (schema 6074): its members given as a query over the server's
  accounts ([DistributionLists.md](hmailserver/docs/DistributionLists.md)).
- **A list manager and bounce handling for lists** (schema 6092): subscribe and unsubscribe by mail or page with
  confirmation, one-click unsubscribe, an archive, a digest, VERP and a footer, and members paused after hard or
  repeated soft bounces; nothing changes for a list until it is switched on
  ([MailingLists.md](hmailserver/docs/MailingLists.md)).

### Operations

- **The update feed is off GitHub.** The live update reads `https://updates.progressiverobot.com`, which GitLab
  Pages serves from the project's releases, in place of GitHub's Releases API, which has answered 404 since 18
  September 2026. It trusts two signers: the GitHub workflow that signed every release up to 6.3.3, for a
  certificate issued before the suspension and a release no later than 6.3.3, and this project's GitLab CI pipeline
  for a `v*` tag, at the project's path and at the group's it is to move to (below), which is to sign the releases
  from 6.3.5 and has signed none yet. 6.3.4 was built and
  Authenticode-signed by hand, before that job existed, and carries no Sigstore bundle: every server, of any
  version, installs it by hand, and a live update offered it says it cannot be verified and downloads nothing. A
  file signed for one release tag is refused as any other release; on Windows the installer's own version resource
  must name the release it is installed as; and a version with a suffix is a pre-release whatever the feed says. On
  Windows the `Updates` folder admits changes from SYSTEM, the Administrators and the service alone, and the update
  helper runs the installer, and the rollback image, only with the SHA-256 the server read when it checked them,
  through a handle it holds until they have finished. **A server from 6.2.28 to 6.3.3 still looks at GitHub.**
  Under `[Settings]` in its `hMailServer.ini`, add
  `UpdateFeedUrl=https://updates.progressiverobot.com/releases.json`, then restart the service, and its update
  check works again. Its live update can then install the re-hosted releases up to 6.3.3. The first release signed
  on GitLab, 6.3.5, will be refused by that server's live update, which trusts only GitHub's signer; installing
  6.3.4 by hand first avoids it, since 6.3.4 trusts both. Until then keep `UpdateAutoDownload` off, or such a
  server downloads a GitLab-signed installer at every check and refuses it
  ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)).
- **Stable download links.** The feed's site answers `https://updates.progressiverobot.com/download/<name>`, where
  `<name>` is a file's name without its version - `hMailServer-x64.exe`, `hmailserver_amd64.deb`,
  `hmailserver.x86_64.rpm` and the rest, each with its `.cosign.bundle` beside it - with a redirect to that file of
  the newest stable release, and `/download/` with one to that release's page. GitLab's own
  `/-/releases/permalink/latest` follows the release published last, a pre-release included; these links never do,
  and a name the stable release does not carry answers 404, never another release's file. 6.3.4 carries the x64
  installer, the amd64 `.deb` and the x86-64 `.rpm`, and no bundle
  ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md#the-download-links)).
- **The live update already trusts where the project is going.** The project is to move from the personal
  namespace `Progressiverobot/hmailserver` to the group [`hmailserver`](https://gitlab.com/hmailserver), which the
  owner reserved on 26 September 2026, and a move changes the path every signing certificate names. A server trusts
  only the signers compiled into it, so 6.3.4 carries a third: the same pipeline at
  `https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version>`, issuer `https://gitlab.com`,
  repository `https://gitlab.com/hmailserver/hmailserver`, with the project's id, `86730559`, which a transfer keeps,
  required in the certificate whatever `UpdateSigningIdentity`, `UpdateSigningIssuer` and `UpdateSourceRepository`
  say (`-` included) - so nobody who registers that path, now or later, can sign as this project, and a release
  signed after the move installs by the live update of every server from 6.3.4 on (the project moves only once a
  release signed before it has shown that Fulcio records the id). The id stays optional for the present path. A
  server that still has the three GitLab values `UpdateFeed.md` gave 6.2.28 to 6.3.3 set trusts the present path
  alone, and refuses what is signed after the move until they are cleared; a server from 6.2.28 to 6.3.3 checks no
  id, is never to be pointed at the group's path with them, and is upgraded by hand after the move
  ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)).
- **From 6.3.5, a release is made by GitLab CI**, from a protected `v*` tag. Only a Maintainer may create one, and
  since 26 September 2026 the project's only member with that role or above is its owner, whose tags these are
  ([GOVERNANCE.md](GOVERNANCE.md)). The release job refuses a tag that is not annotated, not on `master`, or not
  signed by the one key `master`'s `.gitlab/allowed_signers` lists, read from `master` and never from the tagged
  tree, and a file name the package registry already holds with other bytes; and it refuses while the security
  scans of the tagged commit have a finding the policy counts that no baseline holds and `master`'s
  `.gitlab/security-exceptions.yml` does not accept (see Build and tests). The pipeline builds the installer on
  the project's Windows runner after the whole suite has passed there; once the owner has started the `release`
  job, it Authenticode-signs the installer through Azure Artifact Signing with a federated credential made for
  that one tag, makes the signed installer's own SBOM, signs every file with Sigstore as the pipeline, checks each
  signature with the server's own `--verify-update`, and, once the installer smoke test has passed and the owner
  has started the last job,
  publishes the files to the package registry and a GitLab release, in the milestone of its version where the
  project has one, with this file's section as its notes, and refreshes the update feed. The job's
  checks run as the tagged commit has them, so they stop a mistake; what stops a hostile tag is that nobody else
  may create one. The container image, `registry.gitlab.com/progressiverobot/hmailserver`, is pushed, signed with
  cosign and scanned by the same pipeline once the project has moved to the group and the owner has switched it
  on, since GitLab.com cannot move a project whose registry holds an image; signed apt and dnf repositories on the
  feed's site are made from the stable releases' packages once the owner's signing key exists - an apt suite per
  distribution release, 6.3.4's `.deb` in `ubuntu26.04`'s, and dnf repositories only from the first release that
  carries `.rpm` files built on each distribution, since the one `.rpm` 6.3.4 carries is built on Ubuntu and goes
  in none ([PackageManagers.md](hmailserver/docs/PackageManagers.md#apt-and-dnf)). None of this has
  run: 6.3.4 was made by hand ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md),
  [RELEASE.md](RELEASE.md), [PackageManagers.md](hmailserver/docs/PackageManagers.md)).
- **Upgrades without a booked window**: a drain closes the listeners before it waits, the live update drains
  first, and with `UpgradeOnStart=1` in `[Database]` (a new bootstrap key, 0 by default) the service applies
  the schema steps itself as it starts ([Upgrading.md](hmailserver/docs/Upgrading.md)). A rolled-back update's
  outcome is reported as soon as the service is back.
- Fixed: on MySQL and MariaDB, two nodes applying the schema steps at once - both with `UpgradeOnStart=1`, or one
  and `--upgrade-database` - were not serialised: the version-row lock the second waited on was freed by the
  step's first DDL statement, so it ran the same step beside the first and stopped with a table or a column
  already there. Each step now holds a named lock (`GET_LOCK`) until it is over. A server that refuses
  `GET_LOCK` (MariaDB Galera) still upgrades, with the row lock alone, and says so for each step.
- **Backups**: encrypted with AES-256, the passphrase never inside the archive (schema 6059); one mailbox or one
  message restored from an archive ([Backup.md](hmailserver/docs/Backup.md)).
- Fixed: a backup with **Back up messages** copied `<data directory>\Confidential` and `<data directory>\Protected`,
  so the whole of every live message that expires - attachments and all, in the clear even for a mailbox encrypted
  at rest - and the ciphertext of every password-protected message outlived its expiry and withdrawal in each
  archive, and a full restore deleted and replaced both folders. Both are now left out of the backup and left in
  place by a restore, as the recoverable store and the eDiscovery files are. Archives taken by earlier 6.3.4 builds
  still hold those copies ([Backup.md](hmailserver/docs/Backup.md)).
- Fixed: a restore whose backup index was cut short or altered - ending inside a tag, a comment or a quoted
  value, or holding an entity reference without its `;` - read past the end of the index. Such an index is
  refused as malformed - a comment left open after its closing tag is only ignored - and a reference without
  its `;` is kept as written.
- **Monitoring**: syslog (RFC 5424, or the journal under systemd; off) ([Syslog.md](hmailserver/docs/Syslog.md));
  per-domain reports ([Reports.md](hmailserver/docs/Reports.md)) with spam, virus and storage figures per domain
  (schema 6062); the DMARC and TLS-RPT reports a domain receives, read and charted (schema 6070)
  ([DmarcReports.md](hmailserver/docs/DmarcReports.md)); outbound webhooks for five mail events (schema 6051)
  ([Webhooks.md](hmailserver/docs/Webhooks.md)); a daily check of the server's own addresses against block lists
  (schema 6057), on in a new installation and off after an upgrade
  ([ReputationWatch.md](hmailserver/docs/ReputationWatch.md)).
- **Fixed: on Linux, a syslog collector that went quiet held the service.** Over TCP or TLS the sink's connection,
  handshake and writes had only the socket's own timeouts, which Asio's synchronous calls do not honour on Linux,
  so TLS pointed at a plain-TCP port, or a collector that accepted and stopped reading, stopped the sink for good
  and left a reinitialise or a service stop waiting on it until systemd killed the service. Each step now has ten
  seconds and is given up on after that ([Syslog.md](hmailserver/docs/Syslog.md)).
- **Fixed: on Linux, an OpenTelemetry collector that went quiet held its exporter.** The OTLP exporters' three-second
  send and receive deadlines were passed in the Windows form, which Linux refuses, so a collector that accepted the
  connection and never answered held the exporter, and the stop that waits for it, for good. Linux is given the
  deadlines in its own form. This was so in every release with Linux packages, from 6.3.0.
- Fixed: the reports (`GET /api/v1/reports`) keyed and wrote every address and domain through the conversion the
  audit trail's chain was fixed of, so two senders or recipients at a local domain whose names differ after a
  character above U+00FF were counted as one row, named as zero bytes on Windows and question marks on Linux, in
  the JSON and in the CSV. Names are now keyed and written as UTF-8, domains lower-cased in A to Z as before, and the
  `domain` parameter is read as UTF-8. A trace row that holds an address as the SMTP command line gives it, its
  UTF-8 widened one byte to a character, is written as the UTF-8 it stands for, as it was, and counted with the
  same address decoded.
- **Linux**: the live update applies on Linux through a root helper outside the server's unit; a Helm chart and a
  Compose file beside the container image; `hmailserver --set-setting`, `--sample-domain` and
  `--apply-environment`.
- **Windows packages**: `winget` and Chocolatey manifests rendered from the installer a release actually
  published ([PackageManagers.md](hmailserver/docs/PackageManagers.md)); they are not in the public repositories
  yet.
- **HTTP**: gzip and deflate on both listeners for the answers that hold no secret
  ([ResponseCompression.md](hmailserver/docs/ResponseCompression.md)).
- **Storage compression and attachment de-duplication** (schema 6098): with `MessageStoreCompression` on (off as
  shipped), a message delivered or appended is stored deflated, and each MIME part of at least
  `MessageStoreDedupMinKB` (256 KB) is kept once for every message that carries the same bytes. Every reader sees
  the message as it arrived, and a stored file is checked by its SHA-256 before it is served; the queue is never
  compressed. `--measure-store` says what it would save and changes nothing, `--compress-store` and `--compress-store --decompress`
  rewrite what is stored, and `GET /api/v1/store`, the Deck's dashboard and the Control Panel's Performance page
  say what it saves. With it on, `Message.Filename` over COM names the stored form
  ([MessageStore.md](hmailserver/docs/MessageStore.md)).
- **Object storage for the message store**: messages on an S3-compatible store (AWS S3, MinIO, Wasabi, Backblaze
  B2, Ceph RGW) or Azure Blob Storage, with the database as the index. A message is written to the local disk first
  and moved into the store by a background committer, so a store that cannot be reached delays nothing and loses
  nothing; IMAP and POP3 are told to try again for a message only the store holds. TLS to the store is always
  verified. `--copy-store-to-object-storage` and `--copy-store-from-object-storage` copy both ways, verified and
  resumable. Off until an administrator names a store: `MessageStoreBackend` is `file` as shipped, and a server
  with no store reads and writes as before ([ObjectStorage.md](hmailserver/docs/ObjectStorage.md)).
- **Event scripts on Linux**, in JavaScript, in the QuickJS engine (Fabrice Bellard's release 2025-04-26, vendored
  as source), for the same events under the same function names with the same objects; `ScriptLanguage` takes a
  third word, `JavaScript`. Until now a Linux server refused any script. Windows keeps VBScript and JScript and
  refuses JavaScript at load. A script has no file, process or network access, and `ScriptTimeout` and a 64 MB
  ceiling bound it ([EventScripts.md](hmailserver/docs/EventScripts.md)).
- **SNMP**: a read-only SNMPv3 agent (authPriv only: HMAC-SHA-256 and AES-128, with SNMPv2c optional and off)
  answering the mail MIB (RFC 2789) and the standard groups from the same counters `/metrics` serves. Off, and
  answering nobody until a manager's address is in `SnmpAllowedAddresses`; its settings are on REST and applied at
  a restart, with no COM property or Control Panel page. Its enterprise arc is RFC 5612's documentation number, not
  one registered to this project ([Snmp.md](hmailserver/docs/Snmp.md)).
- **The live update's files are held on Windows.** Beside what the update-feed item says, a repaired `Updates`
  folder is emptied, one owned by another account is refused (`HM6894`), the folders are held open through the
  apply so that nothing above them can be swapped, the service's own SID is given Modify there, and the update
  helper loads DLLs from System32 alone ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)).
- **The Windows installer**, after its first smoke test on a clean Windows Server 2025: an upgrade no longer writes
  a Critical `HM5011` event to the Application log; the uninstaller waits for the service's process to end before
  it deletes anything, so `hMailServer.exe` and its DLLs are no longer left in `Bin`, and a file still in use is
  removed at the next restart; the event log source is removed; and an uninstall after an upgrade no longer leaves
  the COM AppID behind.
- Fixed: an administrator password with a space, a double quote or backslashes before one failed a new
  installation at the database step, and a silent upgrade that moved the schema, with exit code 3: the installer
  put it unquoted on `DBSetupQuick.exe`'s command line, and DBSetupQuick on `DBUpdater.exe`'s (`DBSetup.exe`,
  run for an external database, asked for it again). Both hand it on in the environment variable
  `HM_ADMIN_PASSWORD` now, the installer to `DBSetup.exe` too, as it already did for `hMailServer.exe
  --set-admin-password`, so it is on no command line. An unattended install may give the installer the password
  in `HM_ADMIN_PASSWORD` in place of `/adminpassword=`, which cannot carry a double quote (Inno Setup drops it)
  and which anybody listing processes can read; it is set in an elevated prompt, since it does not cross the UAC
  prompt ([Upgrading.md](hmailserver/docs/Upgrading.md)).
- **The product points at GitLab**: the Control Panel's About and stalled-mail links, the `Policy:` line of the
  `security.txt` the server serves, the systemd units' `Documentation=` and the Linux packages' homepage. The
  update feed field is labelled "Update feed URL" in all eighteen catalogues. So do the Arch Linux PKGBUILD's `url`
  and the release tag it clones, the Helm chart's home and sources, and the winget and Chocolatey templates'
  addresses and installer, which is the one in GitLab's package registry. The container image is named
  `registry.gitlab.com/progressiverobot/hmailserver` in the Compose file and the chart; nothing is published there
  yet (above), and the images published on `ghcr.io` before the suspension answer 404. The update check no longer
  sends GitHub's `X-GitHub-Api-Version` header. The release tags' signing keys are in `.gitlab/allowed_signers`,
  which was `.github/allowed_signers`, and the VEX document is `.gitlab/hmailserver.openvex.json`.
- **The project on GitLab**: issue templates for a bug, a feature, a question and a security report, the first
  three asking for the version, the platform, the database and a log excerpt; a merge request template; the
  Service Desk, an address for anyone without a GitLab account, whose message becomes a confidential issue
  (README's Getting help, [SUPPORT.md](SUPPORT.md)); the releases as an Atom feed; and the README's list of where
  each of these is. [GOVERNANCE.md](GOVERNANCE.md) gives the maintainers' roles on GitLab, and
  [Mirroring.md](hmailserver/docs/Mirroring.md) how to push-mirror the repository to a second forge, and the
  weekly export of the issues, merge requests, releases and wiki that a mirror does not carry; neither is set up
  yet.
- Fixed on Linux: a character-set conversion that failed read an unterminated buffer, where Windows had
  substituted; it now gives an empty string.
- Fixed on Linux: the name of the account the service runs as was read with `getpwuid`, whose answer is one
  buffer for the whole process, while the setup checklist, auto-ban's firewall check and the live update ask for
  it from threads of their own, so two at once could read each other's entry or a torn one. It is read into a
  buffer of the call's own (`getpwuid_r`).
- Fixed: the container image built from `hmailserver/source/Server/platform/packaging/docker/Dockerfile` installed
  the `.deb` without its two Recommends, so it had no 7-Zip - a backup failed with "Could not add the backup
  settings to the archive" - and no Python 3 for `hmctl`. It installs `7zip` and `python3` with the package.
  No image of 6.3.4 is published (above); this is the image built from the file.

### Build and tests

- The native server's line coverage was measured for the first time: 73.48% over the whole Windows regression
  suite. The three MIME fuzz harnesses gained a nightly job, and the Linux regression suite was split in two
  halves; both ran on GitHub Actions until 18 September 2026, and on GitLab CI the fuzz job is `linux-fuzz`, nightly once the owner creates its schedule,
  and the Linux suite is one job, `linux-suite`, on the project's own runner (see the CI item below).
- Fixed: the MIME fuzz harnesses did not build. Since encryption at rest, a message the MIME code cannot parse is
  kept aside in "Problematic messages", which calls `FileUtilities::Exists` and `CreateDirectory`, and the
  harness's stand-in for `FileUtilities` lacked both, so `linux-fuzz.sh` and `build-fuzz.ps1` stopped at
  `Mime.cpp`. It gains both, answering false like its other writers, so the harness still writes nothing; the
  parser is unchanged. Found by 6.3.4's release fuzz run.
- Twenty-one new checkers under `build/` hold the rules the batches added, among them `check-account-stores.py`
  (what a backup must carry), `check-com-error-info.py` (COM refusals a caller can read),
  `check-collection-iteration.py` (locked iteration over shared collections), `check-com-dispids.py`,
  `check-openapi-blocks.py` and the audit and role choke-point checks.
- The three GnuPG proofs of the gateway-encryption and zero-access fixtures run on Windows for the first time:
  they had handed Git for Windows' gpg a home directory it could not read.
- A load generator in the repository, `build/perf/load/loadgen.py`: messages per second in and out, IMAP sessions
  per core under a latency limit, memory per session and Dovecot's imaptest as a benchmark, against any server,
  with the machine's details in every result; and a lab recipe that runs it against hMailServer and against
  Postfix and Dovecot on the same machine under stated fairness rules
  ([PublishedNumbers.md](hmailserver/docs/PublishedNumbers.md)). No numbers have been measured with it yet.
- **Windows on ARM, compiled but not run.** A `Release|ARM64` configuration in the server's three projects,
  `-Platform ARM64` in `build.ps1`, `build-tools.ps1`, the three library scripts, `get-installer-binaries.ps1` and
  `get-dotnet-runtime.ps1`, and a second installer, `hMailServerARM64.iss` (`hMailServer-<version>-arm64.exe`),
  sharing the x64 installer's sections. The ARM64 server uses SQL Server or PostgreSQL; it has no built-in
  database, since SQL Server Compact has no ARM64 build, and no bundled MySQL client. An ARM64 server's update
  check looks for the ARM64 installer, and `VersionArchitecture` says `ARM64`. It was first compiled on
  26 September 2026, on the x64 development machine: the three ARM64 libraries, the server, the tools and an
  ARM64 installer all built, every native image an ARM64 one. That compile found two faults, both fixed for ARM64
  only: libpq's cross build wanted a time zone compiler that runs on the build machine, and is now built with
  the system's time zone data, which libpq never reads; and the tools were compiled against the x64-marked COM
  wrapper, which .NET will not load into an ARM64 process, so `generate-com-wrapper.ps1 -Platform ARM64` makes a
  machine-neutral one, `build-tools.ps1 -Platform ARM64` points the tools at it through the `HmInteropDirectory`
  property and a new `Tools/Directory.Build.targets`, and the ARM64 installer ships it. Nothing of it has run:
  the project has no ARM64 hardware, and 6.3.4 carries no ARM64 installer; the x64 build, its COM wrapper and
  its SBOM are unchanged. `hmailserver/docs/WindowsOnArm.md`.
- **CI on the project's own GitLab runners.** `.gitlab-ci.yml` runs the platform-neutral checkers, the Deck's and
  the webmail's script harnesses and, for a merge request from a fork, a sign-off check on GitLab's shared runners;
  and, for a push to `master`, a `batch*` branch or a `v*` tag only, the Linux build, its tester programs, the
  whole Linux suite, the packages, the container image, the cross-compile for AArch64, the database upgrade gates
  and the SBOMs on `linix`, and the Control Panel, the tools and the whole Windows suite on `bench150`. A nightly
  pipeline fuzzes the three MIME harnesses once the owner has created its schedule. Nothing reached from GitHub
  is used. Until 26 September 2026 only the update feed's `pages` job had run there, and the ported Linux scripts
  were run by hand on `linix`. The first pipeline of the whole file ran on `master` at `40c7d3e0c` that day
  (pipeline 2884863496), and it finished failed. The checkers, the Linux build and its packages, image,
  cross-compile, sanitizer build and SBOMs, and the update feed passed; `harnesses` and `update-helper` failed on
  the shared runners, `linux-suite` and both database upgrade gates on `linix`, `windows-gate` and
  `windows-dotnet` on `bench150`, and `dependency-scan`, `repo-hygiene`, `semgrep` and `static-analysis`, which
  may fail without failing the pipeline. Each failure was the tree's or the job's, not a fault of the server's, but
  for two of the findings `static-analysis` reported; and 6.3.4 fixes each but that job's: the helper job checked
  the systemd units on a checkout the runner makes world-writable; the portal harness depended on the runner's time
  zone; the CI image had no 7-Zip, which the suite's backup tests need; three tests assumed the SMTP port, the
  order two messages land in, and an INI section to be a setting; the upgrade gate wound a new database back to
  6029 by reading the later steps, and now starts from the create scripts of 6029 themselves and compares what it
  upgrades to with a new database, which found two columns of `hm_bouncecounts` the create scripts left without the
  default the 6055 step gives them (they now have it); GitLab refused two scans' reports, each two files of one
  type, and each now uploads one; `repo-hygiene` found six images not pinned by digest and a password written in
  the file for the upgrade gate's own database, and judged a job by whether it extends `.own-runners` rather than
  by the rules it runs by; and each finding Semgrep reported beyond its baseline is fixed, or suppressed with the
  reason. Of the findings `static-analysis` reported beyond its baseline, the two that were defects of the server's
  are fixed (above); the others have not yet been read into its baseline, so that job still fails. The owner's edit
  of the README on `master` started a second pipeline, which failed on `harnesses` the same way. No pipeline has
  run on the fixed tree yet ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md)).
- **The CI after its first review on GitLab.** The fuzz, Renovate, whole-history secret scan and export pipelines
  are chosen by their schedule's description - `fuzz`, `renovate`, `security`, `export` - since the project
  refuses pipeline variables and GitLab counts a schedule's as such; only the update feed's schedule exists yet.
  A later push no longer cancels the Linux suite or the Windows gate half-way, and a push that changes only
  Markdown at the repository's root or under `.gitlab/` starts nothing on the project's runners. A busy bench is
  a warning, except in a release tag's pipeline. The fuzz job's findings are its artifact, for Developers only,
  and kept out of the public job log, where an input could be an exploit. cosign's image is pinned by
  digest (2.6.5). GitLab's own SAST (`semgrep-sast`, which adds the server's C and C++) and secret detection
  (`secret_detection`, which fails on a finding; `.gitleaksignore` names the thirty findings of a scan of the
  whole history and the twelve the landing's scan found in this release's own commits, none of them a secret) run
  on the shared runners for `master` and merge requests.
  `forge-export` writes the weekly archive [Mirroring.md](hmailserver/docs/Mirroring.md) describes, and
  `package-repos` the apt and dnf repositories, each waiting for the owner's token or key.
  `build/check-release-chain.py` holds all of it, the release jobs and the container image's gate included, and
  `build/check-forge-export.py` the export.
- **Changes land through merge requests, signed off, from the first landing after 6.3.4.** A batch reaches
  `master` through a GitLab merge request,
  by fast-forward, once its branch's pipeline has passed and every thread is resolved: `build/land-via-mr.py`
  checks the batch, pushes it, opens the merge request, waits for the pipeline, merges without squashing, and
  records on the merge request whether somebody other than the author approved; without that approval - Zain Ul
  Abidin's, or the owner's for Zain's own, given on the tip it lands - it refuses to merge, unless the owner
  overrides it for one landing with `--land-without-approval "<reason>"`, which it records. From the first batch
  landed this way, every commit that reaches `master` carries its author's `Signed-off-by`, the maintainers'
  included: the `sign-off` job checks every merge request and every `batch*` branch
  (`build/sign-off.py`, which the landing script uses too), and Renovate signs its commits off. `master`'s rule is
  to take no direct push once the first batch has landed this way (`hmailserver/docs/ContinuousIntegration.md`,
  "Landing on master"). No batch has landed this way on GitLab yet; both scripts' self-tests run in `checkers`.
- **Nothing is downloaded from GitHub to build**: the seventeen GitHub workflows, Dependabot and the CodeQL
  configuration have left the tree and are in its history; `libraries\build-openssl.ps1` takes OpenSSL's release
  archive from Debian's archive, `openssl_<version>.orig.tar.gz`, which is OpenSSL's own tarball, held to the same
  pinned SHA-256, or from `-Archive`; `build-appimage.sh` uses linuxdeploy only from `TOOLS_DIR` and otherwise
  refuses; winflexbison comes from SourceForge; and the installer's build inputs come from GitLab's package
  registry.
- **Security scanning** on the same runner, in place of CodeQL, dependency review and Scorecard: cppcheck and
  clang-tidy held to a committed baseline, Semgrep, Trivy and OSV over the lock files, the CI image and the
  vendored libraries, and the repository checks Scorecard ran. They hold a release, not a change (next item).
- **A release is held by its security scans, and a secret is refused before it is pushed.**
  `build/ci/security-gate.py` reads what `static-analysis`, `semgrep`, `semgrep-sast` and `dependency-scan` found
  in the release's commit, and the release job - or, for a release made by hand, its runbook - refuses while one
  of them did not finish or found something `SECURITY.md`'s policy counts that is in no baseline and is not
  accepted, for at most a year, in `.gitlab/security-exceptions.yml`, which the owner keeps on `master`. That file
  accepts 455 findings at the release - 188 of GitLab's Semgrep analyzer, 263 of the static analysis and 4 of
  Semgrep's open rules, from the scans of `40c7d3e0c` and of the release tree - each read by two review agents
  under the owner's delegation and each with the reason it is not exploitable; the seven it found real are fixed
  in this release (under Security and identity). `build/scan-secrets.py` runs gitleaks with GitLab's rules in the
  two git hooks the repository ships (`build/hooks`, turned on with `git config core.hooksPath build/hooks`) and
  in the maintainer's landing, which pushes nothing while it finds a secret or cannot scan
  ([SECURITY.md](SECURITY.md), [CONTRIBUTING.md](CONTRIBUTING.md), "Secrets").
- **Dependency updates by Renovate** on the same runner, with nothing read from GitHub; inert until the owner
  creates its token and schedule.
- **A test lab on `linix`**: MySQL 8.4, MariaDB 11.4, PostgreSQL 17 and SQL Server 2022, Keycloak for the OpenID
  Connect and SAML sign-ins, Postfix and Dovecot as peers, Dovecot's `imaptest`, and Uptime Kuma
  ([TestLab.md](hmailserver/docs/TestLab.md)). Its first runs found the unencrypted MySQL connection, the
  certificate check that never read an iPAddress entry and the IMAP answers fixed above.
- **The installer smoke test** installs, upgrades and uninstalls on a throwaway Windows Server 2025 VM, from a
  snapshot, checking what the installer promises for each (`build/ci/installer-smoke.ps1`); its first run, of 6.3.2
  upgraded to 6.3.3, found the four installer defects fixed above.
- **An SBOM for every compiled file a release carries**: from 6.3.4, the Windows installer, the `.deb` and the
  `.rpm` each go out with `<file>.cdx.json`, a CycloneDX 1.6 document whose subject is that file by its SHA-256
  and SHA-512 and whose components are what the file holds and needs, made by `build/asset-sbom.py`, which also
  checks one against its file. The five GitLab releases before 6.2.3 that carry an installer carry one too since
  26 September 2026, made from each installer ([SECURITY.md](SECURITY.md), "Software bills of materials").
- **Every build input pinned and named**: the .NET Desktop Runtime the installer bundles is pinned to 10.0.12 by
  version and SHA-512, where it was whatever Microsoft's 10.0 link gave on the day; the SBOM names seventeen
  components, among them zlib, the installer's fetched files and the Linux packages' system libraries, and
  `build/check-sbom-inputs.py` fails when a build input is missing from it.
- New checkers: `check-message-store-seam.py`, `check-com-shared-copies.py`, `check-mysql-charset.py`,
  `check-installer-scripts.py`, `check-ci-image.py` and `check-sbom-inputs.py`.

Full notes: [the 6.3.4 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.4).

---

## [6.3.3](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.3) - 2026-09-15

Large outbound messages sent with CHUNKING no longer stall, databases older than 6.3.2's schema upgrade again,
the webmail was rebuilt to the shape of Gmail and Outlook.com, the account's address book is served over CardDAV,
and the browser Control Deck was measured against the desktop Control Panel and brought close to it.

**Delivery**

- An outbound BDAT chunk larger than one 60,000-byte send buffer no longer stalls (GitHub issue 261): every
  message over 60,000 bytes to a server advertising CHUNKING had stalled since outbound BDAT arrived.
- The full-text indexer no longer reports an error for a message deleted before its terms were saved.

**The webmail**

- Rebuilt to the shape of Gmail and Outlook.com: one search box, a navigation column, a reading pane right, below
  or off, Undo after every move, a compose window that docks or pops out, and replies written under the message.
- Pin (`$Pinned`), mute (`$Muted`), block a sender, a right-click menu and the inbox in tabs, Gmail's or
  Outlook's way, decided by the server from the headers; follow-up flags with due dates, nudges and quick steps.
- The last ten searches offered again, contact names completing to `from:`, and fourteen more search operators.

**CardDAV**

- The account's address book on phones and desktop clients (RFC 6352) on the web-services HTTPS listener, with
  the card a client sends stored as sent (schema 6040) and the IMAP logon's lockout and auto-ban.

**The Control Deck and REST**

- `build/check-deck-parity.py` measures the desktop Control Panel against REST and the Deck; of 330 properties,
  328 became writable over REST and 322 reachable from a Deck view on 14 September.
- New write routes for the anti-virus settings, account and domain fields, lists, the anti-spam lists, incoming
  relays and, under an account's address, its app passwords, folder permissions and messages.
- A domain's relay password could not be saved on any Windows installation, because a DPAPI envelope is longer
  than the column; schema 6039 widens it.

**Security, from the release's own review**

- The REST routes for the `[Settings]` section of `hMailServer.ini` answered to any API key, so a read-only key
  could read its secrets; they answer to the administrator password only. A key restricted to a domain could
  lift that domain's limits; it is refused them with 403. CardDAV's request handling is bounded.

**Schema:** 6038 to 6040 (6039 widens a domain's relay-password column; 6040 adds the CardDAV tables).

**Upgrading**

- 6.3.2 could not upgrade a database older than its own schema (GitHub issue 263): every upgrade from 5.x or
  6.2.x stopped with "The server has not loaded its configuration". From anything older than 6.3.1, take 6.3.3;
  nothing in the database was changed by a failed 6.3.2 attempt.
- A live update started from an open Control Panel failed with the installer's exit code 5. It is fixed, but the
  helper that runs an update is the one already installed, so on 6.3.1 or 6.3.2 update by hand once: close the
  Control Panel and run this installer.
- On PostgreSQL a backslash is now stored as one backslash; values already stored doubled stay as they are, so
  edit and save them once.
- Until you upgrade, `OutboundChunking=0` is the workaround for the BDAT stall.

Full notes: [the 6.3.3 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.3).

## [6.3.2](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.2) - 2026-09-13

The webmail became a mail client, Kerberos single sign-on arrived on all three protocols, an auto-ban reaches the
operating system's firewall, the Linux packages install on the next distribution release, and thirty-seven
binaries left the repository.

**The webmail**

- Reading: conversations by thread; Archive, Junk and Trash on one key each, with a move into or out of Junk
  teaching the spam filter as an IMAP `MOVE` does; a search syntax with operators; labels as IMAP keywords
  (schema 6036) that every IMAP client sees; image attachments shown and PDFs previewed; warnings for a link
  whose text names another site and for a sender named like a contact.
- Writing: a formatting bar; a From picker offering exactly what SMTP submission would accept; signatures,
  templates, undo send, read receipts (RFC 8098) and one-click unsubscribe (RFC 8058) to public addresses only;
  send later and snooze (schema 6034); an address book collected from what the account sends (schema 6032);
  large files sent as links that expire (schema 6035).
- S/MIME in the browser with the Web Crypto API, the private key wrapped under a key derived from the account
  password (schema 6037), which the server cannot open.
- Settings, Away, Filters (a rules editor that writes Sieve), Contacts, Storage, Files and Security pages; the
  seventeen Control Panel languages plus Greek, Korean and European Portuguese; installs as an app and reads
  the inbox offline; branding per server or domain.

**Authentication**

- SASL GSSAPI (RFC 4752) on SMTP, IMAP and POP3 on Windows, off unless `GssapiEnabled=1`.
- `AutoBanFirewall=1` puts a banned address into Windows Defender Firewall; on Linux a packaged hook keeps an
  nftables set, with a fail2ban filter and jail; `AutoBanCommand` and `AutoBanNeverBan` beside it. All off.
- Making an app password requires the account's own password, and never accepts an app password for it.
- A SURBL server's answers are judged against an expected result (schema 6038); without one, the codes with
  which Spamhaus refuses a query are no longer taken as listings (GitHub discussion 167).

**Linux**

- Boost is linked statically, so the `.deb` installs on Ubuntu 26.04.
- A container image from every tag, whose entrypoint writes the database section, the REST listener and the
  administrator password from the environment and creates or upgrades the database.

**Database**

- The 6029 to 6030 step sweeps parents before children, fixing the orphan-sweep defect 6.3.1 named, in all four
  backends.
- Every statement in a MySQL script is followed by a blank line, and `build/check-sql-separators.py` keeps it so.

**Build**

- Thirty-seven of the forty committed binaries left git; `build/get-installer-binaries.ps1` fetches them from
  the `build-inputs-1` release against the SHA-256 in the manifest, and the COM wrapper is generated.
- A server holding a database whose schema it refused (HM5011) answers COM with the refusal instead of an access
  violation.

**Schema:** 6031 to 6038 (6032 to 6037: contacts, account preferences, scheduled sends and snoozes, files sent as
links, message keywords, S/MIME keys; 6038: the SURBL expected result).

**Known afterwards:** 6.3.2 could not upgrade a database whose schema was older than its own (GitHub issue 263),
and its live update from an open Control Panel failed; 6.3.3 fixes both.

Full notes: [the 6.3.2 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.2).

## [6.3.1](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.1) - 2026-09-11

The first release whose Windows installer carries an Authenticode signature. Nothing in the server changes: the
compiled server differs from 6.3.0 by its version stamp and one comment line.

**Signing**

- The installer is signed with Azure Artifact Signing against a certificate issued to Progressive Robot Ltd, so
  the elevation prompt names the publisher and a policy that refuses unsigned binaries no longer refuses it.
  SmartScreen still warns until reputation accumulates.
- Only the Windows installer is Authenticode-signed; the Linux packages have no such signature, and the
  Sigstore bundle beside every asset is unchanged.
- `UpdateRequireAuthenticode=1`, in the server's update settings since 6.2.28, becomes usable on Windows,
  because the installer it downloads now carries a signature.

**The release path**

- The release workflows were corrected before the certificate was first used: the signing gate checks all six
  of its settings, every attached asset is verified as it stands on the release, both SBOMs are required before
  anything is signed, an RFC 3161 countersignature is required, and runs for one tag are serialised.

**Documentation**

- The README offers the Linux packages on its download line; two files that named a version 6.2.29, which never
  existed, now say 6.3.0.

**Schema:** 6031, unchanged.

**Known, and fixed in 6.3.2:** the orphan sweep of the 6029 to 6030 step ran children before parents, so an
upgrade from below 6030 of a database that already held orphaned rows could refuse its own foreign keys. It
failed loudly and rolled back.

Full notes: [the 6.3.1 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.1).

## [6.3.0](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.0) - 2026-09-10

hMailServer runs on Linux: a mail server that had been Windows-only since 2002 builds, installs, runs and delivers
mail on x86-64 and AArch64 Linux from the same source tree, with the Windows build unchanged in what it compiles
and how it behaves. Because a Linux server has no Control Panel and no COM, the REST API gained a large write
surface and the browser Control Deck drives most of it.

**Linux**

- Every core translation unit compiles under clang on x86-64 and AArch64, and the whole core builds and links
  with GCC on x86-64; `build/linux-tu-census.sh` counts them file by file.
- A `.deb` and an `.rpm` for both architectures, a `PKGBUILD` for Arch and an AppImage, each with a systemd unit
  that runs the server as its own user, a logrotate rule, the configuration under `/etc/hmailserver` and the
  Control Deck.
- `hmailserver --create-database`, `--upgrade-database`, `--set-admin-password` and `--check-config`.
- Proven against PostgreSQL 18 and MariaDB 11.8 from schema creation to a message delivered and read back over
  IMAP; directory authentication against slapd over StartTLS and LDAPS; stored secrets under AES-256-GCM with a
  key file the installation owns, where Windows uses DPAPI.
- The same commit built twice on the hosted runners gave byte-identical binaries.

**REST writes what only COM could write**

- `PUT /api/v1/settings` with its anti-spam and logging groups (108 settings, each through the Control Panel's
  own setter), global rules, SMTP routes, aliases, accounts with their password and administrator level,
  certificates and the listeners that bind them, and `POST /api/v1/server/reinitialize`.
- Refused to a read-only key and to a key issued for other domains; the OpenAPI document is valid JSON for the
  first time.
- Not yet written over REST: a domain's properties beyond its active flag and postmaster, DKIM signing, the
  per-domain size limits, the domain signature and a per-domain relay host.

**The Control Deck**

- Editors generated from the server's own OpenAPI document; rules, routes, certificates and listeners created,
  edited and deleted; accounts created and deleted; the services restarted in place.
- It no longer keeps the administrator password: `POST /api/v1/session` exchanges it once for an `HttpOnly`,
  `SameSite=Strict` session cookie that ends when the password changes, and a write must carry
  `X-Requested-With`.

**The webmail**

- The portal at `/portal` became a webmail: addresses for folders and messages, the Control Deck's look, new
  mail noticed without a reload, folders created, renamed and deleted, and inline images shown without loosening
  the frame's sandbox. `build/check-portal-script.py` runs the page's script in CI.

**Under the surface**

- Non-ASCII folder names and non-ASCII command lines are handled correctly on Linux.
- A TLS key-exchange group list OpenSSL rejects is reported once, not for every listener and delivery.
- The administrator's user name is no longer translated in the Control Panel's sign-in box (GitHub issues 156
  and 177); an installation-paths diagnostic lists every configured directory (GitHub issue 158).
- A Linux regression suite, `hmailserver/test/LinuxRegressionTests`, compiles 260 of the suite's own files in
  place and runs them against the Linux server.

**Schema:** 6031, unchanged.

**Upgrading**

- Windows: run the installer over the existing installation. Nothing new is on by default; the Control Deck and
  the portal need `RestApiPort`.
- Linux, first installation: install the `.deb` or `.rpm`, then `hmailserver --create-database`,
  `hmailserver --set-admin-password` and `systemctl enable --now hmailserver`.
- Moving a Windows installation to Linux is not a supported migration.

**Known and unfixed:** the installer is not Authenticode-signed (6.3.1 is the first that is); per-domain DKIM
cannot be configured on Linux; the Linux regression run skips what no route reaches yet.

Full notes: [the 6.3.0 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.0).

## [6.2.28](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.28) - 2026-09-08

The largest release of the fork so far: the server can update itself, an account can use its mailbox in a
browser, and the Control Panel speaks seventeen languages, with a real HTTP/1.1 server under the REST API and the
web services. Everything new is off until it is turned on.

**Live update**

- `UpdateCheckEnabled` (0 by default) reads the project's release feed every `UpdateCheckHours` (24); fetching
  and applying an installer are two further opt-in steps.
- What is applied is verified against the release's Sigstore bundle, which cannot be turned off; the settings
  `UpdateTrustRootsFile`, `UpdateSigningIdentity`, `UpdateSigningIssuer`, `UpdateSourceRepository` and
  `UpdateLogPublicKeyFile` point it at a private Sigstore instance.
- `hMailServer.Updater.exe` runs the installer with the service stopped, waits `UpdateServiceWaitSeconds` (180)
  and reinstalls the previous version if the service does not come back; `UpdateBackupBeforeApply=1` refuses an
  unattended apply without a successful backup.
- `HttpProxy=host:port` sends every web request the server makes as a client through a forward proxy.

**The mailbox in a browser**

- `/portal` on the REST listener, and `/api/v1/me` behind it, answer to an account's own credentials only: read,
  search, flag, move and delete; send with attachments, drafts, reply, forward and Bcc; the account's quarantine,
  forwarding, signature, vacation reply, Sieve script and password.

**Under it**

- The REST API and the web services run on `HttpServer`: HTTP/1.1 on Boost.Asio with keep-alive, chunked bodies
  and header and body limits.
- IMAP `COMPRESS=DEFLATE` (RFC 4978), behind `IMAPCompressionEnabled`.

**The Control Panel and the Control Deck**

- The Control Panel in Czech, Danish, German, Spanish, Finnish, French, Italian, Japanese, Norwegian bokmål,
  Dutch, Polish, Brazilian Portuguese, Russian, Swedish, Turkish, Ukrainian and Simplified Chinese, beside
  English: 3,376 texts, chosen in the connect card and remembered per user.
- The Control Deck reads the settings, the log files, the certificates and the global rules.

**Fixed**

- A masked password was typed backwards from the second character (GitHub issue 156).
- Saving the SSL/TLS page dropped the post-quantum key exchange groups, and saving the Scripting page took every
  COM object from every script.

**Schema:** 6031, unchanged.

**Upgrading:** this is the first release that carries the live update, so the upgrade to it is manual.
`UpdateCheckEnabled`, `RestApiPort`, `IMAPCompressionEnabled` and `HttpProxy` are all opt-in; a server upgraded
without touching its settings behaves as it did on 6.2.27.

**Known and unfixed:** the Control Deck reads but does not write, and holds the administrator password in
`sessionStorage` while it is open; the regression suite runs on Windows only.

Full notes: [the 6.2.28 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.28).

## [6.2.27](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.27) - 2026-09-07

Two roadmap items open since the roadmap was written: the validating resolver proves a missing DS record missing
rather than assuming it, and an external IMAP account can mirror every folder of a remote mailbox.

**Security**

- A DS query answered with nothing is no longer taken as an unsigned delegation: the resolver requires the
  parent's signed NSEC or NSEC3 proof (RFC 4035 section 5.2, RFC 5155 section 8), and a delegation without one
  under a signed parent is Bogus. `Diagnostics.DnssecChainStatus` reports a chain.

**Mail flow and migration**

- `FetchAccount.MirrorFolders` (schema 6031) mirrors every folder of a remote IMAP mailbox byte for byte, with
  flags and internal dates, without delivering anything through the filters; *days to keep messages* 0 makes it
  a move.
- The Import Tool reads a Maildir, with the INBOX, every Maildir++ folder and the flags in the file names.
- `hmailserver/docs/Migration.md` describes each route into this server and what it keeps.
- A message imported into a folder a client has open now appears at once.

**Schema:** 6030 to 6031 (one column on `hm_fetchaccounts`).

Full notes: [the 6.2.27 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.27).

## [6.2.26](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.26) - 2026-09-06

The release to take if you run the built-in SQL Server Compact database. Nothing else in the server changes.

**Fixed**

- Every SQL Server Compact upgrade through schema 6030 was reported as failed after it had succeeded, and the
  check that gave the false verdict took the service down with it (GitHub issue 114). The four verification
  probes for the foreign keys crashed the Compact Edition provider; they are rewritten so that they run on every
  backend.
- `build/check-db-scripts.ps1` now executes every probe against a freshly created database, with a negative
  control, and DBUpdater's message gives the backend's own words.

**Schema:** 6030, unchanged.

**Upgrading:** if the 6.2.25 installer reported a failed upgrade, the database is already at 6030 with its
foreign keys, and this installer finds nothing left to do.

Full notes: [the 6.2.26 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.26).

## [6.2.25](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.25) - 2026-09-06

The release to take if you use the built-in ACME client, and sixty commits of new work besides.

**The fix to upgrade for**

- ACME issuance and renewal ended the process (GitHub issue 93): two calls handed the OpenSSL DLL a C runtime
  `FILE*`, and OpenSSL answered the missing `OPENSSL_Applink` by terminating the process. Both calls use
  OpenSSL's own file I/O, the certificate is deployed before the DANE TLSA line is computed, and a certificate
  6.2.24 issued before it died is deployed at the first ACME check.
- Two restarts of the servers at once could end in an access violation; restarts run one after the other.

**Mail flow**

- External accounts over IMAP (`FetchAccount.ServerType` 1), the INBOX only.
- Message retention per domain and per account (schema 6027); an archive index with legal hold (schema 6029).
- Outbound delivery pipelines the envelope and sends the message as one BDAT chunk when the remote advertises
  PIPELINING and CHUNKING (`OutboundPipelining`, `OutboundChunking`, both on).
- Delivery hard links (`DeliveryHardLinks`, off); `SEARCH BODY` and `TEXT` read text attachments.
- A Message-ID is added only for submissions (upstream PR 552); a stranded fetch account is released
  (upstream PR 603).

**IMAP**

- Every session numbers its selected folder for itself (upstream PR 602); sessions stop sharing unguarded state
  (upstream PRs 580 and 566); default special-use folders at account creation (schema 6026, off); one
  authorisation choke point for folder access.

**Security and authentication**

- SASL EXTERNAL with client certificates; the AUTH line limit raised to 12,288 octets.
- OAuth2 keys from the provider's JWK Set and token introspection; scrypt (`PreferredHashAlgorithm=7`); the
  password-hash work factor as settings (upstream PR 554); tarpits on failed logons and stranger recipients.
- The administrator's second factor is enforced by the server; `SmtpAuthenticatedSenderCheck` and
  `ScriptAllowedObjects`; the Apple configuration profile served over HTTPS only.

**Administration and databases**

- The REST API reads what was COM-only; a week of metric history (schema 6028); every backup proves its mail
  can be restored (`BackupVerifyRestore`, on); Control Panel accessibility for High Contrast, mnemonics and
  screen readers.
- TLS to PostgreSQL and MS SQL as `[Database]` settings; a plaintext MySQL only by choice (upstream PR 559);
  seventeen foreign keys (schema 6030).
- OpenSSL 4.0.2 and Boost 1.92.0; twelve upstream ports in all.

**Schema:** 6025 to 6030 (6026 the special-use folders switch, 6027 retention, 6028 metric history, 6029 the
archive index, 6030 seventeen foreign keys, which first removes orphaned rows).

**Upgrading**

- The Apple `.mobileconfig` profile is served over HTTPS only; a Message-ID is no longer added to relayed mail;
  the four HTTPS clients require TLS 1.2 or later; AUTH lines may be 12,288 octets; IMAP sequence numbers are
  stable within a session; `Account.DeleteMessages` keeps a designated special-use folder.
- Every new setting is off or inert except `OutboundPipelining`, `OutboundChunking` and `BackupVerifyRestore`.
- COM clients compiled against the 6.2.24 type library keep working.

Full notes: [the 6.2.25 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.25).

## [6.2.24](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.24) - 2026-09-04

The first stable release since 6.2.21: the code of 6.2.23 Alpha 2, cut the same day, plus NuGet lock files for the
.NET tools and the CI changes that hold every restore to them. It brings everything the 6.2.22 and 6.2.23
pre-releases introduced to a 6.2.21 installation at once.

**Since 6.2.21, in short**

- Mail that was silently lost is not any more: a delegated `APPEND`, `COPY` or `MOVE`, Thunderbird's Sent copies
  with `UTF8=ACCEPT` (GitHub issue 53), a kept-open log never flushed (GitHub issue 33), and an ACME client that
  had never worked against real Let's Encrypt (GitHub issue 34).
- Anti-spam bypasses closed: a forged header steering a per-account override, a HELO literal choosing the
  address the IP tests ran against, and a transaction opened without a greeting.
- Shared and delegated mailboxes, full-text search, RFC 3464 bounces, BINARYMIME, per-account spam thresholds, a
  sender blacklist, domain-wide out-of-office, list moderation, the Windows Event Log, OTLP metrics and logs, the
  PROXY protocol, an HTTP filter hook, eleven IMAP extensions, Microsoft 365 XOAUTH2, app passwords, TOTP, a
  quarantine, password history, message trace, per-domain relay and a disk-space floor.
- Control Flow Guard, a reproducible build, signed release tags, SBOMs and Sigstore bundles.

**Schema:** 6025, unchanged since Alpha 1.

**Upgrading from 6.2.21**

- The schema moves from 6011 to 6025 in fourteen steps, one way. The 6024 to 6025 step rewrites
  `hm_messages.messageflags` on MS SQL, SQL Server Compact and MySQL, which can take long on a large database;
  PostgreSQL is unaffected.
- Without a switch: `MAIL FROM` or `AUTH` straight after STARTTLS without a fresh `EHLO` gets `503`; relayed and
  fetched mail is tested against the address the relay observed; mail is refused with a temporary error below
  100 MB free (`MinimumFreeDiskSpaceMB`); serious errors also go to the Windows Event Log; existing out-of-office
  replies honour the RFC 3834 suppressions; old `hm_imapexpunged` rows are pruned once at first start.
- Early-bound COM clients compiled against a 6.2.22 pre-release interop assembly must be recompiled; late-bound
  scripts and anything compiled against 6.2.21 are unaffected.

**Known limitations:** statement timeouts have been exercised only on SQL Server Compact; the weekly C++ CodeQL
analysis was not running; SCRAM-SHA-1 is not offered (GitHub issue 49).

Full notes: [the 6.2.24 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.24).

## [6.2.23-alpha2](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.23-alpha2) - 2026-09-04 - pre-release

A small, deliberate pre-release: one reported defect fixed, seven fixes taken from the original project, which had
become active again, and hardening that had been written down as missing.

**Fixed**

- Thunderbird's Sent copies were refused once `UTF8=ACCEPT` was enabled (GitHub issue 53): the RFC 6855
  `UTF8 (~{n}` wrapper on `APPEND` is now recognised.
- From upstream: the IP-based spam tests of relayed and fetched mail ran against the HELO literal the client
  chose rather than the address the receiving server observed; they use the observed address now.
- From upstream: `RSET` before `EHLO`, or STARTTLS without a fresh `EHLO`, opened a transaction with no greeting.
  A `MAIL FROM` straight after the TLS handshake now gets `503 Bad sequence of commands`, as RFC 3207 requires.
- A reverse-DNS answer with a trailing dot no longer empties the PTR result; null dereferences in the script host
  and the IMAP notification client.

**Hardening**

- Control Flow Guard on `hMailServer.exe` (GitHub issue 45); the first SSH-signed release tag (GitHub issue 46);
  an unattended install whose database step fails now fails; `IMAPFolder.Delete` reports a refused deletion.

**Repository**

- `hmailserver/docs/RegressionEnvironment.md` (GitHub issue 48), a DCO check on pull requests (GitHub issue 47),
  the C# tree `dotnet format` clean (GitHub issue 44), and SCRAM-SHA-1 closed as not planned (GitHub issue 49).

**Schema:** 6025, unchanged. Everything in Alpha 1's notes still applies to an installation coming from 6.2.21.

Full notes: [the 6.2.23 Alpha 2 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.23-alpha2).

## [6.2.23-alpha1](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.23-alpha1) - 2026-08-21 - pre-release

A large feature release cut as an alpha, not for production: 83 commits since 6.2.22-pre6. It also fixes one path
that was silently losing mail.

**New**

- Shared and delegated IMAP mailboxes under `#Users.owner@domain.folder`, reachable when the owner's folder
  carries an RFC 4314 ACL granting the caller rights.
- A full-text search index that narrows `SEARCH BODY` and `TEXT` without changing results
  (`IndexerFullText=1` with message indexing, both off).
- RFC 3464 machine-readable bounces; BINARYMIME (RFC 3030), with relay of a binary message refused rather than
  converted.
- PROXY protocol v1 and v2 and Postfix `XCLIENT`, trusted only from listed peers; an external HTTP filtering hook
  (`FilterHookUrl`) for an engine such as rspamd.
- A sender blacklist; per-account spam thresholds; a domain-wide out-of-office reply; distribution-list
  moderation and a per-list bounce address.
- Windows Event Log integration; OTLP metrics and logs; W3C trace context ingestion; IPv6 on the REST, metrics
  and web-services listeners; five REST routes and `GET /api/v1/openapi.json`; DMARC aggregate reports in the
  RFC 9990 form behind `DmarcRptSchemaVersion=2`.
- Two COM methods for data-subject requests, `Account.ExportMessages` and `Utilities.EraseAddressTraces`, and
  `docs/WarmStandby.md`.

**Fixed**

- Local delivery could lose a message silently when the account-level copy could not be written; the sender is
  now told.
- Two database backends could wait forever (`DatabaseStatementTimeout`); an unattended upgrade could hang at an
  invisible password dialog; the white-list cache never cached.

**Schema:** 6022 to 6025 (6023 the full-text index tables, 6024 the domain out-of-office columns and
`hm_blocked_senders`, 6025 `hm_messages.messageflags` widened, per-account spam settings and list moderation).

**Upgrading**

- From 6.2.21 the schema moves from 6011 to 6025 in fourteen steps. The upgrade is one-way; take a database and
  data-directory backup first. The 6024 to 6025 step rewrites the largest table.
- New defaults: mail refused below 100 MB free, errors written to the Windows Application log, statement timeouts
  of 30 seconds on PostgreSQL and MySQL, QRESYNC tombstones pruned to 5,000 per mailbox, existing out-of-office
  replies suppressed for lists and automatic mail, bounces as `multipart/report`, and `PreferredHashAlgorithm`
  below SHA-256 refused.
- With IMAP ACL enforcement on, which is the default, any account holder whose client speaks `SETACL` can
  grant another local account access to their own folders, with no administrator step.
- A COM vtable layout was repaired to match 6.2.21; clients early-bound against a 6.2.22 pre-release must be
  recompiled, and a hand-copied install must re-register with elevation.

Full notes: [the 6.2.23 Alpha 1 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.23-alpha1).

## [6.2.22-pre6](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre6) - 2026-08-20 - pre-release

A pre-alpha. The same build as pre5 (build 30, commit `bf0d38a`), republished with its SBOMs and a cosign bundle
for every asset, which pre5 could not carry. Its theme is standards catching up with reality.

**DMARC and algorithms**

- DMARCbis: the DNS tree walk (RFC 9989 section 4.10) decides organisational domains, with the Public Suffix List
  as the `DmarcTreeWalkEnabled=0` escape hatch; the `np=` tag for subdomains that do not exist; aggregate reports
  name the DKIM selector.
- DKIM `rsa-sha1` is refused and a domain configured to sign with it signs `rsa-sha256` (`DkimAcceptSha1=1`
  restores both); keys below 1,024 bits are refused; SPF void lookups are capped at two (`SpfVoidLookupLimit`).

**Certificates**

- The ACME renewal window is two thirds of the certificate's lifetime, ready for shorter lifetimes, and the CA's
  ARI window (RFC 9773) is consulted.

**Mail flow and operations**

- A full mailbox is refused at `RCPT` with `452 4.2.2` (`RejectFullMailboxAtRcpt=0` restores the old way); quota
  warnings at `QuotaWarningPercent` (90) (schema 6022).
- A relay host per sending domain (schema 6021; GitHub discussion 31).
- A queryable message trace (schema 6020, off); archive retention (`ArchiveRetentionDays`, 0); per-domain metric
  labels (`MetricsPerDomainEnabled`, off).

**Schema:** 6019 to 6022.

**Upgrading:** from pre3, DBUpdater moves the schema from 6019 to 6022. Not for a production server.

Full notes: [the 6.2.22-pre6 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre6).

## [6.2.22-pre5](https://gitlab.com/Progressiverobot/hmailserver/-/tags/v6.2.22-pre5) - 2026-08-20 (the tag's date) - tag only

Superseded by 6.2.22-pre6, which is the same commit: pre5 went out carrying only the installer, its release was
deleted, and the tag remains.

## [6.2.22-pre4](https://gitlab.com/Progressiverobot/hmailserver/-/tags/v6.2.22-pre4) - 2026-08-20 (the tag's date) - tag only

Withdrawn: its installer could not create its own database on SQL Server Compact. The release was deleted and
the tag remains; do not build from it. The 6.2.22-pre6 notes explain both.

## [6.2.22-pre3](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre3) - 2026-08-19 - pre-release

The third pre-release of 6.2.22, whose theme is authentication: per-account two-factor authentication, and the
chain it needed first.

**Authentication**

- App passwords (schema 6016): a per-account credential, revocable on its own, for IMAP, POP3 and SMTP.
- Per-account TOTP (schema 6017): once enrolled, the account password stops being a mailbox credential and an
  app password becomes the only one.
- A password policy the server enforces where a password is chosen; password expiry and history (schema 6019).
  The server had no self-service password change then, so an expired password was reset by an administrator.
- ES256 OAuth2 tokens verify.

**Anti-spam, IMAP and POP3**

- An administrator-reviewable quarantine (schema 6018), off by default.
- `FETCH BINARY` on a composite section, a known limitation of pre2, is fixed; POP3 `EXPIRE` and `LOGIN-DELAY`
  (RFC 2449).

**Schema:** 6015 to 6019. **Upgrading:** from pre2, DBUpdater runs the four steps.

Full notes: [the 6.2.22-pre3 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre3).

## [6.2.22-pre2](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre2) - 2026-08-18 - pre-release

The second pre-release of 6.2.22: the server now sends the reports it has always asked others for.

**Reporting**

- DMARC aggregate reports (RFC 7489 `rua=`), inert until `DmarcRptFromAddress` is set, with the external
  destination check enforced; no forensic reports.
- TLS-RPT reporting made verifiable with `Utilities.SendTlsRptReports`.

**Authentication and anti-virus**

- Per-account lockout (`AccountLockoutThreshold`, off), which a distributed attack cannot spread around.
- POP3 `AUTH-RESP-CODE` (RFC 3206), `PIPELINING` and `IMPLEMENTATION` (RFC 2449).
- `AVFailAction`: a message that could not be scanned may be held and returned instead of delivered.

**IMAP**

- `BINARY` (RFC 3516) and `OBJECTID` (RFC 8474, schema 6015).

**Schema:** 6014 to 6015. **Known limitation:** `FETCH BINARY` on a composite section returned an empty literal
(fixed in pre3).

Full notes: [the 6.2.22-pre2 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre2).

## [6.2.22-pre1](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre1) - 2026-08-16 - pre-release

The first pre-release of 6.2.22: a Sieve wave, the Microsoft 365 OAuth switch-over, IMAP extensions, and two
reported bugs fixed. (The notes speak of two schema steps; from 6.2.21 there are three, 6012 to 6014.)

**Fixed**

- The built-in ACME client had never worked against a real Let's Encrypt (GitHub issue 34): the challenge
  locator missed the pretty-printed JSON. Failure messages now name the domain and quote the CA's response.
- With *Keep log files open*, log lines could stay invisible for forty minutes (GitHub issue 33); every line is
  now handed to the operating system as it is written.

**New**

- Microsoft 365 XOAUTH2 for outbound relay and POP3 collection, ahead of Basic authentication being switched
  off for SMTP relay.
- Sieve: variables, editheader, duplicate, reject and ereject, include, enotify, date and currentdate, spamtest,
  ihave and environment, mailboxexists and `:regex`; ManageSieve response codes and `RENAMESCRIPT`.
- Ten IMAP extensions - APPENDLIMIT, LITERAL-, LIST-STATUS, UNAUTHENTICATE, QUOTA, PREVIEW, MULTIAPPEND,
  REPLACE, SAVEDATE (schema 6013) and METADATA (schema 6014) - and SIZE on the SMTP client, which now opens every
  delivery with `EHLO`.
- An auto-reply start date and rule criteria up to 2,000 characters (schema 6012).

**Schema:** 6011 to 6014. **Upgrading:** databases upgrade automatically; take a backup before installing a
pre-release.

Full notes: [the 6.2.22-pre1 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre1).

## [6.2.21](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.21) - 2026-08-15

The fix for inbound mail from a Postfix relay that could hang for ever, found by running a real Postfix.

**SMTP**

- GitHub discussion 18: Postfix sends the end of the body, the terminating dot and `QUIT` in one segment, and
  every end-of-data check had looked only at the end of the buffer. The standard terminator is now found
  anywhere, and what follows it is handed to the command parser; bare-LF terminators are still recognised only
  at the very end, the CVE-2023-51764 smuggling rule. The Postfix test rig is in `hmailserver/test/PostfixBench`.
- Dot transparency across buffer boundaries, in both directions, which could silently drop a dot or truncate a
  message.
- A malformed DNS answer is retried once over TCP.

**Sieve**

- Sieve scripts move with domain and account renames and are removed with deletes, so a recreated address no
  longer inherits the previous holder's filter; the Control Panel writes a script only after the save succeeds.
- The `body` test (RFC 5173) with `:text`, `:content` and `:raw`.

**IMAP**

- `SORT` with an empty criteria list spun a connection thread for ever; it is rejected. The message cache could
  never evict and only counted upwards; fixed.

**Control Panel**

- The MX query tool asks the server's own resolver through `Utilities.ResolveMXRecords` (GitHub issue 29).
- Seven things the retired Administrator could do are restored, among them pausing the server and renaming a
  domain or an account; the Welcome page's icons are centred (GitHub issue 30, reported by `@helptiger`).

**Schema:** 6011, unchanged.

**Known issues:** equal-preference MX records are not randomised; one-click unsubscribe (RFC 8058) is not
implemented.

Full notes: [the 6.2.21 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.21).

## [6.2.20](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.20) - 2026-08-15

The fix for fresh installations of 6.2.19, which failed or hung. Upgrades of existing installations were never
affected.

**Fixed**

- Database creation failed: statements added to the create scripts were separated by a single newline, and SQL
  Server Compact executes one statement per command. The create scripts of all backends are fixed.
- A silent install hung on a hidden dialog; every installer dialog now takes its default button under
  `/SUPPRESSMSGBOXES`, and the installer exits with a code a script can read.

**New**

- IMAP `THREAD` (RFC 5256) with `ORDEREDSUBJECT` and `REFERENCES`.

**Schema:** 6011, unchanged.

Full notes: [the 6.2.20 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.20).

## [6.2.19](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.19) - 2026-08-15

**Fresh installations of 6.2.19 fail; use 6.2.20.** Upgrades are unaffected. The release fixes the custom DNS
server that 6.2.16 to 6.2.18 broke, and adds directory provisioning.

**Fixed**

- With a custom DNS server configured, 6.2.16, 6.2.17 and 6.2.18 could resolve nothing through it (GitHub issue
  25): MX, DNSBL, SPF and SURBL lookups all failed. Pinned by a regression test.
- A UID could be handed out twice; a colon in an account address is refused; a session could outlive its
  `io_context`; a status card could report failing backups on a healthy server; a certificate handle leaked.

**New**

- An LDAP directory as an account source (`Settings.PreviewDirectorySync`, `ApplyDirectorySync`), with a Control
  Panel page: opt-in per domain, and nothing is ever deleted.
- Sieve `imap4flags` now reaches the stored message.
- The `[Settings]` values move into the database so they can be administered remotely, with the file still
  winning where both carry a value.

**Schema:** 6005 to 6011.

**Known issue:** GitHub issue 26, a partial `FETCH BODY[]<offset.length>`, does not reproduce here.

Full notes: [the 6.2.19 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.19).

## [6.2.18](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.18) - 2026-08-12

Finishes on the delivery side what 6.2.17 began, and stops a database outage bouncing mail.

**Fixed**

- A recipient lookup that fails because the database did not answer now returns `451` rather than `550`, so a
  database briefly locked by a backup defers mail instead of bouncing it. `DBConnectionAcquireTimeout` now
  defaults to 60 seconds.
- Bounded waits (GitHub issues 23 and 24): an absolute ceiling on outbound sessions (`ClientSessionCeiling`, 30
  minutes); ClamAV on the delivery path; `DNSQueryTimeout` (10 s), `ScriptTimeout` (60 s) and
  `ExternalProcessTimeout` (300 s); pre-authentication IMAP buffering capped at 11 MB; a backup restore validates
  the archive before deleting anything.
- The first `/analyze` pass: buffers sized in bytes where characters were counted, an overrun on long paths,
  and a log rotation that tested an uninitialised value.

**Documentation**

- `Roadmap.md` rewritten after a comparison with Postfix and Dovecot, Stalwart, iRedMail, Mail-in-a-Box and
  Exchange Online.

**Schema:** 6005, unchanged.

**Correction (12 August 2026):** ARC sealing is narrower than the notes first said: relayed third-party mail is
never sealed.

Full notes: [the 6.2.18 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.18).

## [6.2.17](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.17) - 2026-08-12

Reproduces and bounds a stall after `354` on mail from a trusted incoming relay, with the stall now logged by
stage. (6.2.21 later found the defect GitHub discussion 18's reporter actually had.)

**Fixed**

- SpamAssassin's wait is bounded (`SAMaxTimeout` plus 30 seconds), after which the message is accepted without a
  verdict.
- `FinalizationTimeout` (240 seconds, 0 disables it) answers `451 4.3.1` rather than leaving the sender waiting.
- Acceptance is timed per stage and each spam test individually, and a slow stage is logged.

**Also**

- The README rewritten around what the server does; per-version history moved to the release pages.
- Regression coverage for clients that vanish mid-operation, and an installer smoke test on a clean machine.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.17 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.17).

## [6.2.16](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.16) - 2026-08-11

A patch over 6.2.15 fixing one reported Control Panel defect. No server change.

**Fixed**

- Dismissing the Ctrl+K settings palette raised an error dialog (GitHub issue 21), on nearly every search in
  6.2.14 and 6.2.15.

**Schema:** 6005, unchanged. **Not fixed:** GitHub discussion 18, tracked in GitHub issue 20.

Full notes: [the 6.2.16 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.16).

## [6.2.15](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.15) - 2026-08-11

IMAP sequence sets behave as RFC 3501 defines them, and every file the server writes has an owner.

**IMAP**

- `*` is resolved on either side of a colon and descending ranges are normalised in all four sequence-set
  parsers. Before, `UID STORE *:* +FLAGS (\Deleted)` flagged every message and `UID EXPUNGE *` deleted every
  deleted message.
- `STATUS ... (RECENT)` no longer reports the selected folder's count for every folder.

**Data safety**

- A failed restore could empty the data directory; the restore now confirms the replacement store exists before
  deleting anything.
- An unreadable message file no longer hangs an outbound connection; several paths no longer leave orphaned
  files; the account cache has a size limit.

**From the forum**

- `BOOST_USE_WINAPI_VERSION` raised to Windows 10; upstream's end-of-input rule adopted, with a truncated-`BDAT`
  defect it exposed fixed before release; DKIM header-name case under `simple` canonicalisation (upstream PR 530).

**Also**

- ManageSieve limits authentication attempts; the SMTP `DATA` path gains debug logging.
- Control Panel pages for settings that had none: Authentication, Administrative access, DNS resolver, Web
  services and autoconfiguration, message archiving, and the consistency scan's findings in Diagnostics.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.15 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.15).

## [6.2.14](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.14) - 2026-08-11

Two community reports fixed, 21 defects found by an adversarial audit of the server fixed, and settings you can
search for.

**Reported**

- Backup would not start from the Control Panel (GitHub issue 19); the log-retention setting existed but could
  not be found (GitHub issue 16).

**Mail loss, corruption and crashes**

- A failed message copy crashed delivery; IMAP `APPEND` reported success for messages never written; string SQL
  parameters on MS SQL and SQL CE were bound from freed memory; a failed transaction start leaked a pooled
  connection; IMAP `CLOSE` expunged without telling other sessions; a refused POP3 session released another's
  lock.

**Security**

- DKIM test mode (`t=y`) no longer turns a failed signature into a pass; IMAP SASL credentials are no longer
  logged; every DKIM key record at a selector is examined; MTA-STS and MX failover hold for large recipient sets;
  REST requests are time-bounded; `ES256` tokens are refused with a clear message.

**Finding settings**

- Ctrl+K searches all 227 settings by label and key; settings moved to the pages where they are used; "Advanced
  hardening" became "Advanced INI settings".

**Schema:** 6005, unchanged.

Full notes: [the 6.2.14 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.14).

## [6.2.13](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.13) - 2026-08-11

A community-feedback bug-fix release, hardening the SMTP, IMAP and SpamAssassin paths against malformed input.

**Fixed**

- A `DATA` stall when relaying from Postfix or Proxmox Mail Gateway, caused by a reverse-DNS lookup on the network
  I/O thread; `BDAT` refusals, pipelined data and a wedged connection handled.
- A malformed IMAP partial-fetch range could crash the server or read heap memory; `OnClientLogon` fires from
  every `AUTHENTICATE` mechanism.
- A malformed or empty `spamd` response could hang a session, corrupt headers or lose the message.
- DBUpdater labels versions 6002 to 6005; a failed database step reaches the installer as a visible error; a
  customised `EventHandlers.vbs` survives reinstall; Control Panel diagnostics read the right properties.

**Schema:** 6005, unchanged. **Upgrading:** in place over any earlier release; the minimum OS is Windows 10 1607
or Windows Server 2016, 64-bit.

Full notes: [the 6.2.13 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.13).

## [6.2.12](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.12) - 2026-08-10

Every C# component in the product now targets .NET 8. No server-core changes.

**Changed**

- DBSetup, DBSetupQuick, DBUpdater, the Data Directory Synchronizer and the Shared library move from .NET
  Framework 4.8.1 to .NET 8, building against a checked-in COM interop wrapper.
- The VB6 migration wizards are replaced by an Import Tool: accounts from a text file, and messages from mbox files
  through the COM API, with the old tool's documented defects fixed.
- The .NET 8 Desktop Runtime is a server prerequisite and the supported floor is Windows 10 1607.
- CI builds every C# project; the regression suite runs on NUnit 4.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.12 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.12).

## [6.2.11](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.11) - 2026-08-07

A Control Panel accessibility fix, and the regression suite run in full for the first time. No server-core
changes.

**Fixed**

- The alias list on the Domains page and the Live logs list announced their CLR type name to screen readers
  instead of their content.
- Run on the release machine for the first time, the ClamAV and SpamAssassin tests failed twice, both times on
  a defect in the test: the EICAR test now sends a real attachment, and the `spamd` stop is awaited.
- The README documents a working SpamAssassin setup on Windows and the suite's need for elevation.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.11 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.11).

## [6.2.10](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.10) - 2026-08-07 (the tag's date)

A security and housekeeping release: an authorisation defect in the COM API, two unmaintained administration
front-ends removed, and the code-quality findings taken to zero.

**Security**

- Fifteen COM methods rejected an unauthorised caller with `return false`, which is `S_OK`, so each reported
  success for a call it had refused, and five handed back uninitialised memory. They return access denied now.
  Found by running CodeQL's C++ suite by hand.
- The PHP WebAdmin, which kept the administrator password in plaintext in a PHP session, is removed.

**Removals**

- The retired Administrator (`hMailAdmin.exe`) is gone from the repository; "Administrative tools" is renamed
  "Remote administration support (registers the COM API for scripts)", which is what it does.

**Quality**

- Code-quality findings in shipped, hand-written code from 958 to 0, with real defects fixed on the way.

**Schema:** 6005, unchanged.

**Upgrading:** a drop-in over 6.2.9. Anyone who ticked "Administrative tools" for the WebAdmin or the
Administrator should use the Control Panel, which connects to a remote server directly.

Full notes: [the 6.2.10 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.10).

## [6.2.9](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.9) - 2026-08-07 (the tag's date)

A follow-up to 6.2.8. No server-core changes.

**Fixed**

- LiveCharts 2.0.5 painted the dashboard charts as a white block in the dark theme (GitHub issue 11); the chart
  background is now set explicitly.

**Quality and repository**

- The Control Panel has automated tests for the first time (GitHub PR 12): `ControlPanel.Core`,
  `ControlPanel.Tests` and `ControlPanel.sln`.
- Release tags protected, secret scanning on, a Code of Conduct added.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.9 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.9).

## [6.2.8](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.8) - 2026-08-07 (the tag's date)

A Control Panel bug-fix release. Both defects made a healthy server look broken. No server-core changes.

**Fixed**

- Every data-driven list pane showed blank rows (GitHub issue 6), from domain aliases to account rules.
- The Control Panel died after a service restart with "The RPC server is unavailable" (GitHub issue 7); it now
  reconnects on its own, waits for the server to report Running, and no longer starts a service the
  administrator stopped.

**Dependencies**

- WPF-UI 4.3.0 and QRCoder 1.8.0 in the Control Panel; LiveCharts held at 2.0.0-rc2.

**Schema:** 6005 at the tag (the published notes said 6001, which the tree at the tag does not bear out); no
database change.

Full notes: [the 6.2.8 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.8).

## [6.2.7](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.7) - 2026-06-15 (the tag's date)

A Control Panel usability release. No server-core changes since 6.2.6.

**Changed**

- A browse button on every field that holds a path.
- One-click DKIM: an RSA-2048 key pair generated with the exact DNS TXT record to publish.
- Password reveal, a generator for strong passwords, and the external POP3 password masked.
- A date picker for the auto-reply expiry, number boxes, Enter and Esc in every dialog, Copy on MX query and
  Diagnostics output, and a window that remembers its size and position.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.7 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.7).

## [6.2.6](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.6) - 2026-06-15 (the tag's date)

A Control Panel and installer polish release, with one new opt-in protocol mode.

**Changed**

- IMAP4rev2 (RFC 9051) as an opt-in session mode through `ENABLE IMAP4rev2`; IMAP4rev1 behaviour is unchanged
  until a client asks.
- A Control Panel visual redesign on a theme-aware colour-token system, and a Welcome page of quick actions.
- Every configurable server setting has a Control Panel control.
- Two-factor setup shows a scannable QR code.
- The installer's database page scales with DPI, and the dead legacy dependency installers are gone.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.6 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.6).

## [6.2.5](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.5) - 2026-06-15 (the tag's date)

A critical fix release: the default fresh install, SQL Server Compact with DPAPI-protected secrets, could not
connect to its own database. Anyone running a default install should upgrade.

**Fixed**

- The DPAPI-protected database password was truncated by a 255-character INI read buffer, giving SQL CE error
  25028; the buffer is now 4,096 characters.
- The create scripts stamped schema 6004 while the server required 6005, so a new install reported its database
  as too old.

**Control Panel**

- Full settings parity with the classic Administrator, with the Server Status page and the per-account rule
  editor.

**Schema:** 6005.

Full notes: [the 6.2.5 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.5).

## [6.2.4](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.4) - 2026-06-15 (the tag's date)

A Control Panel release that completes the last functional-parity item from the classic Administrator. The
server, the Control Panel and the installer all report 6.2.4.

**Changed**

- Active Directory account pickers: a read-only browser of the forest's domains and users, for an account's
  directory link and for distribution-list recipients.
- "Settings could not be read" now names the real cause.
- The MariaDB Connector/C client is staged into the build output, so MySQL and MariaDB back ends work out of the
  box.

**Schema:** 6005.

Full notes: [the 6.2.4 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.4).

## [6.2.3](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.3) - 2026-06-15 (the tag's date)

The first server-focused release since 6.2.2. Every new behaviour is additive and off by default. The installer
no longer ships the classic Administrator; the Control Panel is the only bundled GUI.

**Mail filtering**

- A Sieve (RFC 5228) interpreter run at local delivery, and a ManageSieve (RFC 5804) listener behind
  `ManageSieveServerPort`.

**Operability**

- `/livez`, `/readyz` and `/healthz` on the metrics listener; Prometheus metrics for the database, TLS, the queue,
  authentication, delivery outcomes and command latency.
- A slow-query log with secrets redacted (`SlowQueryLogMilliseconds`); a graceful shutdown drain
  (`ShutdownDrainSeconds`); `MessageStoreFsync`; a read-only store consistency check
  (`MessageStoreConsistencyCheck`); log retention (`LogDeleteDays`); the session id in the `Received` header;
  an active/passive runbook.

**SMTP**

- BATV backscatter protection (`BATVEnabled`, `BATVSecret`); CHUNKING and `BDAT` (RFC 3030) on receipt.

**Supply chain**

- SBOMs for every release, Dependabot and a dependency-review gate.

**Schema:** 6005.

Full notes: [the 6.2.3 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.3).

## [6.2.2](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.2) - 2026-06-14 (the tag's date)

The 6.2.2 server with a substantially upgraded Control Panel.

**Server**

- Full RFC 4013 SASLprep for non-ASCII SASL credentials; RS256 OAuth2 bearer tokens covered by regression tests.

**Control Panel**

- Search on the long list pages, confirmation before deletes, numeric validation, the OS theme followed by default,
  and the Advanced group split into Security, Network and Maintenance.
- Anti-virus test buttons and scanner presets, event-script templates, and an "Advanced hardening" page for
  settings that had no control.
- Screen-reader names and stable automation ids throughout.

**Schema:** 6005.

Full notes: [the 6.2.2 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.2).

## [6.2.1](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.1) - 2026-06-14 (the tag's date)

Deliverability and modern SMTP standards. Every new feature is off or backward compatible.

**New**

- `PIPELINING` (RFC 2920), `SMTPUTF8` (RFC 6531 and 6532), `ENHANCEDSTATUSCODES` (RFC 2034) and DSN (RFC 3461 and
  3464).
- SRS for forwarded mail (`SRSEnabled`, `SRSSecret`); per-IP and per-destination rate shaping
  (`MaxSubmissionsPerIPPerMinute`, `MaxOutboundPerDestinationPerMinute`).

**Fixed**

- A trailing NUL from `Unicode::WideToMultiByte` could corrupt DPAPI-protected stored secrets.

**Schema:** 6004 to 6005.

Full notes: [the 6.2.1 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.1).

## [6.2.0](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.0) - 2026-06-14 (the tag's date)

The hMailServer Control Panel arrives: a new .NET 8 WPF administration app beside the full 6.x server. The
.NET 8 Desktop Runtime is bundled and installed when missing.

**New**

- The Control Panel: a live dashboard, domains and accounts, the delivery queue with live logs, a graphical rules
  editor, every settings area including DANE, MTA-STS, ARC, TLS-RPT, ACME and the REST API, and the utilities.
- The classic Administrator and the Web Control Deck remain included.

**Schema:** 6004.

Full notes: [the 6.2.0 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.0).

## [6.1.0](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.1.0) - 2026-06-12 (the tag's date)

A UI and UX modernisation across the desktop Administrator, a new web interface and the installer. The server core
is unchanged from 6.0.0 build 4 apart from the new web-page endpoint.

**New**

- The Web Control Deck, served by the REST API listener (`RestApiPort`): a live dashboard, domain and account
  management, queue retry and delete, and DANE TLSA records to publish.
- The Administrator gains light and dark themes, a Ctrl+K command palette and a modernised dashboard.
- The installer's wizard is rebranded and ships the WebAdmin payload.

**Schema:** 6001.

Full notes: [the 6.1.0 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.1.0).

## [6.0.0-B4](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.0.0-B4) - 2026-06-12 (the tag's date) - pre-release

Build 4, which fixes the service crash at startup that build 3 caused on fresh installations.

**Fixed**

- Build 3 bundled VS2019-era VC++ runtime DLLs, which the service loaded in place of the current ones; the
  installer now ships `Microsoft.VC145.CRT` 14.51 and no longer bundles the obsolete UCRT.
- A stdout and stderr pipe deadlock in `build/run-tests.ps1`.

**Schema:** 6001.

Full notes: [the 6.0.0-B4 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.0.0-B4).

## [6.0.0-B3](https://gitlab.com/Progressiverobot/hmailserver/-/tags/v6.0.0-B3) - 2026-06-12 (the tag's date) - tag only

Build 3 exists as a tag only; nothing was saved of a release for it, and there is no evidence one was published.

## [6.0.0](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.0.0) - 2026-06-12 (the tag's date)

The production release of hMailServer 6.0, 64-bit only. **The `v6.0.0` tag points at build 2 (`e4b716318`), while
the published release shipped the build-4 installer, `hMailServer-6.0.0-B4-x64.exe`, and its notes describe build
4.**

**Fixed in builds 3 and 4**

- Build 4: the service no longer crashes at startup on fresh installations from mismatched VC++ runtime DLLs.
- Build 3: MySQL and MariaDB database creation, and `libmysql.dll` bundled.

**Highlights of 6.0**

- OpenSSL 4 with TLS 1.2 and 1.3, MTA-STS, DANE with in-process DNSSEC validation, ARC sealing, Ed25519 DKIM, ACME
  (Let's Encrypt) built in, the REST admin API, Prometheus metrics, IMAP `MOVE`, `ID` and `SPECIAL-USE`, PBKDF2
  password hashing, and the Visual Studio 2026 toolchain.

**Schema:** 6001 (DBUpdater upgrades a 5.7 database).

Full notes: [the 6.0.0 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.0.0).

## [6.0.0-B1](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.0.0-B1) - 2026-06-12 (approximately, when its installer was uploaded) - pre-release

The first regression-validated build of the modernised 6.0.

**New in 6.0**

- MTA-STS and DANE with DNSSEC for outbound TLS, ARC sealing, TLS-RPT reporting, ACME with automatic apply, the
  REST admin API, MTA-STS, autoconfig and autodiscover hosting, Prometheus metrics, JSON logging, PBKDF2 password
  hashing with transparent rehash, Ed25519 DKIM, DMARC, IMAP `MOVE`, `ID` and `SPECIAL-USE`, 8BITMIME, an
  Administrator dashboard with TOTP two-factor sign-in, and TLS 1.2 and 1.3 defaults.
- Built with Visual Studio 2026 (v145), OpenSSL 4.0.1, Boost 1.91, PostgreSQL 18.3 libpq and .NET Framework 4.8.1.

**Fixed**

- A latent `CStdStr::FormatV` buffer-count bug that crashed the service.

**Schema:** 6001 (upgrade from 5.7 through DBUpdater).

**Licence:** AGPLv3, based on hMailServer by Martin Knafve.

Full notes: [the 6.0.0-B1 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.0.0-B1).
