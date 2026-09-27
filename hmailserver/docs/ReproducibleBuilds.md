Reproducible builds
===================

**Anyone with the same source, the same toolchain and the same libraries can
build the server and get the same bytes the project published - on Windows,
`hMailServer.exe`; on Linux, the `hmailserver` binary, the `.deb` and the
`.rpm`. This page says how each build is made reproducible, how it is checked,
what was measured, and what is not claimed.**

A build is reproducible when, given the same source code, build environment
and build instructions, any party can recreate bit-for-bit identical copies of
the specified artifacts. That is the definition of reproducible-builds.org, and
what the OpenSSF Best Practices criterion `build_reproducible` asks for. It is
what lets someone who does not trust the project's build machine check that a
published binary came from the published source: they build it themselves and
compare the SHA-256.

What is covered, and what is not:

| Artifact | Reproducible | Checked by |
|---|---|---|
| `hMailServer.exe`, Windows x64 Release | Yes, with the same MSVC toolset, Windows SDK and libraries; from any checkout path since 26 September 2026 - the releases after 6.3.4 - and before that, 6.3.4 included, only from the path it was built in (see [What was measured on Windows](#what-was-measured-on-windows)) | Clean builds for every release (RELEASE.md step 8), one of them in a second checkout at another path |
| `hmailserver`, the Linux binary | Yes, in the same CI image | The `linux-repro` job, wherever `linux-build` runs; a release waits for it |
| The `.deb` and the `.rpm`, byte for byte as a whole | Yes, in the same CI image, since 26 September 2026 | `linux-repro` |
| The packages' own SBOMs (`<package>.cdx.json`) | Not compared: `build/asset-sbom.py` makes the same document from the same package and the same image (its self-test holds it to that), but `linux-repro` compares the packages, not the documents | - |
| The Windows installer (`hMailServer-*.exe`, Inno Setup) | Not claimed | - |
| `hMailServer.Updater.exe` and `hMailServer.Minidump.exe`, the two helper programs the installer puts beside `hMailServer.exe` | Not claimed: their projects pass none of the options below, not even `/Brepro`, so each carries the time it was linked | - |
| The Authenticode signature, the Sigstore bundles | Not reproducible by design: a signature is made by a key, once | - |
| The Control Panel and the tools (.NET) | Not claimed | - |

---

## Windows

### What makes the executable reproducible

The Release configuration of `hmailserver/source/Server/hMailServer/hMailServer.vcxproj`
passes:

- **`/Brepro`** to the compiler and the linker. MSVC otherwise stamps the time
  into every object file and into the PE header's `TimeDateStamp`; with it, the
  stamp is a hash of the content.
- **`/d1trimfile`**, twice: the checkout's root and the libraries' root
  (`$(hMailServerLibs)`), both as full paths. The compiler strips them from the
  front of every path it writes - `__FILE__`, `std::source_location` and the
  debug records - so `hmailserver\source\Server\Common\Util\Time.cpp` and
  `boost_1_92_0\boost\asio\impl\io_context.hpp` are what the binary holds,
  wherever the checkout and the libraries are.
- **`/pdbaltpath:%_PDB%`** to the linker: the executable names its PDB by file
  name only, not by the absolute path it was linked from.
- **`OPENSSL_NO_FILENAMES`**, defined for every configuration, so OpenSSL's
  allocation macros pass no `__FILE__` at all.

Nothing in the server embeds the commit, the build time or the machine's name.
The version is `Version.h`'s, which is part of the source.

Until 26 September 2026 the project passed `/d1trimfile:$(SolutionDir)`, the
directory of the project file, and that trimmed nothing outside it. Sixteen of
the source files compiled into the Release executable of `40c7d3e0c`, the
commit 6.3.4 was first tagged at, under `Common`, `Imap` and `Smtp`, use
`__FILE__` through `HM_TEST_FAILED` (`stdafx.h`), and
Boost.Asio's error paths record `std::source_location` for 31 headers, so the
executable carried their absolute paths:
`C:\Users\...\hmailserver\source\Server\Common\...` and
`C:\Dev\hMailLibs\boost_1_92_0\...`. Two builds in the same directory were
identical, and a build in another directory was not. See
[What was measured on Windows](#what-was-measured-on-windows).

### The toolchain to match

`build\build.ps1` names the MSBuild it uses, and its log
(`logs\build-Release.log`) gives the compiler's full path, which holds the MSVC
version. The builds measured on 26 September 2026 used:

| | |
|---|---|
| MSBuild | 18.9.1, from the Visual Studio 18 Build Tools |
| MSVC | 14.51.36231, platform toolset v145 |
| Windows SDK | 10.0.26100.0 |
| Boost | 1.92.0, in `boost_1_92_0` under `$(hMailServerLibs)`: its headers, and its `stage\lib` libraries (`libboost_*-vc145-mt-x64-1_92.lib`, Boost.Thread among them) linked in statically |
| OpenSSL | 4.0.2, `out64`, linked as DLLs (`libcrypto-4-x64.dll`, `libssl-4-x64.dll`) |
| PostgreSQL | 18.3's `libpq`, linked as a DLL |
| The ADO type library | `libraries\msado28\msado28-x64.tlb`, which the precompiled header `#import`s: Windows' own `msado28.tlb`, copied there by `build\get-installer-binaries.ps1 -SystemOnly` (git ignores it). The one the measured builds used had SHA-256 `10f86c59f9b90449e145b8ec671a3e5dfadc489c2f4c33474071b8a4b18c187e` |

The libraries are built by `libraries/build-boost.ps1`, `build-openssl.ps1` and
`build-pgsql.ps1`. The
directory names under `$(hMailServerLibs)` matter, because they are what
remains of a path after trimming (`boost_1_92_0\boost\...`); the directory
itself does not, since the fix above.

### How to check

From a clean checkout of the release's tag, with the toolchain above and
`hMailServerLibs` naming the libraries:

```
powershell.exe -NoProfile -ExecutionPolicy Bypass -File build\get-installer-binaries.ps1 -SystemOnly
pwsh -NoProfile -File build\build.ps1 -Configuration Release -Clean
Get-FileHash -Algorithm SHA256 hmailserver\source\Server\hMailServer\x64\Release\hMailServer.exe
```

and compare with the hash the release notes give, or with the
`hMailServer.exe` an installation of the release puts in its `Bin` directory:
only the installer itself is Authenticode-signed, so the executable inside it
is the unsigned file that was built. From 6.3.5 the installer is built by the
`windows-installer` job in bench150's checkout, whose log prints the hash of
the executable it packs, and RELEASE.md ("The release on GitLab", step 3)
holds that hash to the notes' before a release goes out. `build.ps1` runs with
the project's pre- and post-build events off, so it neither stops nor
registers an installed hMailServer.

An executable built from a tree without the fix - 6.3.4 and every release
before it - carries the paths it was built at, so it reproduces only from a
checkout at the same path, with the libraries at the same path (and one built
before `/Brepro` arrived, on 22 August 2026 in 6.2.23 Alpha 2, not even from
there): for the builds of `40c7d3e0c` measured below,
`C:\Users\chris\Documents\projects\hmailserver` and `C:\Dev\hMailLibs`.

A maintainer making a release does the same more than once (RELEASE.md step
8): two clean builds, and since 26 September 2026 one more in a second
checkout of the same tree at another path, because two builds in one
directory cannot show a path leaking into the binary. At that step the
version stamp is not committed yet, so the second checkout is made from
`git stash create`, a commit of the working tree that changes nothing in it:

```
$tree = git stash create; if (-not $tree) { $tree = 'HEAD' }
git worktree add --detach ..\hm-repro $tree
```

### What was measured on Windows

All on the maintainer's laptop, with the toolchain and libraries above.

- **Two clean builds of the v6.3.4 tag, in one directory: identical.** On 26
  September 2026, 01:43 to 01:47, the by-hand release build of 6.3.4 built
  commit `40c7d3e0c` (the tag as it stood then) twice from clean in
  `C:\Users\chris\Documents\projects\hmailserver`. Both builds'
  `hMailServer.exe` had SHA-256
  `c42e31bb087f52e012abb57d0a6d840e562f2aff263d00cd36a8d76198c1b28e`. (That
  build then stopped, as it should: the binary the regression gate had run
  beforehand was another build, `06a220f2...`, and a release ships the binary
  its gate ran. That is a check of which build was tested, not of whether the
  build reproduces.)
- **The same commit from a second checkout: different.** The same morning,
  `40c7d3e0c` built from clean in a second checkout,
  `C:\Users\chris\Documents\projects\hmailserver-wt\gold-repro-winpath`, gave
  `0f5a5bfd65706074ff73f071bd8c06240a569f34b5ec9eb5ddb71921c3d0dcd4`: the same
  file size, and 12.7 million bytes different. The `c42e31bb...` executable
  holds the 16 absolute source paths and the 31 Boost header paths described
  above. So the executable of `40c7d3e0c` reproduces only from a checkout at
  `C:\Users\chris\Documents\projects\hmailserver` with the libraries at
  `C:\Dev\hMailLibs`.
- **After the fix, two checkouts at two paths: identical.** Batch40 as it
  stood that morning (`76f8def83`: 6.3.4's source and the day's fixes) with
  `/d1trimfile` trimming the checkout's root and the libraries' root, built
  from clean in `hmailserver-wt\gold-repro` and in
  `hmailserver-wt\gold-repro-winpath`, gave the same `hMailServer.exe`, SHA-256
  `53f9801fa84c18a54848a69f7bc7299c6052a13cdc8c37e8a7046867ea46c055`. It holds
  no absolute path of either checkout or of the libraries: the only strings
  under `C:\Users` or `C:\Dev` left in it, ASCII or UTF-16, are ones the source
  spells out itself (a Startup folder in `ServiceAccount.cpp`, and two in the
  embedded documentation).
- **What the compiler does with the option.** A one-file test with cl 14.51
  showed that `/d1trimfile` trims both `__FILE__` and `std::source_location`,
  takes more than one prefix, and compares text only - a prefix spelled with
  `..` trims nothing, which is why the project computes full paths - and that
  a prefix with a space in it works quoted, with its trailing backslash
  doubled.

The ARM64 configuration takes the same options; it has not been built since
the change.

---

## Linux

### What makes the binary and the packages reproducible

- `hmailserver/source/Server/CMakeLists.txt` passes
  **`-ffile-prefix-map=<source>=.`**, so the source directory's absolute path
  reaches neither `__FILE__` nor the debug information.
- `build/ci/linux-build.sh` exports **`SOURCE_DATE_EPOCH`** from the commit's
  own date (`CI_COMMIT_TIMESTAMP` in a pipeline). `__DATE__` and `__TIME__`,
  the archiver and CPack's `.deb` and `.rpm` stamp it instead of the clock.
- The packages are the same whoever makes them, since 26 September 2026:
  - `CMakeLists.txt` gives every directory the packages hold mode 0755
    (`CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS`), where CPack otherwise
    creates its staging directories with the builder's umask;
  - it gives the `.deb`'s control member Debian's modes
    (`CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION`): 0755 for `postinst`,
    `prerm` and `postrm`, 0644 for `conffiles`. CPack otherwise copies each
    file's mode on disk, so `conffiles` followed the umask it was written
    under, and the three scripts, which git records as executable, the mode
    the checkout gave them: `rwxrwxr-x` from a tree unpacked under umask 0002,
    as on linix (run 5 below), `rwxr-xr-x` from a clone under 0022, and
    `rwxrwxrwx` from the CI's own checkout, which the GitLab docker executor
    makes under umask 0000. CPack
    asks only whether the variable is set, not whether it is true, so set to
    `OFF` it is on all the same;
  - it tells `rpmbuild` to use `SOURCE_DATE_EPOCH` as the package's build
    time, to clamp every file's time to it, and to write `reproducible` as the
    build host, where it otherwise writes the clock, the time each file was
    staged and the build container's host name;
  - `linux-build.sh` sets umask 0022 before it builds, because CPack's `.deb`
    writer copies the mode of each file it wrote into that member's `ar`
    header.
- The libraries - Boost, OpenSSL, libpq - are Ubuntu's, under `/usr`, at the
  same path on every machine.
- The Linux build runs in one image, `hmailserver-ci/linux:<release>-<hash>`,
  built from `build/ci/linux.Dockerfile`; its tag is the Dockerfile's own hash,
  and `build/check-ci-image.py` holds `.gitlab-ci.yml` to it.

### How it is checked: the linux-repro job

`linux-repro` (`build/ci/linux-repro.sh`) runs on linix wherever `linux-build`
does - a pipeline of a protected ref (`master`, a `batch*` branch, a `v*` tag)
whose change is more than documentation - after it, and a tag's `release` job
waits for it. It takes `linux-build`'s artifact - the binary, the `.deb` and
the `.rpm` a release ships - and makes them a second time from the same commit,
in the same image, with the same script, `linux-build.sh`, but with everything
that must not matter changed:

- the source is a copy of the checkout at another path (`/tmp/hm-repro-source`),
  without `.git`;
- the copy's files have the modes a checkout made under umask 0027 gives them
  (0640, and 0750 for a directory or an executable), where the job's own
  checkout has 0666 and 0777 - the GitLab docker executor runs a job under
  umask 0000 - and a clone by hand usually 0644 and 0755, so a mode a package
  takes from the source shows. The `.deb`'s maintainer scripts did, until the
  setting above: runs 1 to 4 below, whose two builds shared the source's
  modes, could not see it, and run 5 did;
- the build directory is another, of another length (`/tmp/hm-repro-second-build`,
  where `linux-build` uses `/tmp/hm-build`);
- it is started under umask 0002 rather than the job's own (0000 in a GitLab
  docker executor's job, 0022 in a plain `docker run`); `linux-build.sh` sets
  its own, and this shows that the caller's does not reach the packages;
- the time zone is fourteen hours east of UTC;
- it is later, and in another container.

It then compares, and each of the three must be byte for byte identical or the
job fails:

| What | When it differs, the report shows |
|---|---|
| The `hmailserver` binary as `linux-build.sh` leaves it (not stripped) | Which ELF sections differ |
| The `.deb` | Each `ar` member's header (name, mode, owner, time, size) and digest; for a tar member that differs, the listing's differences (names, modes, owners, times, order) and which files' bytes differ |
| The `.rpm` | The header's build time and build host, and which files differ in path, size, digest, mode, owner or time |

The job's report, `linux-repro/report.txt`, keeps every digest for 30 days,
with `image-packages.txt`, the version of every package in the image; the
second build's binary and packages are kept with it when they differ. In a
pipeline the first build is always `linux-build`'s: an artifact without the
binary or the packages fails the job rather than being built again.

The same script runs by hand, in the same image, with no `linux-build/` in the
checkout: it then makes the first build too. The image is not published; it is
built from the commit's own Dockerfile with `bash build/ci/build-linux-image.sh`,
which prints the tag the commit's `.gitlab-ci.yml` names.

```
docker run --rm -v "$PWD":/src -w /src \
  -e CI_COMMIT_TIMESTAMP="$(git log -1 --format=%cI)" \
  hmailserver-ci/linux:<tag> bash build/ci/linux-repro.sh
```

### What was measured on Linux

`linux-repro.sh` ran by hand on linix on 26 September 2026, in the CI image
`hmailserver-ci/linux:26.04-1c3319908955` (Ubuntu clang 21.1.8, CMake 4.2.3),
each run making both builds itself from a `git archive` of the commit unpacked
on linix: three times on batch40 as it stood that morning (`76f8def83`), with
`SOURCE_DATE_EPOCH` 1790402801, the commit's date, and then on the change that
brought this page, as it grew. Each second build took 11 to 16 minutes with
six jobs. The batch40 commits named here and under Windows are the batch as it
stood that day, before it was re-cut onto `master`: they name the trees that
were measured, and are not commits on `master`.

- **Run 1, the tree as it was: the binary identical, the packages not.** Both
  builds' `hmailserver` had SHA-256
  `568c3340c580db31056977edef97db61c7664f80c6f38e891cda5fae6fc967e9`. No file
  either package installs differed in its bytes, but the second build, under
  umask 0002, made 16 of the `.deb`'s directories and 8 of the `.rpm`'s 0775
  where the first made them 0755, and its `conffiles` 0664; and the `.rpm`'s
  header carried the clock (1790405824 and 1790406588, the two builds' own
  times), and its directories and the stripped binary the time they were
  staged.
- **Run 2, with the CMake settings above: the `.rpm` identical, the `.deb`
  not.** The binary was `568c3340...` again; both `.rpm`s were
  `e9b13bb6039151b0d1c0c20ddc6c9d6c399c37ccee1e5641402fc7856d8de4f2`, built at
  1790402801 on host `reproducible`; and the `.deb`s' three members were each
  identical, yet the two `.deb`s were not. A one-file CPack project in the same
  image, packaged under umask 0022 and under 0002, showed why: the second
  `.deb`'s `ar` headers read `rw-rw-r--` where the first's read `rw-r--r--`.
- **Run 3, with `linux-build.sh` setting umask 0022: all three identical.**
  The binary `568c3340...`, the `.deb`
  `20614505d2ccc51a6cd9174abb01f9cf399c4f956158bc576dcb765064561a6d` and the
  `.rpm` `e9b13bb6...`, from both builds.
- **Run 4, the change as first committed, on batch40 at `4a7340c98`: all
  three identical.** That afternoon, with the script listing the image's 315
  packages but its copy still keeping the tree's modes, and
  `SOURCE_DATE_EPOCH` 1790428996: the binary
  `2fd9d2d5f4f3fa652697ed01d3dc5bf14bafbc2545cef7f26391c2e4ecf74b61`, the
  `.deb` `fcff2700d1c4dc0fe0689865c678dd30a323eaa96bed2a65e88566cf0e7131a0`
  and the `.rpm` `687807b0f33a7074fd70b9e8f5e4af934c012d58fac889cac7212f07d08c5818`,
  from both builds; every tester program passed in both. That `.deb` held
  `postinst`, `prerm` and `postrm` as `rwxrwxr-x`: the mode the unpacked tree
  gave the three scripts, which git records as executable, under linix's
  umask 0002. Both builds read the same tree, so nothing differed.
- **Run 5, with the copy's modes changed: the `.deb` not identical.** On the
  same batch40, with the script's copy given the modes of a checkout under
  umask 0027 and `SOURCE_DATE_EPOCH` 1790437423: the binary
  (`3774ff710e1556b664bfebafcfda187e03d2bbc8475fc8c4b8fb98afe6d5b9c4`) and
  the `.rpm` (`288a4f8543b6e23bee059d8f40f97ce67ea63de1cfc61d2dc3dd2dbdd188483c`)
  were identical, the `.deb` was not (`15320bbf...` and `8eeca4bb...`): its
  data member was the same, and its control member held the three scripts as
  `rwxrwxr-x` from the first build and `rwxr-x---` from the second. A `.deb`
  made from a clone under umask 0022 would hold them as `rwxr-xr-x`, and one
  made in the CI's own checkout as `rwxrwxrwx`. The control member's modes
  are fixed since (`CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION`, above).
- **Run 6, with the setting: all three identical.** On batch40 at
  `cda4589fc`, whose SBOM fold puts each package's own SBOM beside it, with
  the copy's modes changed as in run 5 and `SOURCE_DATE_EPOCH` 1790440984:
  the binary `bf89f5b06fc785d2d19c95fc8e01087d23256886db25f3a9619bce40821f86f9`,
  the `.deb` `33e7941ed6fa1b4e592c3a00ade9ac570fdf74da014f7086fc86f8bf7d9e79ba`
  and the `.rpm` `6c37682966e42b09c2dc50f161ed1cd0bd0ca9b5712b49b099a18eadbafaee09`,
  from both builds, the `.deb`'s control member holding the three scripts as
  `rwxr-xr-x` and `conffiles` as `rw-r--r--`; every tester program passed in
  both.

A one-file CPack project in the same image, after run 6, showed the setting's
three states: left unset, a `postinst` at 0750 or 0777 and a `conffiles` at
0664 went into the control member with exactly those modes; set to `ON`, and
set to `OFF` as well, as `rwxr-xr-x` and `rw-r--r--`.

The first builds of runs 1 to 3, made over an hour and a half apart, gave the
same binary, and the same `.deb` too, since they ran under the container's
0022.

### What is not pinned

The image is. What is in it is not, entirely: `linux.Dockerfile` starts from
Ubuntu 26.04 at a pinned digest, but the packages it installs - clang, Boost,
OpenSSL's headers, CMake - are the Ubuntu archive's on the day the image was
built. So the CI image named by a commit is reproducible from the Dockerfile
only as long as the archive has not moved. A compiler or a Boost that moved
between two image builds changes the binary legitimately. `linux-build.sh`
prints the compiler and CMake versions it used, `linux-repro`'s report repeats
them, and `linux-repro` keeps the version of every package in the image
(`image-packages.txt`) with its report for 30 days, so a release's own pipeline
says what to match.

---

## What is not claimed

- **An independent rebuild.** Every comparison above was made on the
  project's own machines - the Windows ones on the maintainer's laptop, the
  Linux ones on linix - with libraries the project built or installed. Nobody
  outside the project has yet rebuilt a release and reported the same hash
  ([ASSURANCE-CASE.md](../../ASSURANCE-CASE.md), *Residual risk*).
- **The Windows libraries built twice.** `hMailServer.exe` links OpenSSL and
  libpq as DLLs, which the installer ships beside it and which are not part of
  this comparison, and Boost's libraries statically, so a third party needs
  Boost built as the project built it. Whether `libraries/build-*.ps1` builds
  byte-identical libraries has not been measured.
- **The packages of a tree without the settings above.** A `.deb` or an
  `.rpm` built from a tree that predates them - every release up to 6.3.4,
  6.3.4 included - has the same binary inside, but its directories' modes and
  its `ar` headers follow the builder's umask, its maintainer scripts' modes
  the checkout's, and the `.rpm`'s header holds the time it was built. Only
  the binary was compared for those.
- **The installer, the .NET components and the signatures**, as the table at
  the top says.
