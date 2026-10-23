Third-party binaries
====================

This repository commits no executable binary. Until 11 September 2026 it held
forty - DLLs, two MSIs, an EXE and two type libraries - and a committed binary
is a file nobody can review: it arrives in a change as `Binary files differ`,
and from then on the only thing anyone knows about it is its name. Since that
day the build obtains the ones it still needs when it runs, and five that
nothing needed were deleted.

This document is the record of each of those build inputs: what it is, where it
comes from, how the build gets it and checks it, and why the build needs it. The
machine-readable half is [third-party-binaries.json](third-party-binaries.json),
and `build/get-installer-binaries.ps1` is the script that reads it and puts the
files in place.

The division of labour between the two files is deliberate. The JSON holds the
SHA-256 hashes and is the only place they appear; this document explains and
never repeats them. Two copies of the hashes would diverge, and the copy people
read would be the wrong one.

The short version
-----------------

| | |
|---|---|
| Executable binaries committed to the repository | **0** |
| Fetched from the project's `build-inputs-<n>` releases or the publisher, checked against a SHA-256 | 18 |
| Gathered from the build machine's Visual Studio, checked by version | 10 |
| Copied from the build machine's own Windows | 2 |
| Generated from this repository's own IDL | 1 |
| Deleted on 11 September 2026, because nothing loaded them | 5 |

The thirty-one are the manifest's entries for the x64 build, apart from the .NET
runtime the installer downloads (see "Moving the .NET runtime pin") and the ARM64
installer's own (WindowsOnArm.md); every one is marked as not in git,
and git ignores every path they are placed at. What the tree still carries is
binary only in the plain sense and is not code: the Fat Cow icon library under
`libraries/fat_cow_icons` (its 2,800 `.ico` images; the archive of PNGs they were
made from, as FatCow published it, left the tree on 23 October 2026, since nothing
read it, and is in the git history from `fe45f43c3`), the images beside these
documents, and the OpenPGP fixtures under `build/fixtures/openpgp`.
None has an executable's extension or a PE, ELF or OLE header, so none is a
binary to the check described below, and none is in the manifest.

Where the build gets them
-------------------------

`build/get-installer-binaries.ps1` handles four kinds, named by each entry's
`disposition`:

* **fetched** - downloaded from a `build-inputs-<n>` release and checked against
  the manifest's SHA-256 before it is placed. Two release assets: from
  `build-inputs-2`, an archive of the MariaDB client and its seven plugins (placed
  twice, see below), which `libraries/build-mariadb-connector-c.ps1` builds from
  MariaDB's source because MariaDB publishes no Windows build of Connector/C
  3.4.11; and from `build-inputs-1`, which holds the exact bytes that were
  committed before 11 September 2026, the SQL Server Compact 4.0 SP1 x64 runtime,
  which has no first-party download left. 7-Zip is fetched from 7-zip.org itself,
  over https with no redirect followed, since 25 September 2026.
* **gathered** - the MSVC runtime, copied from the build machine's Visual Studio
  (`%VCToolsRedistDir%\x64\Microsoft.VC145.CRT`) and checked by major and minor
  version against the toolset that built the server, since a runtime pinned to
  yesterday's bytes would be the wrong runtime after a Visual Studio update.
* **system** - the ADO 2.8 type libraries the server `#import`s, copied from the
  build machine's Windows. `-SystemOnly` places these two and nothing else, and a
  clean clone's server does not compile without them.
* **generated** - `Interop.hMailServer.dll`, made by
  `build/generate-com-wrapper.ps1`, which `build.ps1` and `build-tools.ps1` run
  themselves.

The script downloads a `build-inputs-<n>` release's files from the project's
generic package registry on GitLab
(`https://gitlab.com/api/v4/projects/86730559/packages/generic/build-inputs/<n>/<asset>`),
where `build-inputs-1` was re-published byte for byte after GitHub suspended the
project's account on 18 September 2026
(<https://gitlab.com/hmailserver/hmailserver/-/releases/build-inputs-1>), and
where `build-inputs-2` was published first
(<https://gitlab.com/hmailserver/hmailserver/-/releases/build-inputs-2>).
Until 26 September 2026 it downloaded from the suspended account's address on
GitHub, which has answered 404 since. A `build-inputs-<n>` release's files come
from GitLab alone - both releases are copied to GitHub with the others, and the
build never reads them there - by a rule unchanged since the project's return to GitHub on
18 October 2026 (<https://github.com/RainbowWizard-coder/hMailServer>, a second
home; GitLab stays canonical): no build step downloads from GitHub, so that no
forge's suspension can stop a build again, and `.no-github` keeps GitHub out of
every CI job (`.gitlab/ci/no-github.yml`). A file already in the script's cache
(`%LOCALAPPDATA%\hmailserver\build-inputs`, or the folder given to `-Cache`)
with the manifest's hash is used without downloading anything, and every
download is checked against the hash whatever its address.

The server, the tools, the Control Panel and the test assembly compile without
the fetched and gathered files. The installer and a regression bench need them.
`build/get-installer-binaries.ps1 -Verify` checks that every fetched, gathered
and system file is in place and matches, and changes nothing.

What the build already does right
---------------------------------

The three largest and most security-relevant native dependencies - OpenSSL,
Boost and PostgreSQL's libpq - have never been in this repository. They live
outside the tree under `%hMailServerLibs%`, and
[post-build.bat](../source/Server/hMailServer/post-build.bat) copies the built
DLLs into the output directory. Since 11 September 2026 every other binary the
build uses has followed the same model.

Two native dependencies are in the tree by design. zlib 1.3.2 (1.3.1 until 25 September 2026) is vendored as source
under `hmailserver/source/Server/zlib` (its own LICENSE beside it, the text also
under `docs/Licenses/`) and compiled straight into `hMailServer.exe` for IMAP
COMPRESS=DEFLATE - new in 6.2.28.
Source, not a binary, so it is outside this inventory; since 25 September 2026
`build/native-dependencies.ps1` lists it, reading the version from `zlib.h`'s
`ZLIB_VERSION`, so the SBOM names it.

QuickJS 2026-06-04, Fabrice Bellard's JavaScript engine (MIT), is vendored as source
under `hmailserver/source/Server/quickjs` - the engine and the four libraries it needs,
byte for byte as released, with its LICENSE and VERSION; the licence text is also under
`docs/Licenses/` and is installed by every Linux package as
`/usr/share/hmailserver/licenses/QuickJS.txt` - the .deb, the .rpm and the PKGBUILD
through the CMake install rule, the AppImage by its build script. It is the engine the event scripts run
in on the Linux builds ([EventScripts.md](EventScripts.md)), built by CMake alone: the
release has no MSVC support, so it is in no Visual Studio project and not in
`hMailServer.exe`. `quickjs-libc.c`, where QuickJS's `std` and `os` modules live, was
left out of the tree. It came from its author's site,
bellard.org/quickjs/quickjs-2026-06-04.tar.xz (sha256
`b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a`), and was checked
against Alpine Linux's package of the same release, whose APKBUILD (aports,
`community/quickjs`, commit `a7a96a01`) records the archive's SHA-512,
`e02722a82de0d2b7d6c95edc57daccecb5680b8d87f3580424a143a04df197c0df5bcd51d138c5e1e8b0d78b423acfa39169e8c0bfc1ace8387e13fc3bca22e2`: the same file. Source, not a
binary, so it is outside this inventory too; `build/native-dependencies.ps1` does list
it, reading the version from `quickjs/VERSION` (the one place the CMake build reads it
from as well), so the SBOM names it.

The three scripts that build OpenSSL, Boost and libpq -
`libraries/build-openssl.ps1`, `build-boost.ps1` and `build-pgsql.ps1` - download
too: each fetches one source archive from that library's own release server,
verifies it against a SHA-256 pinned in the script, and builds it outside the
tree. Nothing they fetch is committed.

Policy
------

**1. No binary is committed.** A file the build needs that is not source is
obtained at build time - fetched, gathered, copied from Windows or generated -
and listed in `third-party-binaries.json` with the SHA-256 where it has one, the
component and version, the publisher, the licence, where upstream it came from,
what uses it, and why the build needs it.

**2. Changing a build input means changing its manifest entry in the same
commit.** A fetched file's hash is what turns a new download into a reviewable
text diff. That text diff is the review.

**3. Prefer, in this order:**

1. Taking it from the toolchain, from Windows or from `%hMailServerLibs%` at
   build time, as the MSVC runtime, the ADO type libraries, OpenSSL, Boost and
   libpq are.
2. Fetching the vendor's *signed* original, so that the signature is the
   provenance rather than a hash we wrote down ourselves.
3. Fetching the file from the project's own release with a recorded hash and
   upstream - this document.

**4. A binary with no publisher, no version resource and no known upstream is
not acceptable.** The one there was, `ISC.dll`, was replaced on 11 September 2026
by the installer's own Pascal.

**5. Record what you verified, not what you assume.** Every version, publisher
and signature status in the manifest was read out of the files themselves.
Where an upstream URL could not be confirmed, the manifest says where to look
rather than inventing a link.

Authenticode: what is actually signed
-------------------------------------

Eleven of the thirty-one carry a valid Authenticode signature. Twenty do not.

| Signed by | Files |
|---|---|
| `CN=Microsoft Windows Software Compatibility Publisher` | 10 (the MSVC runtime) |
| `CN=Microsoft Corporation` | 1 (the SQL CE x64 MSI) |
| *unsigned* | 20 |

The sixteen MariaDB Connector/C DLLs (eight files, placed twice) are unsigned, as
MariaDB's own builds were, which is why the recorded hash matters most for exactly
those files: there is no other way to tell that the `libmysql.dll` the build placed
is the one `libraries/build-mariadb-connector-c.ps1` built from MariaDB's source -
and the build is reproducible, so anyone can build it again and compare.
The two `.tlb` files cannot carry an Authenticode signature at all - a type
library is not a signable image format - and they are Windows' own; the
generated interop wrapper is this project's and unsigned.

For the signed files a signature check is stronger than a hash pin and survives a
legitimate vendor patch bump. The gathered MSVC runtime is checked by version
rather than by hash, and its signer is recorded in the manifest; the SQL CE MSI is
checked by hash. Until 18 September 2026 a Windows job of the provenance workflow
also fetched the build inputs and checked that the MSI's signature was valid and
its signer the one recorded; from 6.3.6 the bench150 job `build-inputs-authenticode`
does so again, for every fetched file - see "What checks it" below.

The inventory
-------------

Grouped by component. Per-file hashes are in
[third-party-binaries.json](third-party-binaries.json).

### Fetched from a `build-inputs-<n>` release

| Component | Files | Note |
|---|---|---|
| MariaDB Connector/C 3.4.11 (`libmysql.dll` and seven plugins), from `build-inputs-2` | 16 | The MySQL/MariaDB client. `MySQLInterface.cpp` builds a path beside the running executable and loads `libmysql.dll` from there at runtime, together with the seven authentication plugins found through `MYSQL_PLUGIN_DIR`: `caching_sha2_password` and `sha256_password` (MySQL), `client_ed25519`, `parsec` and `auth_gssapi_client` (MariaDB), `dialog` and `mysql_clear_password`. Without the full plugin set, common MySQL 8 and MariaDB account types cannot authenticate. Placed twice from the same archive: under `libraries/mariadb-connector-c-3.4.11/`, which the post-build step copies beside the server, and under `installation/Extras/`, which the installer ships. The second copy is redundant - repointing `section_files_common.iss` at `libraries/` would remove it - and the manifest says so. Built from MariaDB's source since 3.4.11: see "Connector/C 3.4.11, built from MariaDB's source" below. |
| SQL Server Compact 4.0 SP1 x64 (`installation/SQLCE/SSCERuntime_x64-ENU.msi`) | 1 | The built-in database backend. Identity read from the MSI itself: "Microsoft SQL Server Compact 4.0 SP1 x64 ENU", Microsoft Corporation, built 2012-04-06, validly signed. **Microsoft has withdrawn the download**, so the release copy is the archive of record - there is no upstream left to fetch it from. |
| 7-Zip standalone console (`installation/Extras/7za.exe`) | 1 | 7-Zip standalone console **26.03** (3 September 2026), the `x64/7za.exe` of the 7-Zip Extra package, unsigned as 7-Zip's files always are. Run from `{app}\Bin` by `Compression::GetExecutableFullPath_()` to write, list and extract the backup's archives, as the service account. **Moved from 19.00 on 25 September 2026**, which the first security scan named its most serious finding: 19.00 (2019) predated the fixes for CVE-2023-31102 (a crafted 7z archive and the PPMd decoder) and CVE-2025-55188 (symbolic links written during extraction), and a restore extracts whatever archive an administrator points it at. Fetched from `https://www.7-zip.org/a/7z2603-extra.7z` itself - 7-zip.org's download page links a release to a GitHub release (`ip7z/7zip`), and its `/a/` directory serves the same file with no redirect - pinned by the package's SHA-256 and the member's, and taken out of the package with Windows' own `tar.exe`, which reads 7z. Every switch the server passes (`a`, `x`, `l`, `-r`, `-t7z`, `-mmt`, `-mx1`, `-w`, `-p`, `-mhe=on`, `-o`, `-y`, `-scsUTF-8`, `-slt`, `-sccUTF-8`, `@listfile`) is in 26.03's own usage text, and 26.01's new `-spo` only changes what an `*` in `-o` means, which the server never writes. The licence gained BSD 3-clause and 2-clause parts (the ZSTD decoder, XXH64); both texts are in `Licenses/License - 7Zip.rtf` and in the installer's `License.rtf`. |

### Gathered from the build machine

| Component | Files | Note |
|---|---|---|
| MSVC v145 runtime (`installation/Microsoft.VC145.CRT/`) | 10 | Version 14.51, validly signed by Microsoft. Shipped beside the server so a target machine needs no separate redistributable. Copied from the Visual Studio that built the server, so the shipped runtime tracks the build machine's patch level. |

### Copied from Windows

| Component | Files | Note |
|---|---|---|
| ADO 2.8 type libraries (`libraries/msado28/`) | 2 | `#import`ed at compile time by `stdafx.h` to generate the ADO wrappers the server compiles against. Windows' own files, copied from `%CommonProgramFiles%\System\ado` and its x86 twin. The copy varies with the Windows build, so the manifest records a hash for reference only and the regression suite is the proof. Nothing of them is redistributed: no ADO type library is in the installer (the only `.tlb` it carries is the server's own `hMailServer.tlb`), and what the server carries is the interface definitions `#import` generated from the build machine's own copy of a Windows component, as any program built against ADO does. |

### Ours, not third-party

| Component | Files | Note |
|---|---|---|
| `source/Tools/Interop/Interop.hMailServer.dll` | 1 | TlbImp output from *this project's* own `hMailServer.idl`. `build/generate-com-wrapper.ps1` makes it from the server build's type library, or from the IDL alone with MIDL. It stays in the manifest as `generated` so that it is refused if it is ever committed again; it has no hash there because TlbImp output is not byte-reproducible (a fresh module id per run). [Interop/README.md](../source/Tools/Interop/README.md). |

### Deleted on 11 September 2026

| File | Why it went |
|---|---|
| `installation/System Files/dnsapi.dll` | A Windows 2000 DNS Client API, 5.00.2195.6680, that no installer line and no build step used, and a DLL-planting hazard the day someone copied it beside the executable. |
| `installation/isxdl.dll` | ISX Download DLL 5.1.5.0. The installer declared its exports and called none of them. |
| `installation/SQLCE/SSCERuntime_x86-ENU.msi` | SQL Server Compact 4.0 SP1 for x86. The installer is x64 only. |
| `installation/System Files/ATL/atl70.dll` | The ATL 7.0 runtime from Visual Studio .NET 2003. Nothing on a v145 build links it. |
| `installation/ISC.dll` | An unsigned 45 KB DLL with no version resource and no known upstream, loaded by the installer for one port check. Replaced by a WinSock bind test in the installer's own `[Code]`, with the same answer for the same three ports. |

What checks it
--------------

The `Binary provenance` workflow (`verify-binary-provenance.yml`, a GitHub
workflow, in the git history at `v6.3.3`)
ran on every push and pull request to master on GitHub until 18 September 2026,
and answered three questions:

1. **Has a file the manifest marks as not in git been committed after all?**
   Hard failure: the build fetches, gathers or generates it.
2. **Is a fetched or gathered file on disk different from the manifest?** Hard
   failure.
3. **Has a binary appeared that nobody wrote down?** Hard failure. Adding a DLL
   to a Windows project takes two seconds and is invisible in review.

Its detector matched OpenSSF Scorecard's: the extension set from Scorecard's
`checks/raw/binary_artifact.go`, plus a content sniff for PE, ELF and OLE
headers, because an extension-only scan misses a PE image with another
extension (the `.tlb` type libraries were two). A Windows job of the same
workflow fetched the build inputs, checked their hashes, and checked the
signature of every fetched file the manifest records as signed.

On GitLab, from 6.3.6, two jobs do what it did, and nothing does on the
repository published on GitHub again since 18 October 2026: the two workflows of
the project's that GitHub Actions runs there are OpenSSF Scorecard's and
CodeQL's, and a pull
request made there is checked when a maintainer lands it through GitLab, as a
`batch*` branch and a merge request. `checkers` runs
`build/check-binary-manifest.py` in every pipeline, merge requests included: the
first and third questions over every tracked file. And `build-inputs-authenticode`,
on bench150 for every protected ref (`.gitlab/ci/packages-windows.yml`), runs
`build/ci/build-inputs-authenticode.ps1` before any build of the pipeline uses the
inputs - `windows-packages-x64`, `windows-packages-arm64` and `windows-installer`
need it:

1. every entry of `build_inputs` is downloaded afresh into an empty folder, from
   the package registry for a `build-inputs-<n>` release and from the publisher's
   address, following no redirect, for an https one, and must have its pinned
   SHA-256;
2. every fetched artifact, x64 and ARM64, is taken from its input - the input
   itself, or the member of an archive, extracted as
   `build/get-installer-binaries.ps1` extracts it - and must have its own pinned
   SHA-256;
3. a file whose `authenticode` is `Valid` must have a valid signature by exactly
   the `authenticode_signer` recorded, with an RFC 3161 timestamp; one recorded
   `NotSigned` must carry none. A fetched file recorded any other way - with no
   record, `NotRecorded`, or either word spelt otherwise - is refused, since it
   would pass unchecked; so is a manifest that records no fetched file as `Valid`,
   since the job would then check no signature.

On 2 October 2026 it passed on the maintainer's laptop: the three inputs had their
hashes, the SQL Server Compact runtime its Microsoft signature and timestamp, and
the twenty-two unsigned files no signature. `build/ci/authenticode-selftest.ps1`,
which the job runs next, takes the inputs once and refuses a manifest recording
another signer, an unsigned file as `Valid`, the signed one as `NotSigned`, a
fetched file as `NotRecorded` or `notsigned`, another hash or no `Valid` file; the
key of the throwaway certificate it signs with is made by a name of its own and
deleted when SignTool is done. By hand, `build/get-installer-binaries.ps1 -Verify` answers
the second question for what is in place on the machine.

Updating the manifest
---------------------

When you add, update or remove a build input:

1. Upload the new file to the generic package `build-inputs/<n>` of the
   project's package registry, for a new `<n>`, which is where
   `get-installer-binaries.ps1` fetches a `build-inputs-<n>` release's files
   from, and make the release `build-inputs-<n>` link to it, leaving the old one
   as it is; or change where it is gathered from.
2. Update its entry in `third-party-binaries.json` - `sha256`, `size`,
   `version`, `upstream`, `source`, and `authenticode` if the signing status
   changed - and the `build_inputs` hash of the release asset.
   Get a hash with
   `(Get-FileHash <path> -Algorithm SHA256).Hash.ToLower()`.
3. Update the relevant row in this document if the reasoning changed.
4. Run `build/get-installer-binaries.ps1`, then again with `-Verify`.

For a new MariaDB Connector/C, the file of step 1 is the archive
`libraries/build-mariadb-connector-c.ps1 -Version <version>` writes: add the
source archive's SHA-256 to the script's digest table first, from MariaDB's
`sha256sums.txt` for that release after checking its signature, and take step 2's
hashes and sizes from the `SHA256SUMS` the script writes beside the archive. The
version is in the directory's name too, `libraries/mariadb-connector-c-<version>`,
which `post-build.bat`, the VM test runner's `TestEnvironments.cs`, the manifest's
paths, `CONTRIBUTING.md` and `RegressionEnvironment.md` name; `.gitignore` ignores
every version's directory and names none. Each Windows bench needs the new
directory before its first gate of the change, and `build\get-installer-binaries.ps1`
places only what the manifest of the checkout it runs in names. So, under that
bench's gate lock, put the bench's checkout at a commit that carries the new
manifest, run the script, and run it again with `-Verify`. Keep the old directory
while commits on the old manifest still gate on that bench: their `post-build.bat`
copies the client from it.

Do not "fix" a failure by copying a hash the script printed into the manifest
without knowing why it changed. That is the one move that turns this whole
mechanism into decoration.

Relationship to OpenSSF Scorecard
---------------------------------

Scorecard's `Binary-Artifacts` check reported one alert per committed binary -
forty of them until 11 September 2026 - and has no per-file suppression. With no
binary committed there is nothing left for it to report, and no maintainer
annotation to add. Scorecard ran on GitHub and does not run on GitLab.

Every build input, and its pin
------------------------------

Read on 25 September 2026 from every download under `build/`, `libraries/` and
the Linux packaging. An input is *pinned* when the build refuses any bytes but
the ones recorded for it, and *named* when the SBOM lists it.
`build/check-sbom-inputs.py` fails when an input pinned in
`third-party-binaries.json`, `build/native-dependencies.ps1`,
`build/get-installer-binaries.ps1`, `build/get-dotnet-runtime.ps1` or found by
the Linux `CMakeLists.txt` is missing from the SBOM's component list, or is
there at another version or with another hash; `--sbom` holds a finished SPDX or
CycloneDX document to the same rule.

| Input | Fetched by, from | Pinned by | In the SBOM |
|---|---|---|---|
| OpenSSL 4.0.3, Boost 1.92.0 and PostgreSQL 18.6's libpq, as source | `libraries/build-openssl.ps1`, `build-boost.ps1` and `build-pgsql.ps1`, Boost's and PostgreSQL's from each project's own release server; OpenSSL's release archive from Debian's archive (`deb.debian.org`'s pool, or `snapshot.debian.org` once a version has left it), which ships it unchanged as `openssl_<version>.orig.tar.gz` - OpenSSL publishes its archives only on GitHub, which the project does not download from - or from a copy passed with `-Archive` | The archive's SHA-256, in the script; the version, by the include paths in `hMailServer.vcxproj` | Yes, at the include paths' version |
| zlib 1.3.2 and QuickJS 2026-06-04 | Vendored as source in the tree | git | Yes; zlib since 25 September 2026 |
| 7-Zip 26.03 | `build/get-installer-binaries.ps1`, from `https://www.7-zip.org/a/7z2603-extra.7z` | SHA-256 in `third-party-binaries.json`, of the package and of the member | Yes, with the package's SHA-256 |
| MariaDB Connector/C 3.4.11, as source | `libraries/build-mariadb-connector-c.ps1`, from MariaDB's own archive (`archive.mariadb.org`), to build the archive `build-inputs-2` holds | The source archive's SHA-256, in the script - MariaDB's own value, from its signed `sha256sums.txt` | Through the built files, below |
| MariaDB Connector/C 3.4.11 (built), SQL Server Compact 4.0 SP1 | `build/get-installer-binaries.ps1`, from the `build-inputs-2` and `build-inputs-1` releases' files in the project's package registry on GitLab (`build-inputs-1` from GitHub until 26 September 2026; the copy is byte for byte) | SHA-256 in `third-party-binaries.json`, of each archive and each file | Yes, with the archive's SHA-256, since 25 September 2026 |
| The MSVC v145 runtime, 14.51.36231.0 | The same script, from the build machine's Visual Studio | Its version (major.minor), against the toolset | Yes, by version, since 25 September 2026 |
| The .NET Desktop Runtime 10.0.13 | `build/get-dotnet-runtime.ps1`, from builds.dotnet.microsoft.com | The SHA-512 in Microsoft's release metadata, in `third-party-binaries.json`. Until 25 September 2026: whatever aka.ms/dotnet/10.0 served that day, if it was larger than 40 MB | Yes, with the SHA-512, since 25 September 2026 |
| The Linux packages' libraries: glibc, libstdc++, OpenSSL 3.0 or later, zlib, libpq, OpenLDAP, and Boost 1.83 or later (static) | The distribution, and the build image's Boost | Not pinned: the Linux build's log records the versions it built against (RELEASE.md, step 8) | Yes, by name and with no version, since 25 September 2026 |
| NuGet packages | `dotnet restore --locked-mode` | The committed `packages.lock.json` files | Yes, by Syft |

Not pinned, and why:

* **The AppImage's runtime and packing tools.**
  `hmailserver/source/Server/platform/packaging/build-appimage.sh` no longer
  uses linuxdeploy or its AppImage plugin, which are published only on GitHub;
  until 18 September 2026 it took them from their GitHub releases' `continuous`
  tag, which moves, and the plugin's runtime was inside every AppImage built.
  The runtime is now the project's own, `appimage/runtime.c` beside the script,
  built by `build/ci/packages/appimage-runtime.sh` in an `alpine:3.24` image
  pinned by digest and linked statically against Alpine's squashfuse, libfuse3
  and decompressor packages; the script packs the AppDir with ldd, patchelf and
  mksquashfs from the Ubuntu 24.04 image's archive
  ([PackagesAppImage.md](PackagesAppImage.md)). Those packages are not pinned
  by version: `apk add` and `apt-get install` take what the release's archive
  holds that day. `runtime-<arch>.txt`, kept beside each runtime, records each
  Alpine package it was linked from with its version, and the runtime's
  SHA-256; `runtime-<arch>-linked.txt` records each file the link took, from
  the linker's map, with its package, version and the aports commit it was
  built from. The `appimage-runtime-sources` job fetches the source of each of
  those libraries at exactly that version and commit - the upstream archive
  from Alpine's distfiles mirror and Alpine's build script and patches from
  aports, each held to the SHA-512 the build script gives - and the release
  carries it, so an unpinned version is still one whose source goes out with it
  ([PackagesAppImage.md](PackagesAppImage.md), "Licences"). So with the Ubuntu
  libraries the image carries in `usr/lib`: each image's `bundled.txt` records
  each one's package, version and source package and version, and the
  `appimage-bundled-sources` job fetches each source package at exactly that
  version from Ubuntu's archive (or its snapshot service), each file held to the
  checksums of the signed index and of its `.dsc`, and the release carries it.
* **The Public Suffix List.** `build/generate-public-suffix-list.ps1` downloads
  the current list from publicsuffix.org, as its maintainers ask. What the
  build compiles is the generated `PublicSuffixListData.h`, committed with the
  list's own VERSION and COMMIT lines, so the review of that diff is the pin;
  `-ListFile` regenerates it from a saved copy, and `-Version` and `-Commit`
  from a copy out of the list's repository, which carries no such lines. The
  header was last regenerated that way on 30 September 2026, to add the
  PRIVATE section's rules as arrays of their own (the SURBL test's lookup by
  the ICANN section alone): from `public_suffix_list.dat` in Debian's
  `publicsuffix_20260725.1419.orig.tar.gz` (snapshot.debian.org, SHA-256
  `d6f19f4a59dacf493080d154f9296f1407adc318dd6b83ea166a9b04753ed4a7`), whose
  rules came out byte for byte as the published list of VERSION
  2026-07-25_14-20-03_UTC had made them, since publicsuffix.org serves only
  today's list.
* **The container image's packages.** The Dockerfile pins `ubuntu:24.04` by
  digest and installs `ca-certificates`, `libcap2-bin`, `7zip`, `python3` and,
  from the release after 6.3.7, `libmariadb3` (MariaDB Connector/C, which
  brings `mariadb-common` and `mysql-common`) from the archive at whatever
  version it has that day.
* **The toolchains.** Visual Studio (toolset v145, which the project names),
  the Windows SDK, Inno Setup 6 - whose setup loader is part of every
  installer - and Strawberry Perl are installed on the build machine, not
  fetched by a script. So is the .NET SDK: `global.json` names the lowest one a
  build accepts, 10.0.303, and a build runs the newest .NET 10 SDK installed
  (`rollForward: latestFeature`). Python, Meson,
  Ninja and winflexbison, which only libpq's build runs, are installed on a
  developer's machine, or fetched by `libraries/get-libpq-tools.ps1`, which
  `windows-packages-arm64` runs on bench150: Python 3.14.7 from nuget.org,
  Meson 1.11.1 and Ninja 1.13.2 from PyPI and winflexbison 2.5.24 from
  SourceForge, each held to the SHA-256 the script pins
  ([PackagesWindows.md](PackagesWindows.md), "The ARM64 libraries"). None of them
  is in anything the build ships.
* **What the GitHub workflows fetched.** They have not run since
  18 September 2026, and are in the git history at `faca023d4`. The two inputs they left unpinned were the SLSA generator,
  referenced by tag because its verifier requires it (`sign-release.yml`), and
  wingetcreate's `latest` (`package-managers.yml`, which refused it unless its
  Authenticode signature was valid).
* **Not inputs.** `build/make-package-manifests.ps1` downloads the release's own
  installer to record its hash in the winget and Chocolatey manifests, and
  `build/check-package-manifests.py` fetches the winget schema to validate them;
  neither goes into anything the build makes. Windows' own ADO type library
  (`system` above) and the three SQL Server Compact 3.5 OLE DB headers committed
  under `libraries/mssqlce3.5` are compiled against as the Windows SDK's headers
  are and are never redistributed, so they are not in the SBOM; the runtime the
  headers describe is, as SQL Server Compact.

### The ARM64 installer's entries

The entries marked `"platform": "arm64"` are the ARM64 installer's
([WindowsOnArm.md](WindowsOnArm.md)): the ARM64 C++ runtime, 7-Zip's ARM64
console (the `arm64/7za.exe` member of the same pinned 7-Zip Extra archive) and
the `win-arm64` .NET Desktop Runtime of the same release as the x64 one, all
under `hmailserver/installation/ARM64`. `get-installer-binaries.ps1` and
`get-dotnet-runtime.ps1` place them only with `-Platform ARM64`, and without it
they skip them, so the x64 build and its `-Verify` are what they were. Moving
the .NET pin moves both runtimes: the ARM64 entry's SHA-512 is in the same
release metadata, under `windowsdesktop-runtime-win-arm64.exe`, which
`show-dotnet-runtime-pin.ps1 -Platform ARM64` prints.

### Moving the .NET runtime pin

The installers' runtime moves when Microsoft ships a servicing release of .NET
10 worth taking - a security release above all - and never by itself:

1. `powershell.exe -NoProfile -ExecutionPolicy Bypass -File build\show-dotnet-runtime-pin.ps1 -Version 10.0.N`
   (the channel's latest without `-Version`) reads Microsoft's release metadata
   from builds.dotnet.microsoft.com and dotnetcli.blob.core.windows.net,
   refuses to answer when the two disagree, and prints the release's date, the
   CVEs it names, its address and its SHA-512 for the x64 installer's runtime;
   with `-Platform ARM64`, the same for the ARM64 installer's.
2. Copy each answer's `version`, `sha512` and `source` into its entry in
   `third-party-binaries.json` -
   `hmailserver/installation/DotNet/windowsdesktop-runtime-10.0-win-x64.exe` and
   `hmailserver/installation/ARM64/DotNet/windowsdesktop-runtime-10.0-win-arm64.exe` -
   and the release into each one's `upstream`. The two move to one release.
3. `build\get-dotnet-runtime.ps1`, and again with `-Platform ARM64`, downloads
   each, checks it and replaces the old file; `-Verify` checks the file in place.
4. `python build/check-sbom-inputs.py` and `build\test-get-dotnet-runtime.ps1`.
5. Commit the two entries by themselves, saying which release it is and why.

A new major version is a different change: the `<TargetFramework>` of the tools,
`global.json`, `DOTNET_MAJOR` and `DOTNET_CHANNEL` in `hMailServer64.iss`, and
the file's path in `get-dotnet-runtime.ps1` and here, together.

Connector/C 3.4.11, built from MariaDB's source
-----------------------------------------------

**Why.** MariaDB Connector/C 3.4.1 to 3.4.9 have CVE-2026-107819 (Medium): when
the client checks the server's certificate and the check fails only on the host
name - a certificate a trusted authority issued, for another name - the
authentication switch that follows did not refuse a plugin that sends the password
as it is, so a man in the middle could ask for `mysql_clear_password` and read the
database password. Fixed in 3.4.10 by MariaDB's CONC-846 (commit e8c0a16d94dd),
whose change to `plugins/auth/my_auth.c` refuses such a plugin after any failed
check, where only an untrusted issuer did. hMailServer 6.2.3 to 6.3.6 shipped
3.4.9, and reach that code only with `TlsMode=verify-full`, no `TlsCaFile` and a
database on another machine ([DatabaseTls.md](DatabaseTls.md), "CVE-2026-107819").

**Why 3.4.11 and not 3.4.10.** 3.4.10 is a tag in MariaDB's repository that MariaDB
never released: there is no 3.4.10 on its download site or among its GitHub
releases, so 3.4.11 is the first release with the fix. The same change, CONC-846,
also made 3.4.10 check the certificate, its chain and its name, whenever a CA file
or a revocation list was given, even with checking turned off
(`libmariadb/ma_tls.c`); 3.4.11 undoes that part (CONC-854, commit be67a4fc1e04)
and keeps the `my_auth.c` fix. hMailServer would not have met that regression: it
gives the client `TlsCaFile` for `verify-full` alone, where checking is on
(`DatabaseTls::FromConfiguration`). 3.4.11 also bounds two reads of what a server
sends (CONC-842, in the row reader; CONC-850, in the GSSAPI plugin's
`parse_server_packet`).

MariaDB published 3.4.11 on 6 October 2026 as source only - no Windows package -
and no MariaDB Server release bundles it yet (the newest on 12 October 2026: 13.1.1
and 13.0.2 of 15 September, 12.3.3, 11.8.9 and 11.4.13 of 22 August). So the files
in `build-inputs-2` are this project's build, by
`libraries/build-mariadb-connector-c.ps1`.

**How the fix is shown.** By the source: the signed 3.4.11 archive's `my_auth.c`
tests `tls_verify_status` whole where 3.4.9's tested its trust bit alone, and
refuses with "Failed to verify the server certificate", where 3.4.9 said
"Certificate verification failure: The certificate is NOT trusted."; and by the
built file: `libmysql.dll` in `build-inputs-2` carries the new message, which
3.4.9's does not. The flaw itself needs a certificate that a root in Windows' own
store trusts, issued for another name, and no test here stages one. What was
measured, with `hMailServer.exe --check-database` against MariaDB 11.8 and MySQL
8.4 through both clients on 12 October 2026, is that the swap changes nothing
else hMailServer relies on ([DatabaseTls.md](DatabaseTls.md), "CVE-2026-107819").

**The source.**
<https://archive.mariadb.org/connector-c-3.4.11/mariadb-connector-c-3.4.11-src.tar.gz>
(1,421,044 bytes), SHA-256
`521c0712e9291fa96558df9e2ff431376a3a79329f13751896b694cae12765b4`: the value in
MariaDB's `sha256sums.txt` beside it and in MariaDB's download API. Both files'
`.asc` signatures were good on 12 October 2026, by the MariaDB Signing Key
(`177F 4010 FE56 CA33 3630 0305 F165 6F24 C74C D1D8`; the copies at mariadb.org and
supplychain.mariadb.com are the same key). The script pins the SHA-256 and refuses
any other archive.

**How it is built: as MariaDB built 3.4.9.** The 3.4.9 files' debug records name
MariaDB's buildbot building `libmariadb` inside the server's tree, RelWithDebInfo,
and they import the C runtime DLLs and Schannel's libraries. MariaDB Server's own
build (read from `mariadb-12.3.2.tar.gz`, checked the same way) gives the
Connector/C it bundles its compiler settings, in `cmake/os/Windows.cmake`, and its
options, in `cmake/mariadb_connector_c.cmake`. The script writes a small CMake
project that gives Connector/C the same - the C runtime DLLs (`/MD`), RelWithDebInfo
with its default inlining restored, `/Gw`, `/utf-8`, the same warnings as errors,
`/OPT:REF,ICF`; Schannel for TLS, an external zlib, no curl, the MySQL
compatibility names, the three transports linked in - and adds Connector/C as its
subproject. Built on its own, Connector/C links the C runtime statically, which is
what MariaDB's own 3.4.9 MSI is. The external zlib is the tree's,
`hmailserver/source/Server/zlib`, 1.3.2 - the version MariaDB Server 12.3.2 bundles;
Connector/C's own copy is 1.3.1. The client calls `compress2`, `uncompress` and
`crc32` and nothing else of it.

What differs from MariaDB's build, as far as the files show: the compiler (Visual
Studio 2026, linker 14.51, where MariaDB's was 14.44); whole-program optimisation,
which this build does not use and MariaDB's builder may (its command line is not
published); the subsystem in the DLLs' headers (GUI, where MariaDB's says console,
a field a DLL does not use); the PE time stamp and debug identifier, derived from
the content; and the server version the client reports as its own
(`mysql_get_client_info()`): MariaDB's build is given the server's, 12.3.2 for
3.4.9, and on its own Connector/C reports its default, 10.8.8. hMailServer reads
only `mysql_get_client_version()`, the connector's own version, 30411.
`libmysql.dll` imports exactly the DLLs MariaDB's 3.4.9 did, and the script checks
that it does, that each plugin exports `_mysql_client_plugin_declaration_` and
imports no client library (the client is renamed beside hMailServer, so one that
did would not load), and that every file is an x64 image and says it is Connector/C
3.4.11.

**Reproducible.** What MariaDB's build does not do and this one does: `/Brepro`,
`/pathmap` for the source and build folders (with `/experimental:deterministic`,
which it needs) and `/PDBALTPATH`, and an archive written with a fixed order, a
fixed time stamp and no compression. Built twice on 12 October 2026 in two
library folders - Visual Studio 18.10 (MSVC 19.51.36260), Windows SDK 10.0.26100.0,
CMake 4.3.1-msvc1, Ninja 1.13.2 - the archive and every file in it came out
identical, and the archive's SHA-256 is the manifest's `build_inputs` value. To
check it: on Windows with Visual Studio 2026 and its "C++ CMake tools for
Windows", set `hMailServerLibs` to an empty folder, run
`pwsh -File libraries\build-mariadb-connector-c.ps1 -Version 3.4.11`, and compare
the first line of the `SHA256SUMS` it writes with that value; it writes the tools
it used to `build-environment.txt`, and another compiler gives other bytes. The
PDBs, published beside the archive for reading a crash dump, are not reproducible;
the DLLs' debug records match either build's.

**Seven plugins, not nine.** The 3.4.9 archive held two files that are not
Connector/C's and are not in the 3.4.11 one: `auth_named_pipe.dll`, MariaDB
Server's own `named_pipe` authentication plugin (it exports
`_maria_plugin_declarations_`, a server plugin's entry; no client ever loads it),
and `authentication_windows_client.dll`, the client of MySQL Enterprise Edition's
Windows authentication. Both are built from MariaDB Server's tree (`plugin/auth_pipe`,
`plugin/win_auth_client`, the latter Oracle's code under the GPL version 2 only),
carried MariaDB Server's version, 12.3.2.0, and were recorded here as LGPL
Connector/C files, which they were not. An upgrade removes both from `Bin\plugin`
(`section_files_common.iss`, `[InstallDelete]`), so a database account that signs
in with MySQL Enterprise Edition's Windows authentication (`authentication_windows`)
can no longer sign in, and the server cannot open its database after the upgrade.
None is known to this project. Such a server can take
`lib/plugin/authentication_windows_client.dll` from a MariaDB Server Windows
archive into `Bin\plugin` - but every later upgrade deletes it again, since the
deletion has no condition, so it has to be put back after each upgrade before the
server can open its database. An account that signs in another way
(`caching_sha2_password`, say) needs none of that.

The installer names the seven plugins one by one (`section_files_common.iss`),
where until 6.3.7 it took `Extras\plugin\*.dll`: `get-installer-binaries.ps1`
places files and removes none, so a checkout that once fetched the 3.4.9 files
keeps the two above in `Extras\plugin`, and the wildcard would have shipped them
again. `build/check-installer-scripts.py` holds the named plugins to this manifest's
`Extras/plugin` entries, both ways.

Provenance established for the Connector/C 3.4.9 files (until 6.3.7)
--------------------------------------------------------------------

On 12 September 2026 the ten files in `build-inputs-1/mariadb-connector-c-3.4.9-win64-client.zip`
were traced to the archive MariaDB published. They are not from the Connector/C
3.4.9 Windows installer (`mariadb-connector-c-3.4.9-win64.msi`, SHA-256
`9e1f530a34f2c2f3a2fc963ffe9d38f2399791ed3ff559fbbc815d92bd9b79b3`), whose
`libmariadb.dll` and plugins are a different, statically linked build. They are
the client files of **MariaDB Server 12.3.2 for Windows x64**:

- Archive: <https://dlm.mariadb.com/4716004/MariaDB/mariadb-12.3.2/winx64-packages/mariadb-12.3.2-winx64.zip>
  (103,730,527 bytes), SHA-256
  `67347c129eb9c5923d002ea34fbfa27c60eb95d36dd73b85af2651cdeceecac5`, which is
  the value MariaDB publishes beside it in
  <https://dlm.mariadb.com/4716014/MariaDB/mariadb-12.3.2/winx64-packages/sha256sums.txt>
  (the archive also carries a detached `.asc` signature there).
- `libmysql.dll` is that archive's `lib/libmariadb.dll`, byte for byte (SHA-256
  `fba94855d8b5373eb63772aca9473bb201d772bb286fd2be9b910d8a51057f76`); its
  version resource says so too (OriginalFilename `libmariadb.dll`, product
  MariaDB Connector/C 3.4.9). It is renamed because `MySQLInterface.cpp` loads
  `libmysql.dll` beside the executable on Windows, as it always has.
- The nine `plugin/*.dll` are that archive's `lib/plugin/` files of the same
  names, byte for byte; `authentication_windows_client.dll` and
  `auth_named_pipe.dll` carry the server's own version (12.3.2.0), the seven
  others the connector's (3.4.9). (Until 12 October 2026 this said that only the
  first did; both are MariaDB Server's, as above.)

So the chain of custody of the 3.4.9 files started at the archive MariaDB
published, checked against MariaDB's own checksum file, and every hash in
`third-party-binaries.json` followed from it; `build-inputs-1` keeps the archive.
The LGPL-2.1 text the connector's licence requires to accompany a distribution is
in `Licenses/License - MariaDB Connector C.txt`.

Not yet done
------------

Honest list of what this document does not cover.

- **Nothing re-verifies anything automatically.** The provenance workflow and its
  Windows signature job ran on GitHub only.
- **The Connector/C files are placed twice.** One installer line pointed at
  `libraries/` removes the second copy.
- **Nothing builds the Connector/C archive again to compare it with
  `build-inputs-2`.** The build is reproducible, but only someone who runs
  `libraries/build-mariadb-connector-c.ps1` checks it. When a MariaDB Server
  release bundles Connector/C 3.4.11 or later, its Windows archive's client files
  can replace this build, as MariaDB's own bytes did for 3.4.9.
- **The MSVC runtime's signature is recorded, not checked.** A hosted runner does
  not have the tree's Visual Studio, so the gathered files' signer was checked by
  hand when the manifest was written.
