# Contributing to hMailServer

hMailServer is a free, open-source mail server: SMTP, IMAP and POP3, with a webmail, a REST API, the desktop
Control Panel and the browser Control Deck. It runs on Windows and, since 6.3.0, on Linux for x86-64 and
AArch64. This repository is a maintained fork of the original project, which went quiet for years: Progressive
Robot Ltd forked it in 2026 and released 6.0.0 on 12 June 2026, and the original project has since resumed
development separately.
It is maintained by Christopher Holloway / [Progressive Robot Ltd](https://www.progressiverobot.com) and
licensed under the GNU Affero General Public License, version 3 or later.

This file is for anyone who wants to change it: how to report something, how to build and test it, what every
change is held to, and how a change reaches `master`. By taking part you agree to abide by the
[Code of Conduct](CODE_OF_CONDUCT.md).

**The project's home is GitLab since 22 September 2026, in the group `hmailserver` since 29 September
2026: <https://gitlab.com/hmailserver/hmailserver>. Since 18 October 2026 it is published on GitHub again, as
a second home, under the account `RainbowWizard-coder`: <https://github.com/RainbowWizard-coder/hMailServer>.
GitLab is canonical:** the pipelines run there and nowhere else, a release is built, signed and published there
first and then copied to GitHub, and a change lands on `master` only through a GitLab merge request
([GOVERNANCE.md](GOVERNANCE.md)). GitHub was unavailable to the project from 18 September 2026, when it
suspended the account `Progressiverobot`, until 18 October 2026; that account stays suspended, so every link to
github.com/Progressiverobot - in older notes, commit messages and documents - still answers 404. **GitLab CI
runs the platform-neutral checks** (`.gitlab-ci.yml`) on every push and
every merge request: the Python checkers, the generators' check modes, the Deck's and the webmail's harnesses,
editorconfig, the packaging and package-manager checks when their files change, and the sign-off of every
commit a merge request or a `batch*` branch adds. **The builds, both regression suites, the .NET build and tests, the packages, the
schema-upgrade gate and the rest of what the old GitHub workflows ran happen on the project's own runners,
for `master`, `batch*` branches and `v*` tags only** - never for a merge request, and never for a fork
([hmailserver/docs/ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md)). So for your branch
they are still run by hand, by you before you open a merge request or a pull request, and so are two PowerShell
checks everywhere. [The checkers](#the-checkers) says which is which. No pipeline of the project's runs on
GitHub. GitHub Actions runs two workflows of the project's there: `.github/workflows/scorecard.yml`, which scores
the repository with OpenSSF Scorecard and publishes the result, and `.github/workflows/codeql.yml`, which analyses
the code with CodeQL on each push to `master`, on each pull request and weekly, and whose findings the maintainers
read; neither builds, tests or releases anything.

Contents:

* [Where things are](#where-things-are)
* [Reporting, asking and proposing](#reporting-asking-and-proposing)
* [Issue numbers from GitHub and from GitLab](#issue-numbers-from-github-and-from-gitlab)
* [Prerequisites](#prerequisites)
* [Building](#building)
* [Testing](#testing)
* [The checkers](#the-checkers)
* [Code style and licence headers](#code-style-and-licence-headers)
* [Change checklists](#change-checklists)
* [Commits](#commits)
* [Secrets](#secrets)
* [Sign-off](#sign-off)
* [Merge requests](#merge-requests)
* [Governance, conduct and licence](#governance-conduct-and-licence)
* [Architecture](#architecture)
* [Looking at the two browser pages](#looking-at-the-two-browser-pages)

## Where things are

| What | Where |
|---|---|
| The project | <https://gitlab.com/hmailserver/hmailserver> |
| The second home, on GitHub since 18 October 2026 | <https://github.com/RainbowWizard-coder/hMailServer>, under the account `RainbowWizard-coder`, GitLab's mirror: the repository, every release, the labels and milestones, the wiki's copy, pull requests and Discussions, and no Issues. GitLab stays canonical and the one place the project is worked on, and no pipeline of the project's runs on GitHub |
| Issues | <https://gitlab.com/hmailserver/hmailserver/-/issues>, and nowhere else: the repository on GitHub has no Issues |
| Merge requests | <https://gitlab.com/hmailserver/hmailserver/-/merge_requests>. A pull request at <https://github.com/RainbowWizard-coder/hMailServer/pulls> is accepted too, and landed through a GitLab merge request ([Merge requests](#merge-requests)) |
| Releases | <https://gitlab.com/hmailserver/hmailserver/-/releases>; the latest is always at <https://gitlab.com/hmailserver/hmailserver/-/releases/permalink/latest>. Each is published there first and then copied, file for file, to <https://github.com/RainbowWizard-coder/hMailServer/releases>, where the copies of 6.3.6 and 6.3.7, and every release published after them, are immutable |
| The wiki | <https://gitlab.com/hmailserver/hmailserver/-/wikis/home>; the same pages at <https://github.com/RainbowWizard-coder/hMailServer/wiki> since 18 October 2026, pushed from GitLab's wiki, which is the one that is edited |
| The issue board | <https://gitlab.com/hmailserver/hmailserver/-/boards> |
| The forum | <https://www.hmailserver.co.uk/> |
| Discussions | <https://github.com/RainbowWizard-coder/hMailServer/discussions>: the announcements, and questions in Q&A. GitLab has no equivalent: there a question is an issue with the `question` label |
| A security problem | [SECURITY.md](SECURITY.md). Never a public issue. |
| Copies off GitLab | The repository on GitHub, above - `master`, the `batch*` branches and every tag - pushed there by GitLab's push mirror after every push since 18 October 2026; and the issues, merge requests, releases and wiki by the weekly `forge-export` job, still to be set up: [hmailserver/docs/Mirroring.md](hmailserver/docs/Mirroring.md), which has the owner's checklist for both |

The latest release is **6.3.7, published on 17 October 2026**. Anything this repository describes that
arrived after it is in the tree after 6.3.7 and not yet released.

The documents at the repository root:

| Document | What it is for |
|---|---|
| [README.md](README.md) | What the server does, installing it, and the library build notes in full |
| [ARCHITECTURE.md](ARCHITECTURE.md) | The map of the code: where a change goes, and the constraints that have caused real bugs here |
| [RELEASE.md](RELEASE.md) | The release gates, in order, and why each one exists |
| [Roadmap2.md](Roadmap2.md) | The plan: the rows being worked through now |
| [Roadmap3.md](Roadmap3.md) | The plan, row by row, for bringing back on GitLab everything the project had on GitHub, and for what it never had there: the ARM64 installer, the apt and dnf repositories, winget, Chocolatey and every other channel people install mail servers from |
| [Roadmap.md](Roadmap.md) | The record: what exists, what was deliberately not done, and why |
| [GOVERNANCE.md](GOVERNANCE.md) | Who decides what |
| [ASSURANCE-CASE.md](ASSURANCE-CASE.md) | What the project claims about its own security, and the evidence |
| [SECURITY.md](SECURITY.md), [SUPPORT.md](SUPPORT.md), [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) | Reporting a vulnerability, getting help, and how people are expected to behave |
| [hmailserver/docs/](hmailserver/docs/README.md) | Operator documentation, indexed by its README. Every document there but the index, `DeckParity.md`, `WebAuthorisationMatrix.md` and `WebThreatModel.md` is also embedded in the server as a help page |

The tree:

| Path | What is in it |
|---|---|
| `hmailserver/source/Server/` | The C++ server: `hMailServer/hMailServer.sln` on Windows, `CMakeLists.txt` on Linux |
| `hmailserver/source/Tools/` | The .NET 10 tools: `hMailServer Tools.sln` (DBSetup, DBSetupQuick, DBUpdater, DataDirectorySynchronizer, ImportTool), `ControlPanel.sln` (the desktop Control Panel) and `Cli/` (`hmctl` and the `HmailServer` PowerShell module) |
| `hmailserver/source/DBScripts/` | The create scripts and the upgrade chain, per database backend |
| `hmailserver/test/` | `RegressionTests` (the Windows suite) and `LinuxRegressionTests` (the part of it that runs against a Linux server) |
| `hmailserver/installation/` | The Inno Setup installer, and the Control Deck (`WebAdmin/`) |
| `hmailserver/docs/` | Operator documentation |
| `build/` | The build, test and check scripts |
| `libraries/` | The scripts that build OpenSSL, Boost and libpq, and vendored third-party code |
| `fuzz/` | The libFuzzer harnesses |
| `.gitlab/` | The issue and merge-request templates GitLab offers, and three files that are not GitLab's: `allowed_signers`, the SSH keys release tags are signed with; `hmailserver.openvex.json`, the project's VEX document; and `upstream-sync`, the upstream hMailServer commit this tree was last compared against |
| `.github/` | The files GitHub reads for the copy there and GitLab does not: `PULL_REQUEST_TEMPLATE.md`, which fills a new pull request's description with the merge-request template's checklist; `CODEOWNERS`, which names the code owner a pull request there asks for review (`.gitlab/CODEOWNERS` is GitLab's); and the two workflows of the project's that GitHub Actions runs, `workflows/scorecard.yml` and `workflows/codeql.yml` ([OpenSSF Scorecard](hmailserver/docs/ContinuousIntegration.md#openssf-scorecard)) |
| `.gitlab-ci.yml` | The checks GitLab CI runs on a push and a merge request ([the checkers](#the-checkers)) |
| `renovate.json` | What Renovate, the project's dependency-update bot, keeps current, and how ([Dependency updates](hmailserver/docs/ContinuousIntegration.md#dependency-updates)) |

## Reporting, asking and proposing

An issue goes to GitLab, which needs a GitLab account. GitLab is the one place the project is worked on, so an
issue is opened there and nowhere else: the repository on GitHub, its mirror, has no Issues. Only a security
report can also go by email, as below. A question is an issue with the `question` label, or a thread in the Q&A
of GitHub's Discussions; GitLab has no equivalent of Discussions. The templates are GitLab's: each kind of issue
has one, chosen from the description-template list when you open the issue there.

* **A defect**: the **Bug** template. The log matters more than anything else in the report;
  [SUPPORT.md](SUPPORT.md) says which log and how to get it.
* **A question**: the **Question** template, GitHub Discussions at
  <https://github.com/RainbowWizard-coder/hMailServer/discussions>, or the forum at <https://www.hmailserver.co.uk/>,
  where this fork's releases are announced. General hMailServer questions that are not about this fork are often
  better answered on the long-running [hMailServer forum](https://www.hmailserver.com/forum/).
* **A feature**: the **Feature** template. Read [Roadmap2.md](Roadmap2.md) first, and for a package, an image
  or a place to install from, [Roadmap3.md](Roadmap3.md): it may already be a row, and
  [Roadmap.md](Roadmap.md) records what was deliberately not done, with the reason. A concrete problem with a
  log beats a list of wishes.
* **A vulnerability**: never an issue anyone can read. Open a
  [confidential issue](https://gitlab.com/hmailserver/hmailserver/-/issues/new?issue[confidential]=true)
  (the link ticks *This issue is confidential* for you), or email the project's Service Desk address,
  `contact-project+hmailserver-hmailserver-86730559-issue-@incoming.gitlab.com`, which files the mail as a
  confidential issue; or, since 18 October 2026, GitHub's private vulnerability reporting on the repository,
  <https://github.com/RainbowWizard-coder/hMailServer/security/advisories/new>, which the maintainers read too.
  [SECURITY.md](SECURITY.md) has the rest.

The labels:

| Label | Meaning |
|---|---|
| `bug` | A defect |
| `enhancement` | A new capability, or a change to one |
| `question` | A question rather than a defect |
| `documentation` | The documents, the wiki or the help pages |
| `security` | A security report. Always on a confidential issue |
| `good first issue` | A bounded piece of work for a first contribution |
| `upstream` | Concerns the original hMailServer project, or a change ported from it |
| `dependencies` | A third-party library, package or tool |
| `needs-info` | Waiting for the reporter |

The templates add their label with a quick action. GitLab runs a quick action only for someone who is allowed
to do what it does, so if the label does not appear on your issue, the maintainer adds it.

**Small tasks.** The issues labelled
[`good first issue`](https://gitlab.com/hmailserver/hmailserver/-/issues/?label_name%5B%5D=good%20first%20issue)
on GitLab are real, bounded pieces of work, each of which closes a gap recorded in the roadmap; none is make-work. Each
says what it is for, where to look, how to check it, and what "done" means. Comment on one to claim it. If it
turns out to be larger than it looked, say so on the issue rather than growing the change. The ones filed under
that label on the former GitHub project, under `Progressiverobot`, are still out of reach - that account's issues
are gone - so if the list is empty, ask on an issue.

## Issue numbers from GitHub and from GitLab

Every issue, pull-request and discussion number written in this repository before 24 September 2026 - in
commit messages, release notes, `Roadmap.md`, code comments and documents - is a number on the former GitHub
project, or, where the text says so, on upstream hMailServer's. GitLab numbers its issues from 1 again, and it turns `#6` in a commit message, an issue, a merge
request or a Markdown file into a link to GitLab issue 6, which is something else. The repository on GitHub since
18 October 2026, under `RainbowWizard-coder`, numbers its pull requests and discussions from 1 again too, in one
sequence (it has no Issues), and GitHub turns `#6` in a commit message, a pull request or a discussion it shows
into a link to that repository's pull request or discussion 6, a third thing. So, in anything you write:

* Write a GitHub number in words: "GitHub issue 158", "GitHub PR 264", "GitHub discussion 18"; written so, it is
  the former project's. A number of the repository under `RainbowWizard-coder` names the account: "GitHub
  PR 3 (RainbowWizard-coder)", "GitHub discussion 2 (RainbowWizard-coder)". Upstream hMailServer's numbers are
  "upstream issue 592" and "upstream PR 441".
* Write `#N` only for an issue opened on GitLab, and `!N` only for a GitLab merge request.
* Put a username from the forum, or from the other forge, in backticks. Written bare, `@name` links to whichever
  user of the forge showing it owns that name - GitLab's or GitHub's - and in an issue, a merge request, a pull
  request or a comment it notifies them; a file of this repository is shown on both, so in one put every
  username in backticks.

Existing text that writes a GitHub number with a hash is history. Leave it as it is, and do not turn it into a
GitLab link, nor into a link to the repository under `RainbowWizard-coder`, whose pull request or discussion of
that number is something else.

## Prerequisites

### Windows

| What | Notes |
|---|---|
| 64-bit Windows | Windows 10 or 11, or Windows Server |
| Visual Studio 2026, or its Build Tools | Platform toolset **v145** (`hMailServer.vcxproj`); the 2022 tools will not do. Workloads: .NET desktop development and Desktop development with C++. Components: C++ ATL for the v145 build tools, and a Windows 10 or 11 SDK. [README.md](README.md#installing-visual-studio-2026) has a `winget` line |
| The .NET 10 SDK | `global.json` pins 10.0.303 and rolls forward within .NET 10 only |
| The .NET Framework 4.8.1 developer pack | The Windows regression suite targets `net481` |
| Git, and both PowerShells | PowerShell 7 (`pwsh`) and Windows PowerShell 5.1 (`powershell.exe`). A few scripts work under one and not the other; [the checkers](#the-checkers) say which |
| The external libraries | OpenSSL 4.0.3, Boost 1.92.0 and PostgreSQL 18.6's libpq, as `hMailServer.vcxproj` pins them (`build\Get-LibraryVersions.ps1` prints the pins), built under a folder named by the `hMailServerLibs` environment variable. Building them needs Perl on `PATH` (OpenSSL and PostgreSQL), and for PostgreSQL also Python with Meson and Ninja (`py -m pip install meson ninja`) and winflexbison on `PATH`. No NASM |
| Python 3 | Every checker |
| Node 20 or later | The Deck's and the webmail's script harnesses and the S/MIME and OpenPGP checks. `openssl` on `PATH` for the S/MIME check; `gpg` is optional for the OpenPGP one |
| Inno Setup 6 | Only for the installer |
| The Clang tools for Windows | A Visual Studio component. Only for the fuzz build |

**Do not install a released hMailServer on the machine you build on.** The installer takes over the service
path, the COM registration and the `InstallLocation` registry value, which is exactly the state a development
checkout has to control. The regression bench runs the service from the repository's own build
([hmailserver/docs/RegressionEnvironment.md](hmailserver/docs/RegressionEnvironment.md), step 1).

**Line endings.** The tree checks out CRLF on purpose (`.gitattributes`: `* text=auto eol=crlf`), except the
Linux packaging, `build/*.sh` and `hmctl`, which are LF. The database scripts are split on a blank line in
CRLF, the DKIM-signed `.eml` fixtures carry body hashes, and the raw-message tests send file content verbatim,
so a tool that normalises the tree to LF breaks all three.

**The installer's fetched inputs.** Eighteen files are fetched rather than kept in git: the ten
third-party files the installer carries (7-Zip's `7za.exe`, the MariaDB client and its seven plugins, and the
SQL Server Compact 4.0 runtime), and a second copy of the MariaDB client and its plugins under
`libraries\mariadb-connector-c-3.4.11\`, which the post-build step copies beside the server.
`build\get-installer-binaries.ps1` downloads them from the project's `build-inputs-<n>` releases, whose files are
in the project's package registry on GitLab (the MariaDB client from `build-inputs-2`, built from MariaDB's
source by `libraries\build-mariadb-connector-c.ps1`; the SQL Server Compact runtime from `build-inputs-1`, the
same bytes GitHub served until 18 September 2026), and 7-Zip's console from 7-zip.org, and checks each against
the SHA-256 in `hmailserver/docs/third-party-binaries.json`
([ThirdPartyBinaries.md](hmailserver/docs/ThirdPartyBinaries.md#where-the-build-gets-them)). The server, the
tools, the Control Panel and the test assembly compile without them; a regression bench and the installer need
them.

No build step downloads from GitHub - the owner's rule since GitHub suspended the project's account on
18 September 2026, kept unchanged since the project's return there on 18 October 2026: the project builds from
GitLab alone, so that no forge's suspension can stop a build again, and `.no-github` keeps GitHub out of every CI
job (`.gitlab/ci/no-github.yml`).
OpenSSL publishes its release archives only there, so `libraries\build-openssl.ps1` takes the same archive from
Debian's archive, which ships it unchanged as `openssl_<version>.orig.tar.gz` - from its pool, or from
snapshot.debian.org, which the script looks up by itself once a version has left the pool - and holds it to
OpenSSL's own SHA-256 pinned in the script (`-Archive` takes a copy you have instead); the download is deleted
once it is extracted. `build-appimage.sh` no longer uses
linuxdeploy: the AppImage's runtime is the project's own, `platform/packaging/appimage/runtime.c`, built by
`build/ci/packages/appimage-runtime.sh` from Alpine's packages, and the script packs the AppDir with ldd, patchelf
and mksquashfs from the distribution. `build-pgsql.ps1` names
winflexbison's SourceForge page, and the fuzz build takes clang from the Visual Studio component.

### Linux

The Linux build is the server core only, from `hmailserver/source/Server/CMakeLists.txt`, which refuses to run
on Windows. It needs CMake 3.22 or later, a C++20 compiler (clang or GCC), Ninja, OpenSSL 3.0 or later, Boost
1.83 or later (thread, chrono and filesystem), libpq, zlib, iconv and OpenLDAP (`libldap` and `liblber`). On
Debian or Ubuntu, the packages the Linux workflow installed were:

```
build-essential cmake ninja-build clang libssl-dev zlib1g-dev libpq-dev libldap-dev
libboost-thread-dev libboost-chrono-dev libboost-system-dev libboost-filesystem-dev libboost-regex-dev
file rpm
```

## Building

Run every `.ps1` from the repository root with
`powershell.exe -NoProfile -ExecutionPolicy Bypass -File <script>` unless the table says otherwise. Each
component has one correct command:

| Component | Command | Notes |
|---|---|---|
| External libraries (once, and after a version bump) | `libraries\build-openssl.ps1 -Version 4.0.3`, then `libraries\build-boost.ps1 -Version 1.92.0`, then `libraries\build-pgsql.ps1 -Version 18.6` | In that order. Each script checks the SHA-256 of the source archive it downloads. The README's library sections say what they do by hand |
| The ADO type library (once per clone) | `build\get-installer-binaries.ps1 -SystemOnly` | Copies Windows' own `msado28.tlb` into `libraries\msado28\`, which the server's precompiled header `#import`s (`stdafx.h`) and git ignores. No download. **A clean clone's server does not compile without it** |
| The server | `build\build.ps1 -Configuration Release` | Debug is the default. MSBuild with `/m` for x64 and the pre- and post-build events off; the log is `logs\build-<configuration>.log`. It also copies the Control Deck page, its catalogues and `hMailServer.tlb` beside the executable, and generates the COM wrapper. The service runs the Release output, so stop it before building Release, and never build while the regression suite runs |
| The assertion build (never shipped) | `build\build.ps1 -Configuration Release -Asserts` | Keeps every `HM_ASSERT`; a violated one is reported in the ERROR log as HM6364. RELEASE.md step 8b runs the full suite on it |
| Static analysis | `build\analyze.ps1` | MSVC `/analyze`. It **replaces** the Release binaries, so build normally again before any test run |
| Post-build (runtime DLLs, COM and service registration) | `build\post-build.ps1 -Configuration Release` | Elevates through UAC. Points the service at your checkout's Release build |
| The COM wrapper (`Interop.hMailServer.dll`, not in git) | `build\generate-com-wrapper.ps1` | `-FromIdl` without a server build, `-Force` to remake it. `build.ps1` and `build-tools.ps1` run it themselves |
| A new COM interface or coclass | `build\register-com.ps1`, elevated | Needed only for a new interface. A new member of an existing interface needs nothing |
| Setup and migration tools | `build\build-tools.ps1` | Release by default. Publishes DBSetup, DBSetupQuick, DBUpdater, DataDirectorySynchronizer and ImportTool into each project's `publish\` folder. The CI equivalent was `dotnet restore "hmailserver/source/Tools/hMailServer Tools.sln" --locked-mode`, then `dotnet build` of the same with `-c Release --no-restore -warnaserror` |
| The Control Panel (its own solution) | `dotnet restore hmailserver/source/Tools/ControlPanel/ControlPanel.csproj --locked-mode`, `dotnet build hmailserver/source/Tools/ControlPanel/ControlPanel.csproj -c Release --no-restore -warnaserror`, `dotnet publish hmailserver/source/Tools/ControlPanel/ControlPanel.csproj -c Release -o publish --no-build` | `build-tools.ps1` does not build or publish it. The COM wrapper must exist first: build the server, or run `build\generate-com-wrapper.ps1 -FromIdl` |
| Control Panel unit tests (xUnit) | `dotnet test hmailserver/source/Tools/ControlPanel.Tests/ControlPanel.Tests.csproj -c Release` | |
| Import Tool tests | `dotnet test hmailserver/source/Tools/ImportTool.Tests/ImportTool.Tests.csproj -c Release` | |
| DBSetup tests | `dotnet test hmailserver/source/Tools/DBSetup.Tests/DBSetup.Tests.csproj -c Release` | How `DBSetup /unattended` reads its `HM_DB_` variables |
| The Control Panel's settings search index | `build/generate-settings-index.ps1` (either PowerShell), then commit `SettingsSearchIndex.g.cs` | After adding, removing or relabelling any setting |
| The Windows regression suite | `build\build-tests.ps1` | Debug by default. Restores through the parent `hmailserver\test\hMailServer Tests.sln` (legacy `packages.config`) and builds `RegressionTests.dll` for x64. Log: `logs\build-tests.log` |
| The installer | ISCC on `hmailserver\installation\hMailServer64.iss` | After `build.ps1 -Configuration Release`, `build-tools.ps1`, the Control Panel publish, `build\get-dotnet-runtime.ps1` and a full `build\get-installer-binaries.ps1` (see *The installer's fetched inputs* above). RELEASE.md steps 8 and 11 have the order |
| The Linux core | `cmake -S hmailserver/source/Server -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr`, then `cmake --build build/linux -j "$(nproc)"` | POSIX only. The configure line is the one the Linux workflow used before packaging |
| Linux packages | In `build/linux`: `cpack -G DEB` and `cpack -G RPM`. The AppImage: `bash hmailserver/source/Server/platform/packaging/build-appimage.sh --build-dir build/linux` | |
| The Linux translation-unit census | `bash build/linux-tu-census.sh -j "$(nproc)"` | Compiles every core source on its own; it must report no failures |
| The fuzz harnesses | `.\fuzz\build-fuzz.ps1`, then `.\fuzz\run-fuzz.ps1 -Target mime_message_fuzzer -Minutes 5` | clang-cl with libFuzzer and AddressSanitizer. It writes only under `fuzz\` and is safe beside a running suite. Read [hmailserver/docs/Fuzzing.md](hmailserver/docs/Fuzzing.md) before a long run |

The compiler runs with `/WX`: the server must build warning-free. The .NET projects set no
`TreatWarningsAsErrors` of their own, so build them with `-warnaserror` as the table does. Their NuGet lock
files are committed, so restore with `--locked-mode`.

## Testing

### The Windows regression suite

`hmailserver/test/RegressionTests` is NUnit against a **real hMailServer service on the same machine**, over
SMTP, IMAP and POP3 and through the COM API, with live ClamAV and SpamAssassin, real TLS, and only DNS faked (the
suite's own zone, `Shared/SuiteDns.cs`). It creates and deletes domains, accounts and mail as it goes.
**Never run it against a production installation**: it authenticates as `Administrator`, binds the live ports
and wipes data.

[hmailserver/docs/RegressionEnvironment.md](hmailserver/docs/RegressionEnvironment.md) is how to build the
bench: the service running from the repository's Release build, SQL Server Compact 4.0 and the bench database,
a short data folder (`C:\HMTest\Data`), one domain and the four standard ports, ClamAV's `clamd` on 3310 from
`C:\clamav`, SpamAssassin as a Windows service named `SpamAssassinJAM`, and the `sc sdset` grant that lets an
unelevated runner start and stop both services. Its step 10 lists what a new bench needs beyond that; where it
writes `installation\Extrasza.exe`, read `installation\Extras\7za.exe`.

The run, every time:

1. `wsl --shutdown` if WSL is running. With mirrored networking a socket bound inside WSL takes the port from
   Windows as well, invisibly, and outlives the process that bound it.
2. `build\preflight-tests.ps1`. It checks the service path, a stray `InstallLocation`, COM authentication, the
   domain and port counts, the schema version, a stale ERROR log, `clamd`, `SpamAssassinJAM`, the listeners, the
   twelve TLS fixture ports, WSL, the NUnit runner and the test DLL, orphan test files, leftover test-only
   settings and orphan domain directories, and names what is wrong. `-Clean` removes what an interrupted run
   left behind; restart the service afterwards. Go on only when it passes.
3. `build\run-tests.ps1`. A subset: `-Where "class =~ /RegressionTests.IMAP/"`. `-StopOnError` exists for
   chasing one defect; neither a subset nor a stopped run is a complete run.

What to know:

* **The bench database is upgraded, never recreated.** When a change moves `REQUIRED_DB_VERSION`, run
  `build\upgrade-test-database.ps1` **before** building the new server. It applies the pending upgrade scripts
  through the running service's COM; once the new binary is in place, the service refuses the old database and
  there is nothing left to talk to.
* **Do not `dotnet test` the Windows suite.** It exits 0 having run nothing.
* **Always x64.** An AnyCPU build of the same project also runs, against stale assemblies, and passes tests it
  never executed.
* **Never build, analyse or start other heavy work while the suite runs.** A rebuild restarts the service under
  the tests, and the run can then neither convict nor clear the code.
* NUnit writes its failure details only at the end of the run, so a quiet run is normal.
* **The environment traps** (RegressionEnvironment.md, last section): anti-virus products that proxy localhost
  mail (ESET answers an IMAP literal with `+ just send it`), VPN adapters that rewrite loopback, a tree
  normalised to LF, Mark-of-the-Web on downloaded files, and "passes alone, fails in a group", which means state
  shared between fixtures.
* A second bench can take half of the suite by namespace (`run-tests.ps1 -Where` with a class pattern). The two
  halves together are one run.

The release rule, in [RELEASE.md](RELEASE.md): the full suite, zero failures and zero inconclusive, on the exact
binary that ships.

### The Linux suite

`hmailserver/test/LinuxRegressionTests` is a `net10.0` project that compiles the Windows suite's fixture files
**in place**, against a fixture layer that talks REST instead of COM (`LinuxRegressionTests/Shims`). It edits
no fixture and weakens no assertion; what it cannot do is skipped with the reason. It runs against any server
with the REST listener on, and unlike the Windows project it is meant for `dotnet test`:

```
dotnet test hmailserver/test/LinuxRegressionTests -c Release --logger trx
```

The server's address and ports come from the environment: `HMTEST_HOST`, `HMTEST_SMTP_PORT` (2525),
`HMTEST_POP3_PORT` (1110), `HMTEST_IMAP_PORT` (1143), `HMTEST_SUBMISSION_PORT` (587), `HMTEST_REST_PORT`
(8045), `HMTEST_ADMIN_PASSWORD`, `HMTEST_SERVER_INI` and `HMTEST_NO_FAKE_DNS`. RegressionEnvironment.md,
section *On Linux*, has the recipe for the server: PostgreSQL, `--create-database`, the listeners moved off the
privileged ports, auto-ban off and `--set-admin-password` from a pipe. `build/ci/linux-suite.sh` is the same
recipe as the CI job runs it, whole suite included, and runs by hand in the job's image.

The schema-upgrade gate: `bash build/db-upgrade-gate.sh pgsql` (or `mysql`) makes a database with the create
script of schema 6029 itself (`build/db-upgrade-6029/`), seeds it with orphaned rows, upgrades it through the
real scripts on a real backend, and compares what it upgraded to with a new database made by `--create-database`:
a column, index or constraint an upgrade step makes differently from the create script fails it. It needs the
Linux `hmailserver` binary and a database server; the script's header lists the environment it reads.

### Writing a test

* **`RegressionTests.csproj` lists every file; there is no glob.** A test file that is not listed is invisible,
  and a green suite says nothing about it. The pre-flight names orphans. If the fixture compiles against the
  Linux shims, add it to `LinuxRegressionTests.csproj` as well.
* Fixtures derive from `TestFixtureBase`. Its setup checks that the service did not restart and runs
  `PerformBasicSetup`, which calls `AssertNoReportedError`, so **any entry in the ERROR log fails the next
  test**. A test that provokes an error asserts it with `CustomAsserts.AssertReportedError(...)`, which asserts
  and then deletes the log.
* **A fix ships with a test that fails against the build before it.** A test that passes both ways proves
  nothing: build the pre-fix binary and check.
* A test changes a `[Settings]` value through the settings store (`IniFileSetting.Write`, which calls
  `Settings.SetIniSetting` over COM), never by editing `hMailServer.ini`: `check-test-settings-writes.py` holds
  it.
* Undo in teardown whatever the fixture changed: ports, settings, a renamed domain. The pre-flight lists the
  leftovers it knows.
* **Trap:** `POP3ClientSimulator.AssertGetFirstMessageText` sends `RETR 1` and then `DELE 1`. The message is
  gone after reading, so a later count sees one fewer.

## The checkers

**What GitLab CI runs.** `.gitlab-ci.yml` runs on every push to a branch and on every merge request: on the
project's own runners for `master`, a `batch*` branch and a `v*` tag, and on GitLab's hosted runners for a merge
request and any other branch ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), "Every job on
the project's own runners"). *checkers* (a Python image with git) runs every Python checker below except
`check-package-manifests.py`, the `--self-test` modes (the release chain's, the release's security gate's, the
secret scan's, repo-hygiene's and `build/perf/webmail-timing.py`'s among them), `add-license-headers.py --check`, every
generator's `--check`, `generate-portal-languages.py` and `check-deck-parity.py` held to the files they write
staying as committed, `renumber-upgrade-items.py --check` (the newest *What changes on upgrade* list of
`CHANGELOG.md` numbered 1 to n and every reference inside its section naming its item), and
`check-linux-version-stamp.sh`, and it reports every failure at the end rather than
stopping at the first. *harnesses* (a Node image with Python, OpenSSL and GnuPG) runs `check-deck-script.py`,
`check-portal-script.py`, `check-portal-smime.js` and `check-portal-openpgp.js`. *editorconfig* runs
editorconfig-checker. Four more run when their files change: *chart-and-compose* (the Helm chart and the
Compose file), *update-helper* (`build/test-hmailserver-update.sh` and the units under `systemd-analyze`),
*package-templates* (`make-package-manifests.ps1 -Check` and `check-package-manifests.py --offline` on
placeholder values) and *roadmap* (`check-roadmap.ps1`). *sign-off* runs on every merge request and every
`batch*` branch (see [Sign-off](#sign-off)), and so do the change checks: GitLab's *semgrep-sast*, *semgrep-sast-gate*,
which holds a change on a Critical or High finding in what it touches, a baseline it grows, a suppression comment it
adds, a narrowing of what a scan reads that does not land alone, or a source file it puts out of the scan's reach, and
*mr-dependency-check*, which holds one that adds a dependency version or moves a library pin with a Critical or High
advisory or a malicious package, or adds a requirement that pins no version ([SECURITY.md](SECURITY.md), "Every
change"; [Change checks](hmailserver/docs/ContinuousIntegration.md#change-checks)). A finding that is not fixed passes
only through a declaration already on `master` that it is not exploitable, landed in a change of its own first - save
in a change that re-pins *semgrep-sast*'s analyzer, which may carry the exceptions for findings in files it does not
touch, since no change could land first while the analyzer it needs is gone; and a narrowing of what a scan reads is a
declaration too, landed alone. *checkers* runs its
self-test and `build/land-via-mr.py`'s, and those of
`build/generate-update-feed.py`, `build/ci/release-chain.py` and `build/ci/repo-hygiene.py` (Token-Permissions against
CI files made to break it). Each script is started as `python3 build/...`, `node build/...`, `bash build/...` or
`pwsh -File build/...`, never by its shebang, because the tree checks out CRLF.

**On the project's own runners**, for a push to `master`, to a `batch*` branch or of a `v*` tag, and for
nothing else. On the Linux runners, linix, server2 and s1-linux, which share the tag `linix`: *linux-build* (the server with clang, its tester programs, the OpenPGP
writer against GnuPG, and the `.deb` and the `.rpm`), *linux-suite* (the whole Linux suite against it on
PostgreSQL), *linux-cross* (every core file compiled for AArch64), *linux-sanitize* (`snmp-tests` and
`workqueue-teardown-tests` under AddressSanitizer, once the option exists), *linux-db-upgrade-pgsql* and *-mysql* (`build/db-upgrade-gate.sh`),
*linux-package-deb* and *-rpm* (the packages installed in clean containers, the `.deb` started and asked for
the Deck), *linux-image* (the container image built, not pushed), *linux-sbom*, *linux-repro* (the server and
the packages built a second time from another path and compared byte for byte), and six
security-scanning jobs: *static-analysis* (cppcheck and clang-tidy) and *semgrep*, which fail the pipeline on a
finding new to their baselines that `master`'s `.gitlab/security-exceptions.yml` does not accept, and
*dependency-scan* (Trivy and OSV, fed by *ci-image-packages*, its finding failing *dependency-scan-verdict*) and
*repo-hygiene*, which do not stop it; on a `batch*`
branch the scans judge everything the batch changes from `master`, not its last push. Three of them,
with GitLab's *semgrep-sast*, hold a release as well: the release job reads their reports and refuses while a
scan did not finish or found something `SECURITY.md`'s policy counts that no baseline and no entry of
`.gitlab/security-exceptions.yml` accepts ([Before a release](hmailserver/docs/ContinuousIntegration.md#before-a-release-the-scans-hold-it)). On the Windows benches:
*windows-gate* on bench150 and *windows-gate-s1vm* on S1-WINBENCH, the two halves of the Windows suite, run at once,
each in its bench's own checkout; and on bench150 *windows-dotnet* (the Control Panel and
the tools built with warnings as errors and tested, `generate-settings-index.ps1` held to its file, and
`dotnet format --verify-no-changes`). The package jobs of `.gitlab/ci/packages-deb.yml`, `-rpm.yml` and
`-appimage.yml` build, install and test each distribution's package and the AppImages on the same Linux runners. A `v*` tag's pipeline ends in the release jobs, for 6.3.5 onwards, which the owner starts
([RELEASE.md](RELEASE.md), "The release on GitLab"), and a `batch*` branch's rehearses them and publishes
nothing (`.gitlab/ci/release-rehearsal.yml`). A pipeline schedule runs *linux-fuzz*, the MIME harnesses under
libFuzzer, every night. The project's runners never take a job for a merge request, above all not one from a fork, nor for a push
that changes nothing but Markdown files at the root or under `.gitlab/`;
[hmailserver/docs/ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md) says why, what each old
GitHub workflow became, where every tool comes from, and how the runners, their image and the bench's gate
lock work. The pipeline of the schedule described `renovate` runs *renovate* on a Linux runner and nothing else:
Renovate opens a merge request a week per ecosystem, and one at its next daily run for a NuGet package with a
published advisory, from a `renovate/*` branch. Such a merge request is reviewed and landed like yours, after the
step its description names ([Dependency updates](hmailserver/docs/ContinuousIntegration.md#dependency-updates)).

**What stays by hand:** for any branch that is not protected, everything the project's own runners run - the
builds, both regression suites, the .NET build and tests with `dotnet format`, and the Linux jobs that apply to
the change; everywhere, `check-package-manifests.py` against a released installer and two PowerShell checks,
`check-schema-versions.ps1` and `check-db-scripts.ps1`. Run those from the repository root before you open a
merge request, and say in it which ones you ran; the ones CI runs are worth running first too, since they take
a few minutes here and a round trip there. They need nothing but Python unless their row says otherwise, and
ran on Linux under `python3` in the GitHub workflow `style.yml` (in the git history at `v6.3.3`) before they ran on GitLab.
**Rewrites** marks a mode that changes files.

### Python checkers

Run each as `python build/<name>.py` (`python3` on Linux).

| Checker | What it holds | Modes |
|---|---|---|
| `check-account-stores.py` | Every per-account table is in the backup, or says why not | |
| `check-address-parsing.py` | An IP address in text is read by `IPAddress::Parse` alone, never by Boost's `make_address`, whose Windows parser reads forms Linux does not (`192.0.2`, `0x7f.1`, `010.0.0.1`, a port, brackets), nor by `inet_addr` or `inet_aton`, which read the older forms on every platform | `--self-test` |
| `check-audit-choke-point.py` | Every administrative change reaches the audit trail, and no write route skips it | |
| `check-authz-choke-point.py` | Every folder-access decision goes through `ACLManager` | `--self-test` |
| `check-catalogues.py` | Control Panel translations keep the server's literal words, the page names and the numbers | |
| `check-citation.py` | `CITATION.cff`'s `version` and `date-released` are `Version.h`'s release and its `CHANGELOG.md` date, and the file says that this is Progressive Robot's fork, cites the original and claims no DOI; `cffconvert --validate -i CITATION.cff` checks it against the format's schema | `--self-test` |
| `check-cli.py` | `hmctl` and the PowerShell module against a recorded server | The PowerShell half skips itself when neither `pwsh` nor `powershell` is present |
| `check-cli-packages.py` | The two clients' packages: `HmailServer.psd1`'s version and function list against `Version.h` and the module, and `hmailserver-cli` built from `hmctl` with no dependency and no install-time code ([CommandLinePackages.md](hmailserver/docs/CommandLinePackages.md)) | `--self-test`; `--dist DIR` also checks a built sdist and wheel |
| `check-collection-iteration.py` | Every loop over a shared collection iterates a locked copy (`GetSnapshot()`) | `--self-test` |
| `check-com-dispids.py` | No two members of one COM interface share a dispatch id | |
| `check-com-error-info.py` | Every COM class that raises errors answers `ISupportErrorInfo` | `--self-test` |
| `check-com-shared-copies.py` | A COM change to an item of one of the running server's collections is written onto a copy, and every hand-out attaches the shared collection | `--self-test` |
| `check-control-bytes.py` | No tracked text file holds a control character other than tab, line feed, and a carriage return before a line feed: the character a tool wrote where it was given an escape (`\7`, `\x1b`, `\x01`) shows nowhere on the screen. The few files that hold one on purpose are listed in the script, each with its reason | Proves its own negative controls on every run |
| `check-db-settings-coverage.py` | Every database setting `SettingsDefaults.h` names has a Control Panel editor - through a COM property the Control Panel writes, or a REST settings row one of its pages names - or a recorded reason for having none | `--self-test`; `--list` prints how each setting is reached |
| `check-deck-languages.py` | The Control Deck's catalogues against its page | `--keys` |
| `check-deck-parity.py` | A census of the desktop Control Panel's writes against the REST API and the Deck | **Rewrites** `hmailserver/docs/DeckParity.md` by default; `--no-doc` prints only; `--strict` exits 1 when anything is missing. After a change, run it and commit `DeckParity.md` if it moved |
| `check-deck-script.py` | Runs the Deck's script against a small DOM and a recorded server | Needs `node`; `--extract-only` does without |
| `check-environment-names.py` | Every `HM_SETTING_` and `HM_PROPERTY_` name in the container files and documents is a real setting | |
| `check-forge-export.py` | The weekly `forge-export` job runs for the `export` schedule alone, on linix, with its artifact for Developers only and no token in any CI file, and `build/ci/forge-export.py` passes its self-test ([Mirroring.md](hmailserver/docs/Mirroring.md)) | Proves its own negative controls on every run |
| `check-gitleaksignore.py` | Each directory-form line of `.gitleaksignore` (`/builds/hmailserver/hmailserver/<path>:<rule>:<line>`, what a pipeline's scan of the checkout matches) names a finding of its rule at its line, and every finding of a file and rule those lines name has one, so that the change that moves a fixture moves its line; fails while `secret_detection` runs another analyzer than the one its patterns were read from (`RULES_FROM`); and fails on any line of the tree that carries gitleaks' inline allow marker, which that job takes as a waiver shown to no one ([What the secret scans read](hmailserver/docs/ContinuousIntegration.md#what-the-secret-scans-read)) | `--self-test` |
| `check-help-map.py` | Every page of the three surfaces has help, and every help entry names a real document and heading | `--self-test` |
| `check-idl-case.py` | `hMailServer.idl` spells every name one way whatever its case, but for the seven that shipped two ways, each with its reason: a type library keeps one spelling, so a new `Helo` renames `Client.HELO` for every early-bound client | `--self-test` |
| `check-image-pins.py` | Every image the CI files and the build scripts pin by digest is still served by its registry, asked as a runner asks it ([Image pins](hmailserver/docs/ContinuousIntegration.md#image-pins)) | Needs the network; `--list` asks nothing; `--self-test` asks a registry it serves on the loopback |
| `check-ini-coverage.py` | Every `[Settings]` key the server reads has a Control Panel editor or a recorded reason for having none, no new `[Settings]` key appears (see [a new setting](#a-new-setting-goes-in-the-database)), and no section beyond the three it records is read around `IniFileSettings` | **Rewrites** `build/ini-settings-baseline.txt` and `IniSettingsBaseline.h` with `--write-baseline`, for a key legitimately removed |
| `check-installer-scripts.py` | The Windows installer runs each uninstall step once, waits for the server's process before it replaces or deletes a file, marks every file that process loads `uninsrestartdelete`, and starts the service for an upgrade's database step with the argument the server reads; `hMailServer.exe /Unregister` creates nothing and removes the event source; setup refuses an installation of the original hMailServer's 5.7 first, by bounds the upgrade chain in `DBScripts` holds, and the Windows package jobs run that decision's self-test (`build/ci/packages/original-guard-test.ps1`) before they build the installer | Proves its own negative controls on every run |
| `check-localisation.py` | The Control Panel's catalogues agree with the translation marks, and the 17 complete languages are complete; a translation keeps the white space its English starts or ends with, where the source joins it to another text, and adds none; no sentence is split around a value with its end left a literal | **Rewrites** `Strings.resx` and every catalogue with `--write`; `--inventory` lists what is untranslated; `--self-test` |
| `check-mnemonics.py` | Every static Control Panel caption has an Alt-key mnemonic, with no clash on one screen | `--list` |
| `check-mysql-charset.py` | Every MySQL table is `utf8mb4` under `utf8mb4_general_ci`, InnoDB and DYNAMIC: every CREATE TABLE in the create script and in the upgrade scripts from 6102 on, the 6102 step's conversion of every table, the probe that opens each step from 6040 to 6102 making a utf8mb4 table (and those tables InnoDB and DYNAMIC), the index lengths at four bytes a character, and the table and database the server creates itself | `--self-test` |
| `check-openapi-blocks.py` | Exactly one comma between the OpenAPI path blocks | |
| `check-osv-ignores.py` | Each advisory an `osv-scanner.toml` ignores - the file OSV-Scanner reads beside a manifest when OpenSSF Scorecard's Vulnerabilities check runs it, `build/ci/osv-scanner.toml` for the semgrep job's `requirements.txt` - has a `not_affected` statement in `.gitlab/hmailserver.openvex.json` by its id or an alias, a version that statement covers still pinned by a requirements file beside it, a reason, and an `ignoreUntil` still to come; and the file holds nothing but `[[IgnoredVulns]]` entries ([OpenSSF Scorecard](hmailserver/docs/ContinuousIntegration.md#openssf-scorecard)) | `--self-test` |
| `check-package-licenses.py` | The licence fields of the Linux packages. The project's `.rpm` tag (`CPACK_RPM_PACKAGE_LICENSE`), the RPM spec's `License`, the APKBUILD's and the PKGBUILD's `license` and the Nix package's `meta.license` name the server's licence, that of every third-party component compiled into the server that an SPDX identifier names and, in the two RPM fields, Fedora's `LicenseRef-Fedora-UltraPermissive` for CStdString; nothing else: each component of `build/asset-sbom.py`'s `SOURCE_COMPONENTS` is decided there as compiled in or not, and `BSL-1.0` goes where that build links Boost statically. The snap's `license`, the container image's two licence labels and the Helm chart's `artifacthub.io/license` name the server's licence alone, each for the reason the script gives; no other licence field shares a field's line, and no other line of the files it reads - those of `snap/`, `nix/` and `platform/packaging/`, `flake.nix`, the server's `CMakeLists.txt` and `build/ci/linux-image.sh` - is a licence field in one of the forms the script lists for the kind of file, a CMake command on a CPack licence variable, an assignment to `license` or to an element of it and a YAML key in a block or in a flow among them | `--self-test`; the tree's run runs the self-test first |
| `check-package-manifests.py` | The winget and Chocolatey manifests against their schemas, that every text a user reads says the package is Progressive Robot's fork of hMailServer, and, with `--release-date`, that the manifest is dated by the release | Needs the `jsonschema` and `pyyaml` packages (pinned with their hashes in `build/requirements-dev.txt`) and manifests rendered by `build/make-package-manifests.ps1`, and checks that the installer URL is a release's file in the project's package registry on GitLab. It refuses a schema download that is on GitHub or redirects there: put a copy in the `.schema-cache` directory beside the manifests, or pass `--offline`, which skips the schema fetch and the URL check |
| `check-package-texts.py` | Every package text the tree ships says that it is Progressive Robot's fork of hMailServer: the RPM spec's and `debian/control`'s summaries and descriptions, CPack's, the PKGBUILD's and the APKBUILD's `pkgdesc`, the nix package's and the flake's descriptions, the snap's title, summary and description, the image's labels, the chart's description and the openings of its README and `ContainerImage.md`, the Chocolatey and winget copyright and authors, the AppImages' desktop entry (its Name and Comment), the SBOM descriptions of each release file and container image (`build/asset-sbom.py`) and the apt and dnf repositories' origin, label, description and name (`build/generate-package-repos.py`; the origin is Progressive Robot). The long descriptions (the RPM spec's and `debian/control`'s, CPack's, the nix package's `longDescription` and the snap's), the image's description label, the chart's description, the openings of its README and `ContainerImage.md`, and the Chocolatey and winget copyright and authors also credit the original's author, Martin Knafve. The summaries are within their tools' lengths, and the packages' maintainer is the project's | `--self-test` |
| `check-password-choice-sites.py` | Every place a password is chosen asks the policy and the breach check | `--self-test` |
| `check-portal-languages.py` | The webmail's catalogues against its page, and `PortalLanguagesData.cpp` equal to what the generator writes | `--keys`, `--print` |
| `check-portal-script.py` | Runs the webmail's script against a small DOM and a stubbed API | Needs `node`; `--no-node` runs the static checks only |
| `check-project-items.py` | No file listed twice in a project, and every listed file exists | |
| `check-roadmap2.py` | The counter line at the top of `Roadmap2.md` and of `Roadmap3.md` equals that file's rows' marks | **Rewrites** each counter with `--write`; `Roadmap3.md` (or `Roadmap2.md`) as an argument checks that file alone; `--self-test` |
| `check-release-chain.py` | The release jobs of `.gitlab-ci.yml`, their scripts and `SigstoreVerifier.cpp`'s GitLab identity agree: the ID tokens' audiences, cosign's flags, the order of the jobs, the two manual starts, the feed deployed in the tag's own pipeline after `release-publish` and no trigger of a project, every other job refusing a schedule's and a downstream pipeline, and the release links' paths; the release waits for the four security scans and reads their reports before it collects anything, and `.gitlab/security-exceptions.yml` is well formed and the owner's | `--self-test` |
| `check-role-choke-point.py` | `RoleManager` is the only decider of what a credential may do; every `RouteKind` is classified | `--self-test` |
| `check-sbom-inputs.py` | Every input the build pins - in `third-party-binaries.json`, `native-dependencies.ps1`, `get-installer-binaries.ps1` and `get-dotnet-runtime.ps1`, and every library `CMakeLists.txt` finds - is in the SBOM's component list, at its pinned version and with its pinned hash | `--self-test`; `--sbom FILE` (repeatable) holds a finished SPDX or CycloneDX document to it. Runs `native-dependencies.ps1` when `pwsh` or `powershell` is present, and reads its tables when neither is |
| `check-security-insights.py` | `SECURITY-INSIGHTS.yml`, the project's security facts in the OpenSSF Security Insights format 2.2.0, against that version's schema (every required key, no key it does not have, the types, the enumerations and the patterns), with `last-updated` and `last-reviewed` not after the latest calendar date anywhere (the UTC clock plus fourteen hours) and the review not more than a year before it (a review older than the last update passes: the file's header asks for `last-updated` to move with any change, and `last-reviewed` only when the whole file has been read against the tree again), the file's own address, the fork's name, the GitLab project and the GitHub copy, the licence and the vulnerability contact SECURITY.md gives; every URL anywhere in the file https, written in lower case; every URL on `gitlab.com` under `/hmailserver/` or on `github.com` under `/RainbowWizard-coder/` the project's own, its host taken without its case or a port; every link read as GitLab reads it, its host as a browser takes it, its path percent-decoded and its query left out, none on a GitLab route written without `/-/`, and none to the GitHub copy written with it; every link to a file or folder of the tree (blob, raw, tree or blame) at `master` and to a path the tree holds, in the case given, every link to a heading of a Markdown file one it has; every link under `/-/releases/<tag>/downloads/` - `v{version}`, `permalink/latest` or a version's tag - a name `build/ci/release-chain.py` gives a file of that release (`release_files` under `permalink_name`, or the file's own name under the tag of a release before 6.3.6); and every path it names outside a link, written from the top or from `./`, that has a path's look - its first part an entry at the tree's root, a `/` at its end or a file's extension: a script in a comment, a YAML comment, a file named with no folder such as `SECURITY.md` or `renovate.json`, which is a file at the root - and every ruleset a tool names, one the tree holds, in the case given, a folder where it ends in `/`, a name with spaces, a folder's or a file's, such as `hmailserver/test/SSL examples/` or `hmailserver/docs/Licenses/License list.md`, read whole whatever the number of its spaces, and no word of it read again as a name of its own, unless it is in `NOT_TREE` (Semgrep's registry rulesets, the specification's `spec/schema.cue`); a word such as S/MIME, and/or or amd64/arm64 is prose, and not held. It fetches nothing, and reads the file as a plain subset of YAML, with nothing but Python, and refuses what it does not take | Proves its own negative controls on every run; `--self-test` says each |
| `check-setting-levels.py` | A setting with domain or account levels is read only through `EffectiveSetting`, and its marks agree with `build/settings-levels.txt` | Proves its own negative controls on every run |
| `check-settings-rows.py` | Every database setting gets a row on a database made before it | |
| `check-source-literals.py` | A literal in the server's C++ holds the same characters on Windows as on Linux: no character beyond ASCII written raw in a literal of a file without a UTF-8 byte-order mark (MSVC reads one in the ANSI code page), nor in a narrow literal of a file with one | `--self-test` |
| `check-spacing.py` | The Control Panel's margins and paddings written as numbers, in XAML and in C#, are on the spacing scale (4, 8, 12, 16, 24 and 32 pixels, `docs/ControlPanelDesign.md`) or held by `build/spacing-baseline.txt`, the optical nudges and the rest the tree held when the check came in, which may only shrink | `--write` lowers the baseline; `--rebaseline` writes it from the tree, adding entries too, for the one who folds branches written before the check, who reads its diff; `--self-test` |
| `check-sql-parameters.py` | Each placeholder use has its own parameter, added in order | |
| `check-sql-separators.py` | Every MySQL statement ends with a blank line | **Rewrites** with `--fix` |
| `check-test-settings-writes.py` | Tests change `[Settings]` through the store, never by editing the file | |
| `check-upgrade-inserts.py` | An upgrade script inserts a setting row only if it is absent | |

### Generators with a check mode

A generator writes committed source from something else in the tree. Its `--check` fails when the committed
file differs from what it would write. After changing the input, run it without `--check` and commit its output.

| Generator | Writes | From |
|---|---|---|
| `python build/generate-help-pages.py [--check]` | `Common/Util/HelpPagesData.cpp` | `hmailserver/docs/*.md`, except `README.md`, `DeckParity.md`, `WebAuthorisationMatrix.md` and `WebThreatModel.md` |
| `python build/generate-portal-page.py [--check]` | `Common/Util/PortalPageData.cpp` | The webmail's `Portal.html`, `Portal.js`, `Portal.webmanifest`, `PortalServiceWorker.js` and `PortalMarkdown.js`, and the protected-message page `PortalProtected.html` and `PortalProtected.js` |
| `python build/generate-portal-smime.py [--check]` | `Common/Util/PortalSmimeData.cpp` | `PortalSmime.js` |
| `python build/generate-portal-openpgp.py [--check]` | `Common/Util/PortalOpenPgpData.cpp` | `PortalOpenPgp.js` |
| `python build/generate-portal-languages.py` | `Common/Util/PortalLanguagesData.cpp` | `PortalLanguages/*.json`; `check-portal-languages.py` is its check |
| `python build/generate-settings-defaults.py [--check]` | `Common/Application/SettingsDefaults.h` | The create scripts and `EnsureSettingDefaults_` |
| `python build/generate-profiles.py [--check]` | `Common/Application/ProfilesData.cpp`, and the tables in `hmailserver/docs/Profiles.md` | `Common/Application/Profiles/*.json` |
| `python build/generate-idna-tables.py [--check]` | `Common/Idna/IdnaTables.cpp` and `Common/Idna/IdnaTestData.cpp` | Unicode's data files under `Common/Idna/ucd`, each held to the SHA-256 and Unicode version the generator pins |
| `python build/generate-html-references.py [--check]` | `Common/AntiSpam/HtmlReferencesData.cpp` | HTML's list of named character references, `Common/AntiSpam/whatwg/entities.json`, held to the SHA-256 the generator pins |
| `python build/generate-auth-matrix.py [--check]` | `hmailserver/docs/WebAuthorisationMatrix.md` | The route parser (`ParseRoute_` and the three parsers it calls), `Authorize_` and the functions whose lists it reads, the built-in roles' permissions in `RoleManager.cpp` (the permission lines of their functions) and the permissions and route kinds of `RoleManager.h`'s and `RestApiServer.h`'s enumerations, each one's value its place, the report's sections counted from server-wide metrics in `RestApiReports.cpp`, and the two listeners' anonymous routes. The steps of `Authorize_`, of the functions whose lists it reads, of the built-in roles' functions, the `Bit` they set a permission with and `RoleManager::BuiltInPermissions_`, of `RoleManager::Allows` and the `RoleGrant::Holds` and `RoleManager::IsDomainReached_` it asks, of `IsDomainAllowed_`, of `IsServerWideReportSection_` and the `FindSection` it asks, and of `ProcessRequest_` between the credential and the handler are held to its `STEPS`: one changed stops it until `decide()` has been read against it and `STEPS` updated (`--steps` prints them), and a definition it reads that stands inside a conditional of the preprocessor, or holds one, or is not the only definition of its name in its file, stops it too, as do a directive of the preprocessor of any other kind inside what it reads - an `#include`, which brings in code it never sees - and an enumerator whose value is not its place. It reads the code as the compiler's first three translation phases do: what a comment or a literal holds is not read, a directive is one wherever it begins a line, and a form it does not read as the compilers do, such as a backslash with spaces after it at a line's end, stops it. `Authenticate_` and the grants `RoleManager` makes for it, the string helpers, a macro defined elsewhere over a name those functions use, an overload of one of them defined in another file and the functions `ProcessRequest_`'s other checks call are not held. `--self-test` breaks the source in memory and proves each flag still fires and each changed step stops it |
| `build/generate-settings-index.ps1` (either PowerShell) | `ControlPanel/Services/SettingsSearchIndex.g.cs` | The Control Panel's settings pages; check with `git diff` afterwards |
| `python build/add-license-headers.py [--check]` | The licence header on every source file | See [below](#code-style-and-licence-headers) |

`build/generate-public-suffix-list.ps1` regenerates `PublicSuffixListData.h` and has no check mode.
`python build/find-stale-roadmap-rows.py` is an audit, not a gate: it lists not-started roadmap rows that name
something which already exists.

### PowerShell, Node and shell checks

| Check | What it holds | How to run it |
|---|---|---|
| `build\check-schema-versions.ps1` | The five lists that describe the schema chain agree: `REQUIRED_DB_VERSION`, DBUpdater's `LoadScripts` and `GetDatabaseVersionName`, four upgrade scripts per step, and a probe for every step whose `ALTER` is marked `[IGNORE-ERRORS]`; and the Schema row of README.md's Technology table states `REQUIRED_DB_VERSION` | **PowerShell 7 (`pwsh`)**; it dies under 5.1. `-SelfTest` checks the checks |
| `build\check-db-scripts.ps1` | A throwaway SQL CE database builds from `CreateTablesMSSQL.sql` with the server's own splitting rules, every DBUpdater probe succeeds against it (and a negative control fails), and every Compact upgrade script holds one statement per command | **Windows PowerShell 5.1 (`powershell.exe`)**; SQL CE's native components will not load under PowerShell 7. Needs SQL Server Compact 4.0 installed, and skips itself without it |
| `build\check-roadmap.ps1` | `Roadmap.md`'s counts agree with its tick boxes | `pwsh`. After any edit to `Roadmap.md` |
| `build\test-get-dotnet-runtime.ps1` | `get-dotnet-runtime.ps1` places the pinned .NET Desktop Runtime and nothing else: a file whose SHA-512 is not the pinned one is refused, on disk or downloaded, and a pin that is not one (a moving link, no SHA-512, another channel) is refused | Either PowerShell, and no network: a loopback listener serves the downloads. After any change to the script or to the pin |
| `node build/check-portal-smime.js` | The webmail's S/MIME module against OpenSSL, both ways | Node 20 or later and `openssl` on `PATH` |
| `node build/check-portal-openpgp.js` | The webmail's OpenPGP module against GnuPG's fixtures, and back to `gpg` when it is present | Node 20 or later; `gpg` optional |
| `bash build/check-linux-version-stamp.sh` | The Linux stamps (the PKGBUILD's and the APKBUILD's `pkgver`, the source packages' versions) agree with `Version.h` | Git Bash, WSL or Linux |
| `editorconfig-checker` | `.editorconfig` on every text file, with the exclusions in `.editorconfig-checker.json` | `ec -config .editorconfig-checker.json`. CI uses Alpine's package, 3.0.3, which reads the configuration only when named; the GitHub workflow used v3.11.3 |
| `dotnet format --verify-no-changes` | C# formatting, on `"hmailserver/source/Tools/hMailServer Tools.sln"` and on `hmailserver/source/Tools/ControlPanel/ControlPanel.csproj` | Windows; the .NET 10 SDK. CI runs it in *windows-dotnet* |

### Which ones for which change

| You changed | Run at least |
|---|---|
| Anything | `add-license-headers.py --check`, `check-project-items.py`, `check-control-bytes.py` |
| C++ in the server | The server build (`/WX`); `check-collection-iteration.py`, `check-audit-choke-point.py`, `check-role-choke-point.py`, `check-authz-choke-point.py`, `check-com-dispids.py`, `check-com-error-info.py`, `check-idl-case.py`, `check-password-choice-sites.py`, `check-setting-levels.py`, `check-address-parsing.py`, `check-source-literals.py` where they apply; the Linux build or census if the file is in the Linux core |
| SQL or the schema | `check-schema-versions.ps1`, `check-db-scripts.ps1`, `check-mysql-charset.py`, `check-sql-separators.py`, `check-sql-parameters.py`, `check-upgrade-inserts.py`, `check-settings-rows.py`, `check-account-stores.py`, `generate-settings-defaults.py --check` |
| A REST route | `check-role-choke-point.py`, `check-openapi-blocks.py`, `check-audit-choke-point.py`, `check-deck-parity.py`, `generate-auth-matrix.py` (and commit the matrix it writes; if it stops on a changed step, read `decide()` against the step and update `STEPS`), `check-cli.py` if a client verb changed |
| The Control Panel | Its build and tests, `check-mnemonics.py`, `check-localisation.py`, `check-catalogues.py`, `check-ini-coverage.py`, `check-db-settings-coverage.py`, `check-help-map.py`, `check-spacing.py`, `generate-settings-index.ps1`, `dotnet format` |
| The Control Deck | `check-deck-script.py`, `check-deck-languages.py`, `check-help-map.py`, `check-deck-parity.py` |
| The webmail | `generate-portal-page.py --check`, `check-portal-script.py`, `check-portal-languages.py`, `check-help-map.py`; the S/MIME and OpenPGP pairs if those modules changed |
| The Windows installer | `check-installer-scripts.py`; then an installer build and the installer smoke test ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md#the-installer-smoke-test)) |
| `hmailserver/docs/` | `generate-help-pages.py --check`, `check-help-map.py`, `check-environment-names.py` |
| A setting | `check-ini-coverage.py`, `check-db-settings-coverage.py`, `check-settings-rows.py`, `generate-settings-defaults.py --check`, `check-setting-levels.py`, `check-environment-names.py`, `check-test-settings-writes.py` |
| `Roadmap2.md`, `Roadmap3.md` or `Roadmap.md` | `check-roadmap2.py`, `check-roadmap.ps1` |

## Code style and licence headers

* `.editorconfig`: spaces, three to an indent, in `.h`, `.cpp` and `.cs`. `editorconfig-checker` also holds the
  final newline and trailing whitespace; its indent-size rule is off on purpose, because `dotnet format`'s
  continuation lines do not fall on multiples of three.
* C++: warning level 3 with warnings as errors (`/WX`) in Debug and Release x64. Release adds the
  reproducibility flags `/Brepro`, `/d1trimfile` and `/pdbaltpath`, and two clean Release builds of the same
  tree, and one from a second checkout at another path, must produce the same `hMailServer.exe` (RELEASE.md
  step 8; [ReproducibleBuilds.md](hmailserver/docs/ReproducibleBuilds.md)). The Linux build is C++20.
* C#: every shipped project is `net10.0-windows` with nullable reference types off. `dotnet format` is the
  formatter for the two targets named above.
* **Use parameterised SQL exclusively — never build SQL strings manually.** Backend DDL differences go through
  `SQL/Macros/`.
* **The licence header.** Every source file opens with three lines in its own comment syntax:

  ```
  https://www.progressiverobot.com
  Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
  SPDX-License-Identifier: AGPL-3.0-or-later
  ```

  `python build/add-license-headers.py` puts it there (after a byte-order mark, a shebang or an XML
  declaration, which have to come first) and `--check` reports what it would change. Markdown, `.resx` and the
  packaged configuration files have their header made canonical when they carry one and are left alone when
  they do not. Generated MIDL output and vendored third-party sources are skipped.

## Change checklists

### A schema step

The current example is 39d74047a, schema 6095 to 6096.

* `REQUIRED_DB_VERSION` in `hmailserver/source/Server/Common/Application/Constants.h`.
* Four upgrade scripts in `hmailserver/source/DBScripts/`: `Upgrade<from>to<to>MSSQL.sql`, `…MSSQLCE.sql`,
  `…MySQL.sql` and `…PGSQL.sql`.
* The three create scripts, `CreateTablesMSSQL.sql` (also used for SQL CE), `CreateTablesMySQL.sql` and
  `CreateTablesPGSQL.sql`, each with the new `insert into hm_dbversion values (<n>)`.
* DBUpdater's `formMain.cs`: `_upgradeScripts.Add(new UpgradeScript(<from>, <to>))` in `LoadScripts`, and a
  `case` in `GetDatabaseVersionName`.
* A probe in `hmailserver/source/Tools/DBUpdater/SchemaVerification.cs`. `[IGNORE-ERRORS]` on an `ALTER`
  discards every error while the version stamp still runs, so every marked step needs its probe.
* The SQL text rules: the script parser splits on a blank line (PostgreSQL on `;` and a blank line), so every
  statement ends with one. An upgrade inserts a setting row only when it is absent.
* A new per-account table goes into the backup, or is excused (`check-account-stores.py`), and is audited, or
  excused (`check-audit-choke-point.py`).
* The persistence side: the business object, its persistence class, and the cache if the read is hot.
* Run `check-schema-versions.ps1` (pwsh) and `check-db-scripts.ps1` (powershell.exe), and the SQL checkers.
* Move your bench forward with `build\upgrade-test-database.ps1` **before** you build the new server.
* Say the step in the commit message ("schema 6095 to 6096").

### A COM or IDL change

Seven places, as the grey-listing bypass coclass (f7a19f62e) shows:

1. `hmailserver/source/Server/hMailServer/hMailServer.idl`: the forward declaration, the interface, the coclass,
   and the property on the parent interface.
2. A fresh GUID for each new interface and coclass.
3. An `.rgs` file per coclass in `hmailserver/source/Server/COM/`.
4. An `IDR_INTERFACE…` id in `hMailServer/resource.h`.
5. A `REGISTRY` line in `hMailServer/hMailServer.rc`.
6. The `ClCompile`, `ClInclude` and `None` entries in `hMailServer.vcxproj` (there is no `.filters` file).
7. `OBJECT_ENTRY_AUTO` at the foot of the interface's header.

Then:

* Implement every member you declare: a declared, unimplemented property makes the class abstract.
* `Release`, `AddRef` and `QueryInterface` cannot be member names.
* `check-com-dispids.py` (find the interface's highest dispatch id first), `check-com-error-info.py`,
  `check-idl-case.py` (a name must not be spelled like one already in the IDL in another case), and
  `check-audit-choke-point.py` for a COM write.
* The COM wrapper regenerates itself; build the server first, then the tools, so it is made from the new type
  library.
* A new interface needs `build\register-com.ps1`, elevated, before the regression suite can reach it.

### A new setting goes in the database

The rule, from 15 September 2026: **a setting belongs in the database, not in `hMailServer.ini`.**

Put it in `hm_settings` - the store the COM `Settings` object and the Control Panel's classic pages have always
used, reached through `Property` and `PropertySet` - and give it a Control Panel editor. In the server that means
a `PROPERTY_…` name in `Common/Application/Constants.h`, its default row on existing databases from
`Configuration::EnsureSettingDefaults_` or an upgrade script's insert (`check-settings-rows.py`), and
`SettingsDefaults.h` regenerated (`generate-settings-defaults.py`). Do not add a key to the `[Settings]` section:
`build/check-ini-coverage.py` compares every key the server reads against `build/ini-settings-baseline.txt` and
fails if a new one appears there. `build/check-db-settings-coverage.py` fails when a database setting has no Control
Panel editor: a COM property the Control Panel writes, or, for a setting the server reaches over the REST API
alone, a REST row on a settings page (`Views/ServerSettingsView.xaml.cs`, `new RestText { Key = "...", Group = ... }`).
A setting that should have no editor goes in its `EXEMPT` list, with the reason.

The file keeps only what is needed to reach the database, or to be let in without one: `[Directories]`,
`[Database]`, `[Security]`'s administrator password and second-factor secret, and `[GUILanguages]`, which
`Languages::Load` reads before the database is opened. If something genuinely has to be readable before the
database is open, that is the exception: add it to one of those sections, add the line to
`build/ini-settings-baseline.txt` in the same commit - the check names the file when it fails - say so in the
commit message, and expect to be asked.

Why, in one line each:

* Two nodes cannot share a file, and shared settings are what an active-active pair needs.
* A configuration backup that does not include the file is not a backup of the configuration.
* A Control Panel or a Control Deck on another machine has no share to that file.
* A change to a file setting is saved now and applied at the next service start; a change in the database can
  be published to the running server.

The 238 keys that were in the file were migrated at schema 6042 (`Roadmap2.md`, section 13); three
`PasswordHash…` keys were added to the baseline the next day, so it holds 241 `[Settings]` keys. They are read
from and written to `hm_inisettings`, and the `[Settings]` section of `hMailServer.ini` is now a CACHE the server
rewrites at every start, kept complete because `hMailServer.exe /Register` reads the service account from it with
no database open at all. Three consequences worth knowing before you touch this area:

* **Editing `[Settings]` in the file changes nothing.** The stored value is used, the edit is named in the
  ERROR log (`ERROR_hmailserver_<date>.log`) as HM5804, and the line is put back. A test that needs to change a
  setting goes through `Settings.SetIniSetting` over COM - `RegressionTests`' own `IniFileSetting.Write` does.
* **Deleting a line no longer returns a setting to its default.** `DeleteIniSetting`,
  `DELETE /api/v1/settings/ini/{name}` and the Control Panel do, by dropping the row and the line together.
* **`[SettingsOverride]` is the door**, for a database that is unreachable or holds a value the server will not
  start on. A key there is applied over the store and announced by name in the error log at every start.
  Nothing writes that section but a person.

Six `[Settings]` keys are the deliberate exception, read from the file alone and never from the database, each
for a reason of security. `PasswordPepper` (`IniSettingStore::IsFileOnlyName`) protects the password hashes only
while it is somewhere the database and its backups are not. The live update's trust - `UpdateSigningIdentity`,
`UpdateSigningIssuer`, `UpdateSourceRepository`, `UpdateTrustRootsFile` and `UpdateLogPublicKeyFile`
(`IniSettingStore::IsTrustAnchorName`, from 6.3.6) - is what an update is checked against, and the process an
update is checked for must not choose it: on Linux, root checks every package again with `hmailserver
--verify-update`, which opens no database and reads the file, and the server has to read the same values or the
two checks disagree. COM and the REST API refuse a change to those five, a row an earlier version stored is
taken out at the next start (`HM5809`), and `[SettingsOverride]` does not apply to them
(`hmailserver/docs/UpdateFeed.md`, "Where the trust is read from"). A new key joins them only for a reason of
that weight, said in the commit message.

`IniFileSettings` is the seam readers go through, with `IniSettingStore` behind it as the only class that
touches the file as a file. Three sections are still read around it - `[LDAP]`, `[SendingLimits]` and
`[SendingLimitsOverrides]` - and `build/check-ini-coverage.py` records those three and fails on any other.

And, for any new setting:

* A setting that a domain or an account may also hold is read through `EffectiveSetting` and marked in
  `build/settings-levels.txt` (`check-setting-levels.py`; `hmailserver/docs/SettingLevels.md`).
* It has a container name, `HM_PROPERTY_<name>` or `HM_SETTING_<key>`, and any document that names one must name
  a real setting (`check-environment-names.py`).
* Regenerate the Control Panel's settings search index (`build/generate-settings-index.ps1`).

### A REST route

* A value in the `RouteKind` enum in `Common/Util/RestApiServer.h`, parsed in `ParseRoute_`.
* Classified for authorisation in `PermissionFor_`, `IsSelfServiceRoute_` or `IsSessionRoute_`, with
  `IsMutatingRoute_` for a write and `SecondPermissionFor_` where one permission is not enough.
  `RoleManager` decides, and `check-role-choke-point.py` holds it.
* Its entry in the served OpenAPI document: an `OpenApi<Feature>Paths_()` block, joined with exactly one comma
  (`check-openapi-blocks.py`).
* A write reaches the audit trail through the actor `RestApiServer::ProcessRequest_` installs
  (`check-audit-choke-point.py`).
* A route that sets a password asks the password policy and the breach check (`check-password-choice-sites.py`).
* If `hmctl` or the PowerShell module gains a verb, `check-cli.py`, and `check-cli-packages.py`: a function the
  module exports goes into `HmailServer.psd1`'s `FunctionsToExport` too. If the Deck reaches the property, run
  `check-deck-parity.py` and commit `DeckParity.md`.

### The Control Panel and its languages

* Every static caption carries an Alt-key mnemonic (`_Save`), with no clash on one screen
  (`check-mnemonics.py`).
* Mark a caption for translation where it is used: `L("_Save changes")`, `F(...)` or `N(...)` in C#,
  `{loc:L '_Save changes'}` in XAML. Then `python build/check-localisation.py --write` regenerates
  `Resources/Strings.resx`, and the caption is translated into all 17 complete languages (cs, da, de, es, fi,
  fr, it, ja, nb, nl, pl, pt-BR, ru, sv, tr, uk, zh-Hans). An unmarked or untranslated caption fails
  `check-localisation.py` - one word ending a sentence (`"Copied."`) and a piece of one joined to others
  (`"in " + page`) are captions too: write the whole sentence, `F("in {0}", page)`, so that a translator can
  order it. The check also reports a sentence split around a value with its end left a literal
  (`L("... followed by @") + domain + "."`): `F("... followed by @{0}.", domain)` lets the translator place
  the value. A translation keeps the space at an edge of its English, where the source joins it to another
  text, and adds none where it has none; `check-catalogues.py` holds the literals, page names and numbers a
  translation must keep. The English text is the key, so changing an English caption is a new key in all
  eighteen catalogues.
* A catalogue's style note, where it has one, is the comment at its head: the Japanese one says キー for a key,
  送信者 for the sender, スパム, 自動 BAN, half-width parentheses and the key's suffix written against its caption
  (`JapaneseStyleTests` holds it, and `check-mnemonics.py` refuses a space before a Japanese or Chinese key's
  suffix). The English is British: behaviour, honour, organisation, reinitialise (`BritishSpellingTests`).
* Every `[Settings]` key the server reads has an editor or a recorded reason for none (`check-ini-coverage.py`), so does
  every database setting (`check-db-settings-coverage.py`), and folder-access decisions stay in `ACLManager`
  (`check-authz-choke-point.py`).
* A new page has a help entry in `Services/HelpMap.cs`, keyed by `Services/NavigationMap.cs`
  (`check-help-map.py`), and the settings search index is regenerated.
* Build with `-warnaserror`, run `ControlPanel.Tests`, and run `dotnet format --verify-no-changes`.

### The Control Deck and the webmail

* **The Deck** is one file, `hmailserver/installation/WebAdmin/index.html`, with its catalogues in
  `WebAdmin/languages/*.json` (`check-deck-languages.py`). `check-deck-script.py` runs its script in Node.
  `build.ps1` copies the page and the catalogues beside the server it builds.
* **The webmail** is the `Portal*` files in `hmailserver/source/Server/Common/Util/`. The page and its script
  are embedded in the server by `generate-portal-page.py` (commit `PortalPageData.cpp`) and run by
  `check-portal-script.py`. Its catalogues are `PortalLanguages/*.json`, embedded by
  `generate-portal-languages.py` and checked by `check-portal-languages.py`. The S/MIME and OpenPGP modules have
  their own generator and their own Node check each.
* A new page or view on either needs its help entry: the Deck's `HELP_MAP` in `index.html`, the webmail's
  `HELP_MAP` in `Portal.js` (`check-help-map.py`).
* The Deck's parity with the desktop program is measured by `check-deck-parity.py`, which rewrites
  `hmailserver/docs/DeckParity.md`.
* A page is judged by looking at it as well: see
  [Looking at the two browser pages](#looking-at-the-two-browser-pages).

### Documentation and the help pages

* Operator documents live in `hmailserver/docs/` and are listed in `hmailserver/docs/README.md`.
* **Every document there except `README.md`, `DeckParity.md`, `WebAuthorisationMatrix.md` and `WebThreatModel.md` is
  compiled into the server** as a help page.
  After any change to one, run `python build/generate-help-pages.py` and commit
  `hmailserver/source/Server/Common/Util/HelpPagesData.cpp` with it. A docs change is therefore a server change,
  and rides a build.
* A new document must be reachable: named by one of the three help maps, or listed in `ReferenceOnly` in
  `HelpMap.cs`. A heading a help map names must be a level-one or level-two heading (`check-help-map.py`).
* The documents at the repository root, `hmailserver/docs/README.md` and the wiki are not compiled into
  anything. The wiki copies several of the root documents; a change to one of those is a wiki change too.
* House style: British English, plain declarative prose, and every claim true of the tree it describes. Nothing
  after 6.3.7 is written with a version number until it is released: it is "in the tree after 6.3.7, not yet
  released".

### Roadmap2 and Roadmap3 rows

* The legend is `Roadmap.md`'s: ✅ done and verified, 🔄 underway, ⬜ not started, ⏸️ deferred with the reason
  written down. A row is counted by the mark in its first cell.
* `Roadmap2.md` and `Roadmap3.md` each open with a line **Where it stands**, counted from that file's rows. A
  change that flips a row in either runs `python build/check-roadmap2.py --write` and commits the line with the
  row.
* A finished row says so at the start of its text - `**Done 15 September 2026.**`, or `**Built ...**` with what
  is still to be proven - and then says what was built.
* `Roadmap.md` is the record and is changed rarely; after any edit to it, run `build\check-roadmap.ps1` under
  `pwsh`.

## Commits

As practised on `master`:

* **One logical change per commit.** A Roadmap2 row, or two built together, lands as one commit, followed by
  separate commits for what a review or the regression gate found.
* **The first line is a whole paragraph**, not a short subject: what changed and why it matters, naming the
  Roadmap2 row and the schema step where there is one - "SCIM provisioning - the Roadmap2 row, schema 6095 to
  6096." and then what it does. The body is prose: what was built, why it is built that way, what the review
  found, and how it was proven - the tests, the checkers and the gate run.
* **Say what you ran.** GitLab CI records the checks it runs; for the builds, the suites and the checks run by
  hand, the commit message and the merge request are the only record of what passed.
* A commit that adds a key to a bootstrap section of `hMailServer.ini` says so, and why, in its message.
* A commit that flips a Roadmap2 or Roadmap3 row carries that file's counter line with it.
* **Every commit is signed off**, `git commit -s`, the maintainers' included ([Sign-off](#sign-off)).
* The history is linear: `master` has had no merge commit since 15 September 2026.
* Commits on `master` are not signed. Release **tags** from `v6.2.23-alpha2` on are annotated and SSH-signed
  with a key listed in `.gitlab/allowed_signers`.
* Numbers and names follow [the convention above](#issue-numbers-from-github-and-from-gitlab).

## Secrets

Nothing that opens anything - a token, a password, a private key that is not a throwaway test fixture - goes
into a commit. Three things stand in the way, as GitLab's push protection is not available on the Free tier:

* **The hooks.** Turn them on once in your clone:

  ```
  git config core.hooksPath build/hooks
  ```

  `build/hooks/pre-commit` scans the lines a commit adds, and `build/hooks/pre-push` the commits a push brings,
  with `build/scan-secrets.py`. It runs gitleaks from the image the `secret_detection` job of your working tree
  pins, with GitLab's rules and the repository's `.gitleaksignore`, in Docker when Docker runs on your machine;
  without Docker, a
  gitleaks 8 on `PATH` (or named by `HM_GITLEAKS`), with gitleaks' own rules, which are close to GitLab's but not
  the same; or `HM_SECRET_SCAN_SSH` pointing at a machine with Docker that sees your
  repository's git directory (`user@host:/path/to/.git`, with `HM_SECRET_SCAN_SSH_KEY` for the key). With none
  of them the hooks refuse the commit or the push, because a scan that did not run has not said anything is
  clean. `python build/scan-secrets.py --range master..HEAD` scans a branch by hand. The hooks, the landing's
  scan and the job read every path under a directory named `build`, `bin`, `obj`, `target` or `packages` too,
  this tree's `build/` among them, which GitLab's own rules leave out: they run GitLab's rules but for the two
  entries of their global allowlist that leave those directories out, as `build/ci/secret-detection-rules.sh`
  derives them from GitLab's own
  ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md#what-the-secret-scans-read)).
  gitleaks' inline allow marker (the word `gitleaks`, then `:allow`, on the secret's line) waives nothing: the
  hooks and the landing report such a line as any other, and the `checkers` job fails a tree that carries it.
* **`.gitignore`** leaves out the usual key and credential files (`*.pem`, `*.key`, `*.pfx`, `*.p12`, `*.jks`,
  SSH private keys, `.env`, `.netrc`, `.npmrc`, `.pypirc`, `credentials.json`), but for the directories of the
  test fixtures. Adding one elsewhere takes `git add -f`, which is the moment to ask whether it should be there.
* **The pipeline.** The `secret_detection` job fails the pipeline of a merge request, or of a push to `master`, on
  a finding `.gitleaksignore` does not name (not of a push to another branch: open a merge request, or turn the
  hooks on); and the maintainer's landing scans every commit of a batch before it reaches `master`. A batch's own
  pipeline does not run the job, and `master`'s runs it only once the batch has landed, so the landing's scan is
  what reads a batch's commits before `master` gets them.

A finding that is a real secret is revoked where it was issued, and taken out of the commits before they are
pushed; once pushed, assume it is known ([SECURITY.md](SECURITY.md#secrets-and-credentials)). A finding that is
not a secret - a key made for a test and used for nothing else, an example key from a standard - is named in
`.gitleaksignore` by the fingerprint the range scan prints (`<commit>:<file>:<rule>:<line>`), in a commit that
says what it is; a staged finding has no commit yet, so commit it with `--no-verify` after reading it and add the
line the pre-push scan then prints. `.gitleaksignore` is the owner's to accept (`.gitlab/CODEOWNERS`). A
throwaway key is better generated by the test at run time than committed.

## Sign-off

Commit with `git commit -s`. That adds a `Signed-off-by: Your Name <you@example.com>` trailer, which is your
statement that you wrote the change, or have the right to submit it, and that it may be distributed under this
project's licence: the [Developer Certificate of Origin](https://developercertificate.org/). The trailer's email
must be the commit's author email. There is no CLA, so the sign-off is the record that a contribution may be
distributed under AGPL-3.0-or-later.

**Sign-off is required of every commit that reaches `master`, the maintainers' own included**, since the first
batch that landed through `build/land-via-mr.py`, `batch41`, on 29 September 2026. Before it
the maintainers' commits were exempt, as they were under the GitHub check (the workflow `dco.yml`, in the git
history at `v6.3.3`), which is why the commits on `master` before it carry none. A direct push to `master`,
which Maintainers can make until its push rule becomes *No one*, is checked by nothing
([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), "Landing on master"). Two things hold
it, and both ask the same question through `build/sign-off.py`:

* **GitLab CI.** The *sign-off* job runs in every merge request's pipeline - a fork's, and a branch of this
  project's - and in the pipeline of every `batch*` branch, which stands for the merge request a batch lands
  through (a merge request from a `batch*` branch runs no pipeline of its own). It fails unless every commit the
  merge request or the batch adds to `master` carries a `Signed-off-by` trailer with the commit's author email
  (compared without regard to case). For a `batch*` branch it leaves out the commits the project's own `v*`
  release tags already hold, which a trailer could be added to only by rewriting a release; a merge request's
  range exempts nothing.
* **The landing script.** `build/land-via-mr.py` refuses to push a batch with a commit that lacks it
  ([Merge requests](#merge-requests)).

A commit that lacks it is fixed with `git commit --amend -s` (or `git rebase --signoff master` for several) and
a push. Only sign off what you wrote or may submit: `git rebase --signoff` adds *your* name to every commit it
rewrites. To sign off without remembering the flag, `git config alias.cs "commit -s"` and commit with `git cs`.

## Merge requests

A change lands on `master` only through a GitLab merge request ([GOVERNANCE.md](GOVERNANCE.md)). Since 18 October
2026 a pull request on GitHub, <https://github.com/RainbowWizard-coder/hMailServer/pulls>, is accepted as well: a
maintainer lands it through GitLab, as a `batch*` branch and a merge request, keeping your authorship and your
sign-off. The steps below are a merge request's; a pull request is held to the same - one logical change, every
commit signed off, the checks you ran named in its description, a review, and the gate on the batch - and, as no
pipeline of the project's runs on GitHub, none of the project's checks runs for it until a maintainer puts it on a
batch, so run them yourself before you open it. GitHub fills a new pull request's description from
`.github/PULL_REQUEST_TEMPLATE.md`, which carries the merge-request template's checklist whole and says how a pull
request is landed.

1. **Fork** the project on GitLab and clone your fork.
2. **Branch from the current `master`.** There are no version branches: releases are tags such as `v6.3.3`, and
   a fix goes to `master` and ships in the next release.
3. **Keep it to one logical change.** Add or update regression tests for a change of behaviour. Say in the
   description which checks you ran and on what; the merge-request template asks.
4. **Open the merge request against `master`** of `hmailserver/hmailserver`, and sign off every commit.
5. **Review.** A maintainer other than the author reviews the change - by the rule Zain Ul Abidin, the second
   maintainer, or, for a merge request of his own, the owner - asks for changes in threads on the merge request,
   and presses **Approve** when it is right. GitLab will not merge a merge request with an unresolved thread or a
   pipeline that has not passed (both are project settings). From the first change after 6.3.4 no change lands
   without that approval, the maintainers' own included, unless the owner overrides it for one landing
   ([GOVERNANCE.md](GOVERNANCE.md), "Review"). Since 13 October 2026 Zain Ul Abidin holds no role in the project,
   so the owner reviews a contributor's merge request, and the batches land on his override until another person
   can approve. No approval rule is configured on the project, so the landing script below holds the rule, and it
   never counts an author's approval.
6. **The gate.** A merge request's own pipeline runs the checks only, the change checks among them, on GitLab's
   hosted runners, and they must pass ([The checkers](#the-checkers)). A merge request from your fork runs that
   pipeline in your fork, with your fork's CI files and on runners your fork's settings choose, so its result tells
   you how the change fares and is not the gate: the gate is the pipeline of the `batch*` branch your commits land
   on, or one a maintainer runs for your merge request in this project after reading its CI files
   ([SECURITY.md](SECURITY.md), "Every change"), which runs on GitLab's hosted runners on the project's compute
   minutes, and cannot run while those are spent. The builds and both suites run on the project's machines for a
   `batch*` branch, which only a Maintainer can push: the maintainer puts your commits on one - rebased onto the
   current `master`, your authorship and your `Signed-off-by` kept, or squashed into one commit of yours that keeps
   them - with other changes ready to land.
7. **Landing.** A `batch*` branch lands through a merge request of its own, by fast-forward, so history stays
   linear: `python3 build/land-via-mr.py batchN`, run from a checkout of `master`, checks that the batch is on top
   of `master`, adds no merge commit, is signed off throughout and holds no possible secret (`build/scan-secrets.py`
   over every commit it adds, which must run: see [Secrets](#secrets); the checkout's own copy of it and of its
   rules script, in the analyzer image the checkout's `secret_detection` names, so that the batch chooses neither
   what scans it nor how - and it refuses to run from a checkout whose copies of those files, of itself or of
   `build/sign-off.py` are not the remote `master`'s, working tree and all, but for a batch that repairs them, which
   a maintainer lands from its own worktree with `--own-copies "<who, why>"`, recorded), pushes it, opens the merge
   request, waits for the branch's pipeline, and
   merges only when it passed, with each of its gate jobs - *semgrep-sast*, *semgrep-sast-gate*,
   *mr-dependency-check* and *checkers*, and *static-analysis* and *semgrep* unless the batch changes only Markdown
   at the root or under a dot-directory - succeeding itself, in a pipeline of this project whose gates each say in
   their log that
   they judged the batch against `master`'s tip, and GitLab calls the merge request mergeable; a batch that changes
   the gates' code also needs each gate's log to say that `master`'s copy of its script passed it. A batch that
   changes the gates' own files (their scripts; `.gitlab-ci.yml` and the CI files their jobs are in or take a part
   from; the landing script, `build/check-release-chain.py`, `build/sign-off.py` and `build/scan-secrets.py`,
   which the landing runs, and `build/ci/secret-detection-rules.sh`, which makes both secret scans' rules; and the
   source scans' scripts, `build/ci/static-analysis.sh` and `build/ci/semgrep.sh`)
   lands only with `--gates-read "<who read it, and what>"`, a person's word that the change
   was read whole, which is recorded (exit 9 without it). It never squashes. It
   records on the merge request, and in `land-via-mr.log` in the git directory, the pipeline and its gate jobs, the
   sign-off and whether somebody other than the author approved the tip it lands (an approval given before a later
   push does not count), and without such an approval it refuses to merge (exit 7). The owner alone may override
   that for one landing that cannot wait - a security fix inside its disclosure window, a broken `master` - with
   `--land-without-approval "<reason>"`: the script refuses the override unless it runs as one of the owner's
   accounts, records the reason on the merge request and in its log, and the change is reviewed afterwards. Run with
   `--dry-run` it checks and changes nothing, with `--no-wait` it stops once the merge request is open, and run
   again it finds the open merge request and carries on. Your merge request is then merged or closed with a link to
   the batch's. `master` is protected: only the owner can merge to it, and once the batch that brings the change
   checks has landed through them it is to take no direct push ([SECURITY.md](SECURITY.md), "Every change"), so
   that a merge request is the only way in; that batch landed on 9 October 2026, and on 14 October 2026 `master`
   still let Maintainers push. (On GitHub, until 14 September 2026, a pull request was merged by rebase once
   the other maintainer had approved it, as the master ruleset required; from then until 6.3.4 no approval was
   required.)

If a change spans more than about three rows of ARCHITECTURE.md's *Where to start* table, or touches the schema,
the COM interface or the release path, open an issue to discuss it before writing it.

## Governance, conduct and licence

* [GOVERNANCE.md](GOVERNANCE.md) says who decides what. Decisions are made by the maintainers, in the open, on
  the issue tracker, with a reason written down.
* The [Code of Conduct](CODE_OF_CONDUCT.md) is the Contributor Covenant 2.1. It applies on the GitLab project -
  issues, merge requests, code review, commit messages and the wiki - and, since 18 October 2026, on the GitHub
  repository - pull requests, discussions and code review - and names how to report a problem.
* **Licence.** hMailServer is licensed under the [GNU Affero General Public License](LICENSE), version 3 or
  later (`SPDX-License-Identifier: AGPL-3.0-or-later` in every header). By contributing you agree that your
  contribution is licensed the same way. There is no CLA. Third-party components' licences are in
  [hmailserver/docs/Licenses](hmailserver/docs/Licenses), and the provenance of every third-party binary the
  build uses is in [hmailserver/docs/ThirdPartyBinaries.md](hmailserver/docs/ThirdPartyBinaries.md).

## Architecture

The layering is BO → Persistence → SQL, with Cache in front of the hot reads. All configuration and management
goes through the COM API in `Server/COM/` — that is the seam the Control Panel, the test suite and external
scripts all use; the REST API in `Common/Util/RestApiServer.cpp` is the same administration over HTTP, and the
Deck, the webmail, `hmctl` and the Linux suite use it. Networking is Boost.Asio, wrapped by
`Server/Common/TCPIP/`. [ARCHITECTURE.md](ARCHITECTURE.md) has the full map.

## Looking at the two browser pages

The webmail (`/portal`) and the browser Control Deck (`/`) are pages a person uses, and a page is judged by
looking at it. `.mcp.json` in the repository root declares one Model Context Protocol server for that:
**Playwright** (`@playwright/mcp`, pinned), which drives a real Chromium against a running server so an agent -
or a contributor pairing with one - can open a view, sign in, click through a flow, take a screenshot and read
the accessibility tree rather than guess from the source. It is optional: nothing in the build or the tests uses
it, and a checkout that never starts it behaves exactly as before.

Point it at a server you are already running (`https://localhost:8045/portal` on the bench). Nothing else is
declared, deliberately:

* **A design-tool server** (Figma and the like) would need designs in that tool, and this project has none - the
  desktop console's design system is `hmailserver/docs/ControlPanelDesign.md` and the pages are hand-written.
* **Component-library servers** (shadcn, Tailwind and friends) describe a stack this project does not use: the
  webmail and the Deck are plain markup, a stylesheet and hand-written script - the webmail embedded in the
  binary, the Deck installed beside it - and the Control Panel is WPF.
* **A third-party accessibility server** would put an unvetted npm package inside the repository with an agent
  driving it. Accessibility belongs in a check this project owns and runs, which is a roadmap row rather than a
  convenience.

The repository's own harnesses stay the authority on behaviour: `build/check-portal-script.py` and
`build/check-deck-script.py` run each page's script against a small DOM and a recorded server, with no browser
and no network. Run them before a merge request that touches either page. A browser is for what a harness cannot
see - how it looks, and whether it can be used.
