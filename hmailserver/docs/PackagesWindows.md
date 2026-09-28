Windows installers in CI
========================

Every protected pipeline builds both Windows installers on bench150: master, a
`batch*` branch and a `v*` tag. The x64 installer is the one every release has
carried. The ARM64 installer is for Windows on ARM. Both are built unsigned and
kept as the job's artifact. The jobs are in `.gitlab/ci/packages-windows.yml`,
which `.gitlab-ci.yml` includes.

**Both jobs have run on bench150; only the x64 one has built an installer.**
They were written and parse-checked on 26 September 2026 on the maintainer's
laptop ([What was run, and the result](#what-was-run-and-the-result)), and first
ran in `batch41`'s pipelines on 27 September
([What the pipelines ran](#what-the-pipelines-ran)): `windows-packages-x64`
built `hMailServer-6.3.5-70eb52b9-x64.exe` in pipeline 2887115157, and
`windows-packages-arm64` stopped at its toolchain check, on a report that was
the check's own fault.

What each job makes
-------------------

| Job | Runs on | Makes | Fails the pipeline |
|---|---|---|---|
| `windows-packages-x64` | bench150 (shell, pwsh), stage `release`, after `windows-gate` and `windows-dotnet` (and `windows-installer` on a tag) | the x64 server, tools, Control Panel and installer | yes, except exit 2 (Inno Setup missing), allowed for now (see below) |
| `windows-packages-arm64` | bench150, the same | the ARM64 server, tools, Control Panel and installer, and the ARM64 libraries once | no, `allow_failure: true` for now (see below) |

Each job builds in the runner's own checkout of the commit, as `windows-dotnet`
does. It does not use the bench's checkout (`C:\bench\hmailserver`), where
`windows-gate` runs its tests and from which the bench's hMailServer service runs,
so it cannot relink a `hMailServer.exe` that is running. The jobs still take the
bench's gate lock (`build/ci/gate-lock.ps1`) around the whole build, as
`windows-dotnet` does, because a build next to a running suite disturbs it.

They run after the bench's other jobs, not beside them. `windows-gate` and
`windows-installer` wait only 90 minutes for the lock, and GitLab gives a runner
its oldest pending job first. Started with the pipeline, these jobs would take the
lock first, and an x64 build (about an hour) followed by an ARM64 build would make
the gate, or on a tag the release's installer, give up with exit 10. So both jobs
need `windows-gate` and `windows-dotnet`, and on a `v*` tag `windows-installer`
(an optional need, because that job is only in a tag's pipeline). A job can only
need jobs of its own or an earlier stage, which is why they are in the `release`
stage. The `release` job does not need them, so they add no wait to a release. A
failed gate or .NET job means no installer from that pipeline.

The ARM64 job also needs `windows-packages-x64`. Side by side, whichever job took
the lock second would wait for the other's whole build, and the ARM64 job's first
run, with its libraries, may take longer than the 90 minutes the x64 job waits. An
x64 job that stopped with the allowed exit 2 still lets the ARM64 job run; an x64
build that failed any other way means no ARM64 build in that pipeline.

They wait up to 90 minutes for the lock, and the job timeout is 4 hours, the
runners' own `maximum_timeout`. A longer `timeout:` would be cut to 4 hours, and
the script's own stop (`RUNNER_SCRIPT_TIMEOUT`, 3h50m) has to come before it so that
`after_script` still releases the lock.

`windows-packages-x64` is allowed to fail with exit 2 only, the exit of its third
step when Inno Setup 6 is missing. Exit 2 shows as a warning; every other failure
still fails the pipeline, as the version check's exit 4 did in pipeline
2886955494. The allowance was made while bench150 had no Inno Setup for the
machine. It has had one since 26 September 2026 (ContinuousIntegration.md,
bench150), and the job passed with it in pipeline 2887115157, so the condition
for removing the `allow_failure` - Inno Setup installed and one run passed - is
met; removing it is a change to `.gitlab/ci/packages-windows.yml` still to make
([What the owner must do](#what-the-owner-must-do), 4).

Both jobs run `build/ci/packages/windows-packages.ps1 -Platform x64|ARM64`. In
order, it does the following. Any failed step stops the job with an exit code
the script documents.

1. It checks that the checkout is the commit. No untracked file may be where a
   build would pick it up (the same refusal as `windows-installer.ps1`).
2. It takes the version from `section_setup_64.iss`. For a tag, that version must
   be the tag's.
3. It looks for Inno Setup 6, installed for the machine or given by `HM_ISCC`.
4. ARM64 only: it checks the toolchain (`check-arm64-toolchain.ps1`, below).
5. It clears everything git does not track from the installer's inputs and
   deletes every tool's `publish` and `publish-arm64` folder. The two .NET
   Desktop Runtime installers are kept, because the pinned scripts hold them to
   their SHA-512.
6. ARM64 only: it makes sure the ARM64 libraries are in the cache (below).
7. It runs `get-installer-binaries.ps1 [-Platform ARM64]`, which also places the
   ADO type libraries the server `#import`s. It then runs
   `build.ps1 -Configuration Release [-Platform ARM64]`, with `hMailServerLibs`
   set to the job's library folder.
8. It runs `build-tools.ps1 [-Platform ARM64]`. For x64 it also builds the Control
   Panel: a locked restore, a build with warnings as errors, then a publish, as
   in RELEASE.md step 8. For ARM64, `build-tools.ps1` publishes the Control Panel
   itself.
9. It runs `get-dotnet-runtime.ps1 [-Platform ARM64]`, then both `get-` scripts
   again with `-Verify`.
10. It checks that no tracked file changed.
11. It runs the PE machine check over everything the installer script packs
    (below).
12. It compiles `hMailServer64.iss` or `hMailServerARM64.iss` with ISCC.
13. It checks the result. The file ISCC made must be `hMailServer-<version>-x64.exe`
    or `-arm64.exe`, and its version resource must be `section_setup_64.iss`'s
    `VersionInfoVersion` (for a tag, `<numbers>.0`).

What the job keeps in its artifact (for 7 days; GitLab also keeps the newest
successful pipeline's artifacts for each ref):

- `packages-windows/<x64|arm64>/<installer>`:
  - for a `v*` tag, `hMailServer-<version>-x64.exe` or
    `hMailServer-<version>-arm64.exe`, exactly the names the update checker and
    `build/generate-update-feed.py`'s `installer_names()` look for;
  - for a branch, `hMailServer-<version>-<first 8 of the commit>-<arch>.exe`, so
    that a branch build is never mistaken for the release its version stamp
    names.
- `SHA256SUMS` and `build-info.txt`. The second records the commit, the Visual
  Studio version, ISCC, the .NET SDK, the library versions and folder, and the
  Authenticode status, which is always unsigned.
- ARM64 only, `lockfiles/`: any `packages.arm64.lock.json` that the first ARM64
  publish created and the repository does not have yet, to be committed
  (WindowsOnArm.md).
- `logs/` (`build.ps1`'s MSBuild log) and `libraries/build-*-arm64.log`.

Each installer is about 100 MB, because it carries its .NET Desktop Runtime. Two
per protected pipeline, kept for a week, count against the project's storage
quota on GitLab.com.

**Cost:** nothing in GitLab compute minutes. Both jobs run on bench150, the
project's own runner. No hosted runner is used, and GitLab has no hosted Windows
ARM runner.

The update checker is unchanged. A Windows server looks for
`hMailServer-<version>-x64.exe`, or `-arm64.exe` on ARM64 (`UpdateChecker.cpp`),
in a release. These jobs make exactly those names for a tag. The distro-specific
Linux names of the other package jobs are not something the update checker
knows about. Making it distro-aware is a later row.

The ARM64 toolchain
-------------------

`build/ci/packages/check-arm64-toolchain.ps1` only reads the machine. It runs
before anything is built, and anyone can run it by hand on a bench.

It asks vswhere for a Visual Studio 2026 instance (`[18.0,19.0)`, not a
prerelease, the same query `build.ps1` and `libraries\build-common.ps1` make)
that has all four of these components:

- `Microsoft.Component.MSBuild`
- `VC.Tools.x86.x64`
- `VC.Tools.ARM64`
- `VC.ATL.ARM64`

It then checks for these files in that instance's default toolset:

- `bin\Hostx64\arm64\cl.exe` and `link.exe`
- `atlmfc\lib\arm64\atls.lib`
- `lib\arm64\msvcrt.lib`
- `vcvarsamd64_arm64.bat`
- the ARM64 `Microsoft.VC*.CRT` redistributable

In the newest Windows SDK, it checks for `um\arm64\kernel32.lib` and
`ucrt\arm64\ucrt.lib`.

MFC for ARM64 is reported but not required: no server project uses MFC.

When something is missing, the check names it and prints the command that mends
it. When a component is missing, that is the Installer's `modify` adding the
components the instance lacks, as in WindowsOnArm.md:

    "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vs_installer.exe" modify --installPath "<the instance with MSBuild>" --add Microsoft.VisualStudio.Component.VC.Tools.ARM64 --add Microsoft.VisualStudio.Component.VC.ATL.ARM64 --passive --norestart

When every component is installed and a file is not there, adding the same
components again changes nothing, and until 28 September 2026 that is what the
check printed. Now, for the ARM64 C++ runtime, it reads the instance's own
catalog (`%ProgramData%\Microsoft\VisualStudio\Packages\_Instances\<id>\catalog.json`)
for the component that installs the package `Microsoft.VisualCpp.CRT.Redist.ARM64`,
and prints the `modify --add` of that component when the instance lacks it; in
Build Tools 18.9's catalog it is `VC.Tools.ARM64` itself. When that component is
installed, or the catalog names none, and for any other missing file, it prints
the Installer's `repair --installPath "<the instance>" --passive --norestart`.
`check-arm64-toolchain.ps1 -SelfTest` holds this to catalogs and instance trees
made to order.

**Finding the runtime.** Until 28 September 2026 the check looked for it with
`Get-ChildItem -Path '<instance>\VC\Redist\MSVC\*\arm64' -Filter 'Microsoft.VC*.CRT'`.
With a wildcard in `-Path`, `Get-ChildItem` returns the matching `arm64` folders
themselves and filters their names, so the check found no runtime on any machine:
on the laptop, whose Build Tools 18.9 has
`VC\Redist\MSVC\14.51.36231\arm64\Microsoft.VC145.CRT`, it reported the
runtime missing, and it did the same on bench150 in pipeline 2887115157. It now
asks for `VC\Redist\MSVC\*\arm64\Microsoft.VC*.CRT` as one path, and on the
laptop finds that folder and exits 0.

If the SDK lacks its ARM64 libraries, the check instead names the SDK feature to
add: "Windows SDK for Desktop C++ ARM64 Apps".

The ARM64 libraries
-------------------

The ARM64 OpenSSL, Boost and libpq are built with `libraries\build-*.ps1
-Platform ARM64` into `C:\bench\hMailLibs-arm64` on the bench. That folder is a
cache kept between jobs, separate from the x64 libraries in `C:\Dev\hMailLibs`,
which the gate uses. The versions come from `build\Get-LibraryVersions.ps1`,
which reads the project file.

A library is rebuilt only when its tree in the cache is incomplete:

- OpenSSL: both `-arm64` DLLs, the import libraries and `ssl.h`;
- Boost: `version.hpp`, plus every `libboost_*-vc145-mt[-gd]-a64-*.lib` the
  server auto-links;
- libpq: `libpq.dll` and `libpq.lib`.

The cached DLLs are then checked to be ARM64.

OpenSSL publishes its release archives only on GitHub (`openssl.org/source`
redirects there), and this project's CI downloads nothing from GitHub.
`libraries/build-openssl.ps1` takes the archive from Debian's archive instead,
`https://deb.debian.org/debian/pool/main/o/openssl/`, or snapshot.debian.org once
a version has left the pool: the same bytes, held to the SHA-256 the script pins
(for 4.0.2, `736b4675...43a8`, measured against Debian's copy on 26 September
2026). The job still reads every string in the script's code (not its comments)
with PowerShell's parser, and stops with exit 7 if one names `github.com` or
`githubusercontent.com` while the ARM64 OpenSSL is not in the cache; with the
script as it is, none does, so the job builds the ARM64 OpenSSL itself. It can
also be built once by hand on bench150 into the cache, from an elevated prompt
with the ARM64 tools installed:

    mkdir C:\bench\hMailLibs-arm64
    set hMailServerLibs=C:\bench\hMailLibs-arm64
    powershell -NoProfile -ExecutionPolicy Bypass -File libraries\build-openssl.ps1 -Version 4.0.2 -Platform ARM64

That downloads from Debian too, and holds the archive to the pinned SHA-256.

Boost comes from archives.boost.io and PostgreSQL from ftp.postgresql.org. libpq
also needs Strawberry Perl, Meson, Ninja, `win_flex` and `win_bison` on the
bench's PATH, and `build-pgsql.ps1` names each one that is missing.

The check of what the installer carries
---------------------------------------

The ARM64 installer cannot be tested on any machine the project has:

- `hMailServerARM64.iss` sets `ArchitecturesAllowed=arm64`, so its Setup stops on
  an x64 Windows before it reads `/DIR`. Installing into a folder on bench150
  is not possible.
- GitLab has no hosted Windows ARM runner.
- An x64 machine cannot start an ARM64 service.

What can be checked on x64 is what the installer is made of.
`build/ci/packages/check-pe-machine.ps1 -Iss <script> -Expect x64|ARM64` reads
the installer script the way ISCC's preprocessor does:

- it follows `#define` (strings, `GetEnv`, `ReadIni`, `SourcePath`, `+`),
  `#ifdef`/`#ifndef`/`#if`/`#else`/`#elif`/`#endif`, `#include` and `#error`;
- it takes every `Source:` of every `[Files]` section that is compiled in, with
  its wildcard, `recursesubdirs` and `Excludes`;
- it reads the PE header of each file.

The rules:

- A native image must be `0x8664` (x64) or `0xAA64` (ARM64).
- A .NET assembly must be that processor or AnyCPU (`0x014C`, IL only, not
  32-bit-required).
- An x64 image with CHPE metadata is ARM64EC, and is not ARM64.
- Entries to `{tmp}` are not checked. They are the .NET runtime and SQL Server
  Compact installers, which Setup runs and which are held to their pins.

Both jobs run it after everything is built and before ISCC. A wrong file stops
the job with exit 6 and names the file.

Two findings from building this check, both against real files:

- **`vcruntime140_1.dll` in the arm64 folder of the MSVC 14.51.36231
  redistributable is ARM64EC, not ARM64.** Microsoft ships it there for x64 and
  ARM64EC processes. An ARM64 process never imports it, and
  `third-party-binaries.json` gathers it. The ARM64 job passes
  `-Allow vcruntime140_1.dll`, which names it in the output instead of failing.
- **`Interop.hMailServer.dll` for ARM64 is AnyCPU.** An ARM64 .NET process does
  not load an assembly marked x64, and the x64 wrapper is made with `TlbImp
  /machine:X64`. TlbImp has no ARM64 machine type, so `build/generate-com-wrapper.ps1`
  makes the ARM64 build's wrapper separately, in `Interop\ARM64`, with
  `/machine:Agnostic`, and leaves the x64 one as it was (`build/build-tools.ps1`
  takes the ARM64 one for the win-arm64 tools). `check-pe-machine.ps1` refuses
  an x64-marked assembly in the ARM64 installer, so a wrapper made the old way
  fails the job with exit 6.

Signing
-------

Signing stays in the release chain. The jobs here build unsigned installers for
every protected pipeline. Only a release's installers are signed, by
`release-authenticode`, once a maintainer starts the release. Wiring the ARM64
installer into a release is the integrator's job. It needs these changes:

1. `build/ci/release-chain.py`:
   - `expected_assets()` gains `names["windows-arm64"]`;
   - `sources()` maps it to `packages-windows/arm64/hMailServer-<version>-arm64.exe`
     (the x64 one stays `release-windows/`, from `windows-installer`).
2. `.gitlab-ci.yml`:
   - `release` needs `windows-packages-arm64` with `artifacts: true`;
   - `windows-packages-arm64` loses `allow_failure`;
   - `build/check-release-chain.py` currently reads `.gitlab-ci.yml` alone and
     has to follow the include.
3. `release-authenticode`: a second `windows-authenticode.ps1` call, after the
   x64 one:

       -Installer "release-assets\hMailServer-$version-arm64.exe" -OutDir release-authenticode-arm64

   It needs its own `-OutDir`, because the script empties its `-OutDir` first.
   The job exits with the first non-zero code of the two, and
   `release-authenticode-arm64/` joins its artifacts.
4. `build/ci/release-sign.sh`: copy the signed ARM64 installer over the unsigned
   one, as it does for x64, with the same "not byte for byte the unsigned one"
   check. Then cosign signs it with the rest.

For a tag, `windows-packages-x64` builds the same x64 installer as
`windows-installer`, which builds it in the bench's checkout after the gate.
The release still takes `windows-installer`'s. Whether to keep both, or to
retire `windows-installer` in favour of `windows-packages-x64` (and drop
`windows-packages-*` from tag pipelines, or the other way round), is the
integrator's and the owner's choice. Keeping both costs one more x64 build under
the lock in a release pipeline.

What the owner must do
----------------------

1. **The ARM64 toolchain on bench150.** Its Build Tools 18.10 instance has the
   ARM64 tools and ATL for ARM64: pipeline 2887115157's check found both
   components and every compiler and library file. It reported the ARM64 C++
   runtime missing, which was the check's own fault (*Finding the runtime*, in
   [The ARM64 toolchain](#the-arm64-toolchain)), so whether bench150 has it is
   for the next run of `windows-packages-arm64` to say. If that run names
   something, run, from an elevated prompt, the command it prints with the
   instance's path.
2. Land the GitLab sweep's `build-openssl.ps1` (Debian's archive), or put the
   ARM64 OpenSSL into `C:\bench\hMailLibs-arm64` by hand (see
   [The ARM64 libraries](#the-arm64-libraries)).
3. Make sure bench150 has what `build-pgsql.ps1` needs on the runner's (the
   service's) PATH: Strawberry Perl, Meson, Ninja, `win_flex` and `win_bison`.
   It has to be the machine PATH, because the runner is LocalSystem.
4. Inno Setup 6 installed for the machine: done on 26 September 2026, and
   `windows-packages-x64` passed with it in pipeline 2887115157. What is left
   of this item is to remove that job's `allow_failure: exit_codes: [2]`.
5. After the first ARM64 run, commit the `lockfiles/` from its artifact.
6. When one ARM64 run has passed, remove `allow_failure: true` from
   `windows-packages-arm64`.

What the first run must confirm
-------------------------------

These were assumptions, written from the code at 6399e08a5 before any run.
`batch41`'s pipelines have since shown the x64 ones ([What the pipelines
ran](#what-the-pipelines-ran)): the runner's checkout built the x64 server and
installer with the libraries of `C:\Dev\hMailLibs`, the files
`get-installer-binaries.ps1` places and MSBuild found as LocalSystem, the
whole job in under six minutes where an hour was estimated (whether the
server's 3 min 47 s reused objects of the checkout's earlier builds was not
looked at); and GitLab saw the script's own exit code, failing pipeline 2886955494 on the
x64 job's exit 4 while `allow_failure: exit_codes: [2]` stood. Everything ARM64
past the toolchain check is still as written:

- The runner checkout on bench150 builds the x64 server from clean with only
  `C:\Dev\hMailLibs` and `get-installer-binaries.ps1`. That includes the ADO
  type libraries in `libraries\msado28`, which that script copies from Windows.
  `windows-gate` builds in a checkout that has had them for weeks.
- `powershell.exe build.ps1`, as `windows-installer.ps1` calls it, finds MSBuild
  as LocalSystem. `Find-MsBuild` asks vswhere without `-prerelease`, so a Visual
  Studio Insiders instance is not used.
- `build.ps1 -Platform ARM64`, `build-tools.ps1 -Platform ARM64` and
  `hMailServerARM64.iss` have never been run anywhere (WindowsOnArm.md).
  Neither have the three `-Platform ARM64` library builds, including libpq's
  Meson cross file.
- The win-arm64 runtime packs are restored from nuget.org, and the ARM64
  publish creates its lock files without failing the restore.
- The x64 build takes about an hour in a clean checkout. The ARM64 libraries add
  about an hour the first time (WindowsOnArm.md's estimates: 15 to 25, 5 to 15
  and 3 to 10 minutes). Both are estimates.
- The job timeouts (4 hours, up to 90 minutes of it waiting for the lock) fit the
  ARM64 job's first run, which builds its libraries too.
- `allow_failure: exit_codes: [2]` sees the script's own exit code. The job's
  script ends with `exit $code` under the runner's pwsh, and GitLab documents
  exit codes for shell executors, but no job of this project has relied on it yet.
- `GIT_CLEAN_FLAGS` keeps the two .NET runtime installers between jobs. It was
  checked with git itself, not with the runner.
- No file in the x64 or ARM64 set other than `vcruntime140_1.dll` needs
  `-Allow`. The x64 set of the laptop's existing build passed (below). The
  ARM64 publish's NuGet dependencies, for example SqlClient's SNI, have not
  been seen.

What was run, and the result
----------------------------

On the maintainer's laptop, 26 September 2026. Nothing was built:

- Every new script parses with PowerShell's parser (`Parser::ParseFile`, 0
  errors).
- `check-pe-machine.ps1 -SelfTest` passes every case. It uses PE images made
  to order: native ARM64, x64 and x86; AnyCPU, 32-bit-preferred, x86-only, x64
  and ARM64 assemblies; ARM64EC; not-PE files. It also uses a made-up installer
  script with `#ifdef`/`#ifndef`/`#else`, `#include`, `ReadIni(SourcePath ...)`,
  `GetEnv`, `{#...}`, wildcards, `recursesubdirs`, `Excludes`, `{tmp}` and
  `skipifsourcedoesntexist`, and checks that `#error`, a `#define` it cannot
  evaluate, and an unclosed `#if` stop it.
- `check-pe-machine.ps1` on real files:
  - Visual Studio's arm64 CRT redistributable: 9 ARM64 and
    `vcruntime140_1.dll` ARM64EC, the finding above;
  - `kernel32.dll` and `ntdll.dll`: x64;
  - the x64 OpenSSL: x64;
  - Newtonsoft.Json: AnyCPU;
  - the laptop's `Interop.hMailServer.dll` and `hMailCP.dll`: x64 .NET.
- `check-pe-machine.ps1 -Iss hMailServer64.iss -Expect x64`, reading the files
  of the laptop's existing x64 build: 80 PE images, 80 right, 0 wrong. The
  `{tmp}` runtime and SQL Server Compact were skipped.
- `-List` of both installer scripts: `hMailServerARM64.iss` resolves to 26
  entries, without the x64 7-Zip and MySQL client. `hMailServer64.iss` resolves
  to 29.
- `check-arm64-toolchain.ps1` on the laptop: exit 2. It reported:
  - Build Tools 18.9 has MSBuild and the x64 compilers but neither ARM64
    component;
  - the Insiders instance is a prerelease and is not used;
  - the fix was printed with the Build Tools path.
- `windows-packages.ps1 -Platform ARM64` on the laptop, from this worktree:
  - steps 1 to 3 passed (the checkout, version 6.3.4 with the branch artifact
    name `hMailServer-6.3.4-6399e08a-arm64.exe`, ISCC found through
    `HM_ISCC`);
  - it stopped at step 4 with exit 2 and the toolchain report, before cleaning
    or building anything.
- `.gitlab-ci.yml` with the include was loaded, and `extends` resolved as
  GitLab does, by a local script. Both jobs came out with bench150's tag, the
  `.own-runners` rules, their variables and artifacts. GitLab's own CI lint was
  not run.

Not run then: either job on bench150, any build, ISCC, and the libraries.

The adversarial check of the same day re-ran, on the laptop again: the parser over
the three scripts (0 errors); `check-pe-machine.ps1 -SelfTest` (every case
passed); `-List` of both installer scripts (26 and 29 entries, as above);
`check-arm64-toolchain.ps1` (exit 2, the same report); and
`windows-packages.ps1 -Platform ARM64` up to its toolchain stop (exit 2). It also
ran every checker of the `checkers` job over the tree. `check-help-map.py` and
`generate-help-pages.py --check` failed on this document, which no surface named.
It is now listed as reference-only in `HelpMap.cs` and `HelpPagesData.cpp` is
regenerated, and every checker passes. It found and fixed five things in the jobs:
the 5-hour timeout above the runners' 4-hour maximum; the start beside
`windows-gate`, which would have made the gate lose the lock; the two builds side
by side, where the x64 one could lose the lock to the ARM64 one; the x64 job failing
every pipeline while Inno Setup is missing; and an OpenSSL check that would have
gone on refusing after the sweep, because it only understood a quoted `$tarUrl`
assignment. The CI file was loaded again with the include and `extends` resolved.
GitLab's CI lint was still not run, and neither job had run on bench150 then.

What the pipelines ran
----------------------

`batch41`'s pipelines, on bench150, after `windows-gate` and `windows-dotnet`:

- **2886955494 (`4a0e53cc8`, 27 September 2026).** `windows-packages-x64`
  built the server, the tools and the Control Panel, and ISCC made the
  installer (Inno Setup found at `C:\Program Files (x86)\Inno Setup 6`); then
  the job's own check stopped it with exit 4: "the installer's version
  resource says 6.3.5.0 [followed by spaces], not section_setup_64.iss's
  6.3.5.0". The fault was the check's reading of the version, and
  `70eb52b94` makes it read the version as the server does. The pipeline
  failed on it, since exit 4 is not the allowed 2, and `windows-packages-arm64`,
  which needs the x64 job, was skipped. 339 s.
- **2887115157 (`70eb52b94`, 27 September 2026).** `windows-packages-x64`
  passed in 338 s: `hMailServer-6.3.5-70eb52b9-x64.exe`, unsigned by design,
  SHA-256 `b3a4b36f3bbba4b9d7118df3e321be525effc5f9d8618aaec56fd774e61678e1`.
  `windows-packages-arm64` stopped after 17 s at its toolchain check, exit 2,
  allowed to fail: Build Tools 18.10.12210.168 with MSBuild, the x64 and ARM64
  tools and ATL for ARM64, toolset 14.51.36231 with every compiler and library
  file, and Windows SDK 10.0.26100.0 with its ARM64 libraries - and the ARM64
  C++ runtime reported missing, which the check could not have found anywhere
  ([The ARM64 toolchain](#the-arm64-toolchain)).
- **2887524317 (`63d58f8a6`).** Both jobs were waiting for `windows-gate`
  when this was written, on 28 September 2026 at 00:40 UTC.

No ARM64 library, server or installer has been built by a pipeline yet.
