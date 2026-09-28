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
| Fetched from the project's `build-inputs-1` release, checked against a SHA-256 | 22 |
| Gathered from the build machine's Visual Studio, checked by version | 10 |
| Copied from the build machine's own Windows | 2 |
| Generated from this repository's own IDL | 1 |
| Deleted on 11 September 2026, because nothing loaded them | 5 |

The thirty-five are the manifest's entries; every one is marked as not in git,
and git ignores every path they are placed at. What the tree still carries is
binary only in the plain sense and is not code: the Fat Cow icon library under
`libraries/fat_cow_icons` (its images and the archive they came in), the images
beside these documents, and the OpenPGP fixtures under `build/fixtures/openpgp`.
None has an executable's extension or a PE, ELF or OLE header, so none is a
binary to the check described below, and none is in the manifest.

Where the build gets them
-------------------------

`build/get-installer-binaries.ps1` handles four kinds, named by each entry's
`disposition`:

* **fetched** - downloaded from the `build-inputs-1` release, which holds the
  exact bytes that were committed before 11 September 2026, and checked against
  the manifest's SHA-256 before it is placed. Two release assets: an archive of
  the MariaDB client and its nine plugins (placed twice, see below), and the SQL
  Server Compact 4.0 SP1 x64 runtime - a release rather than the vendors' URLs,
  because neither has a stable first-party download left. 7-Zip is fetched from
  7-zip.org itself, over https with no redirect followed, since 25 September 2026.
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
(<https://gitlab.com/Progressiverobot/hmailserver/-/releases/build-inputs-1>).
Until 26 September 2026 it downloaded from GitHub's address, which has answered
404 since. A file already in the script's cache
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

QuickJS 2025-04-26, Fabrice Bellard's JavaScript engine (MIT), is vendored as source
under `hmailserver/source/Server/quickjs` - the engine and the four libraries it needs,
byte for byte as released, with its LICENSE and VERSION; the licence text is also under
`docs/Licenses/` and is installed by every Linux package as
`/usr/share/hmailserver/licenses/QuickJS.txt` - the .deb, the .rpm and the PKGBUILD
through the CMake install rule, the AppImage by its build script. It is the engine the event scripts run
in on the Linux builds ([EventScripts.md](EventScripts.md)), built by CMake alone: the
release has no MSVC support, so it is in no Visual Studio project and not in
`hMailServer.exe`. `quickjs-libc.c`, where QuickJS's `std` and `os` modules live, was
left out of the tree. It came from its author's site,
bellard.org/quickjs/quickjs-2025-04-26.tar.xz (sha256
`2f20074c25166ef6f781f381c50d57b502cb85d470d639abccebbef7954c83bf`), and was checked
against the Ubuntu archive's pristine copy of the same release,
quickjs_2025.04.26.orig.tar.xz (sha256
`ec81da5b3249ed648dbc7df4a1654988910e66a118d9b71cb63534216efcfbbe`): the files are
identical, and only the top directory's name and the compression differ. Source, not a
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

Eleven of the thirty-five carry a valid Authenticode signature. Twenty-four do not.

| Signed by | Files |
|---|---|
| `CN=Microsoft Windows Software Compatibility Publisher` | 10 (the MSVC runtime) |
| `CN=Microsoft Corporation` | 1 (the SQL CE x64 MSI) |
| *unsigned* | 24 |

The twenty MariaDB Connector/C DLLs (ten files, placed twice) are unsigned, which
is why the recorded hash matters most for exactly those files: there is no other
way to tell that the `libmysql.dll` the build placed is the one MariaDB shipped.
The two `.tlb` files cannot carry an Authenticode signature at all - a type
library is not a signable image format - and they are Windows' own; the
generated interop wrapper is this project's and unsigned.

For the signed files a signature check is stronger than a hash pin and survives a
legitimate vendor patch bump. The gathered MSVC runtime is checked by version
rather than by hash, and its signer is recorded in the manifest; the SQL CE MSI is
checked by hash. Until 18 September 2026 a Windows job of the provenance workflow
also fetched the build inputs and checked that the MSI's signature was valid and
its signer the one recorded - see "What checks it" below.

The inventory
-------------

Grouped by component. Per-file hashes are in
[third-party-binaries.json](third-party-binaries.json).

### Fetched from `build-inputs-1`

| Component | Files | Note |
|---|---|---|
| MariaDB Connector/C 3.4.9 (`libmysql.dll` and nine plugins) | 20 | The MySQL/MariaDB client. `MySQLInterface.cpp` builds a path beside the running executable and loads `libmysql.dll` from there at runtime, together with the nine authentication plugins found through `MYSQL_PLUGIN_DIR`. Without the full plugin set, common MySQL 8 and MariaDB account types cannot authenticate. Placed twice from the same archive: under `libraries/mariadb-connector-c-3.4.9/`, which the post-build step copies beside the server, and under `installation/Extras/`, which the installer ships. The second copy is redundant - repointing `section_files_common.iss` at `libraries/` would remove it - and the manifest says so. Provenance: see below. |
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

It does not run on GitLab, and nothing there runs it yet. By hand,
`build/get-installer-binaries.ps1 -Verify` answers the second question for what
is in place on the machine; the other two have no script outside the workflow.

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
| OpenSSL 4.0.2, Boost 1.92.0 and PostgreSQL 18.3's libpq, as source | `libraries/build-openssl.ps1`, `build-boost.ps1` and `build-pgsql.ps1`, Boost's and PostgreSQL's from each project's own release server; OpenSSL's release archive from Debian's archive (`deb.debian.org`'s pool, or `snapshot.debian.org` once a version has left it), which ships it unchanged as `openssl_<version>.orig.tar.gz` - OpenSSL publishes its archives only on GitHub, which the project does not download from - or from a copy passed with `-Archive` | The archive's SHA-256, in the script; the version, by the include paths in `hMailServer.vcxproj` | Yes, at the include paths' version |
| zlib 1.3.2 and QuickJS 2025-04-26 | Vendored as source in the tree | git | Yes; zlib since 25 September 2026 |
| 7-Zip 26.03 | `build/get-installer-binaries.ps1`, from `https://www.7-zip.org/a/7z2603-extra.7z` | SHA-256 in `third-party-binaries.json`, of the package and of the member | Yes, with the package's SHA-256 |
| MariaDB Connector/C 3.4.9, SQL Server Compact 4.0 SP1 | `build/get-installer-binaries.ps1`, from the `build-inputs-1` release's files in the project's package registry on GitLab (from GitHub until 26 September 2026; the copy is byte for byte) | SHA-256 in `third-party-binaries.json`, of each archive and each file | Yes, with the archive's SHA-256, since 25 September 2026 |
| The MSVC v145 runtime, 14.51.36231.0 | The same script, from the build machine's Visual Studio | Its version (major.minor), against the toolset | Yes, by version, since 25 September 2026 |
| The .NET Desktop Runtime 10.0.12 | `build/get-dotnet-runtime.ps1`, from builds.dotnet.microsoft.com | The SHA-512 in Microsoft's release metadata, in `third-party-binaries.json`. Until 25 September 2026: whatever aka.ms/dotnet/10.0 served that day, if it was larger than 40 MB | Yes, with the SHA-512, since 25 September 2026 |
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
  SHA-256.
* **The Public Suffix List.** `build/generate-public-suffix-list.ps1` downloads
  the current list from publicsuffix.org, as its maintainers ask. What the
  build compiles is the generated `PublicSuffixListData.h`, committed with the
  list's own VERSION and COMMIT lines, so the review of that diff is the pin;
  `-ListFile` regenerates it from a saved copy.
* **The container image's packages.** The Dockerfile pins `ubuntu:24.04` by
  digest and installs `ca-certificates`, `libcap2-bin`, `7zip` and `python3`
  from the archive at whatever version it has that day.
* **The toolchains.** Visual Studio (toolset v145, which the project names),
  the Windows SDK, Inno Setup 6 - whose setup loader is part of every
  installer - Strawberry Perl, Meson, Ninja and winflexbison are installed on
  the build machine, not fetched by a script. Only the .NET SDK is pinned, by
  `global.json`.
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
release metadata, under `windowsdesktop-runtime-win-arm64.exe`.

### Moving the .NET runtime pin

The installer's runtime moves when Microsoft ships a servicing release of .NET
10 worth taking - a security release above all - and never by itself:

1. `powershell.exe -NoProfile -ExecutionPolicy Bypass -File build\show-dotnet-runtime-pin.ps1 -Version 10.0.N`
   (the channel's latest without `-Version`) reads Microsoft's release metadata
   from builds.dotnet.microsoft.com and dotnetcli.blob.core.windows.net,
   refuses to answer when the two disagree, and prints the release's date, the
   CVEs it names, its address and its SHA-512.
2. Copy `version`, `sha512` and `source` into the entry for
   `hmailserver/installation/DotNet/windowsdesktop-runtime-10.0-win-x64.exe`
   in `third-party-binaries.json`, and the release into its `upstream`.
3. `build\get-dotnet-runtime.ps1` downloads it, checks it and replaces the old
   file; `-Verify` checks the file in place.
4. `python build/check-sbom-inputs.py` and `build\test-get-dotnet-runtime.ps1`.
5. Commit the entry by itself, saying which release it is and why.

A new major version is a different change: the `<TargetFramework>` of the tools,
`global.json`, `DOTNET_MAJOR` and `DOTNET_CHANNEL` in `hMailServer64.iss`, and
the file's path in `get-dotnet-runtime.ps1` and here, together.

Provenance established for the Connector/C files
------------------------------------------------

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
  names, byte for byte; `authentication_windows_client.dll` carries the server's
  own version (12.3.2.0), the eight others the connector's (3.4.9).

So the chain of custody now starts at the archive MariaDB published, checked
against MariaDB's own checksum file, and every hash in
`third-party-binaries.json` follows from it. The LGPL-2.1 text the connector's
licence requires to accompany a distribution is in
`Licenses/License - MariaDB Connector C.txt`.

Not yet done
------------

Honest list of what this document does not cover.

- **Nothing re-verifies anything automatically.** The provenance workflow and its
  Windows signature job ran on GitHub only.
- **The Connector/C files are placed twice.** One installer line pointed at
  `libraries/` removes the second copy.
- **The MSVC runtime's signature is recorded, not checked.** A hosted runner does
  not have the tree's Visual Studio, so the gathered files' signer was checked by
  hand when the manifest was written.
